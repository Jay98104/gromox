// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2021 grammm GmbH
// This file is part of Gromox.
#include <algorithm>
#include <list>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/exmdb_rpc.hpp>
#include <gromox/socket.h>
#include <gromox/svc_common.h>
#include "exmdb_client.h"
#include "exmdb_server.h"
#include "common_util.h"
#include <gromox/double_list.hpp>
#include <gromox/list_file.hpp>
#include "exmdb_ext.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <poll.h>

struct REMOTE_CONN;
struct REMOTE_SVR {
	std::list<REMOTE_CONN> conn_list;
	std::string host, prefix;
	BOOL b_private;
	uint16_t port;
};

struct REMOTE_CONN {
	time_t last_time;
	REMOTE_SVR *psvr;
	int sockd;
};

struct REMOTE_CONN_floating {
	REMOTE_CONN_floating() = default;
	REMOTE_CONN_floating(REMOTE_CONN_floating &&);
	~REMOTE_CONN_floating() { reset(true); }
	REMOTE_CONN *operator->() { return tmplist.size() != 0 ? &tmplist.front() : nullptr; }
	bool operator==(std::nullptr_t) const { return tmplist.size() == 0; }
	bool operator!=(std::nullptr_t) const { return tmplist.size() != 0; }
	void reset(bool lost = false);

	std::list<REMOTE_CONN> tmplist;
};

struct AGENT_THREAD {
	REMOTE_SVR *pserver;
	pthread_t thr_id;
	int sockd;
};

static int g_conn_num;
static int g_threads_num;
static BOOL g_notify_stop;
static pthread_t g_scan_id;
static std::list<REMOTE_CONN> g_lost_list;
static std::list<AGENT_THREAD> g_agent_list;
static std::vector<LOCAL_SVR> g_local_list;
static std::list<REMOTE_SVR> g_server_list;
static std::mutex g_server_lock;

int exmdb_client_get_param(int param)
{
	int total_num;
	
	switch (param) {
	case ALIVE_PROXY_CONNECTIONS:
		total_num = 0;
		for (const auto &srv : g_server_list)
			total_num += srv.conn_list.size();
		return total_num;
	case LOST_PROXY_CONNECTIONS:
		return g_lost_list.size();
	}
	return -1;
}

static BOOL exmdb_client_read_socket(int sockd, BINARY *pbin)
{
	int tv_msec;
	int read_len;
	uint32_t offset;
	uint8_t resp_buff[5];
	struct pollfd pfd_read;
	
	pbin->pb = NULL;
	while (TRUE) {
		tv_msec = SOCKET_TIMEOUT * 1000;
		pfd_read.fd = sockd;
		pfd_read.events = POLLIN|POLLPRI;
		if (1 != poll(&pfd_read, 1, tv_msec)) {
			return FALSE;
		}
		if (NULL == pbin->pb) {
			read_len = read(sockd, resp_buff, 5);
			if (1 == read_len) {
				pbin->cb = 1;
				pbin->pv = common_util_alloc(1);
				if (pbin->pv == nullptr)
					return FALSE;
				*(uint8_t*)pbin->pb = resp_buff[0];
				return TRUE;
			} else if (5 == read_len) {
				pbin->cb = *(uint32_t*)(resp_buff + 1) + 5;
				pbin->pv = common_util_alloc(pbin->cb);
				if (pbin->pv == nullptr)
					return FALSE;
				memcpy(pbin->pv, resp_buff, 5);
				offset = 5;
				if (offset == pbin->cb) {
					return TRUE;
				}
				continue;
			} else {
				return FALSE;
			}
		}
		read_len = read(sockd, pbin->pb + offset, pbin->cb - offset);
		if (read_len <= 0) {
			return FALSE;
		}
		offset += read_len;
		if (offset == pbin->cb) {
			return TRUE;
		}
	}
}

static BOOL exmdb_client_write_socket(int sockd, const BINARY *pbin)
{
	int tv_msec;
	int written_len;
	uint32_t offset;
	struct pollfd pfd_write;
	
	offset = 0;
	while (TRUE) {
		tv_msec = SOCKET_TIMEOUT * 1000;
		pfd_write.fd = sockd;
		pfd_write.events = POLLOUT|POLLWRBAND;
		if (1 != poll(&pfd_write, 1, tv_msec)) {
			return FALSE;
		}
		written_len = write(sockd, pbin->pb + offset, pbin->cb - offset);
		if (written_len <= 0) {
			return FALSE;
		}
		offset += written_len;
		if (offset == pbin->cb) {
			return TRUE;
		}
	}
}

static int exmdb_client_connect_exmdb(REMOTE_SVR *pserver, BOOL b_listen)
{
	int process_id;
	BINARY tmp_bin;
	char remote_id[128];
	const char *str_host;
	EXMDB_REQUEST request;
	uint8_t response_code;

	int sockd = gx_inet_connect(pserver->host.c_str(), pserver->port, 0);
	if (sockd < 0)
	        return -1;
	str_host = get_host_ID();
	process_id = getpid();
	sprintf(remote_id, "%s:%d", str_host, process_id);
	if (FALSE == b_listen) {
		request.call_id = exmdb_callid::CONNECT;
		request.payload.connect.prefix = deconst(pserver->prefix.c_str());
		request.payload.connect.remote_id = remote_id;
		request.payload.connect.b_private = pserver->b_private;
	} else {
		request.call_id = exmdb_callid::LISTEN_NOTIFICATION;
		request.payload.listen_notification.remote_id = remote_id;
	}
	if (EXT_ERR_SUCCESS != exmdb_ext_push_request(&request, &tmp_bin)) {
		close(sockd);
		return -1;
	}
	if (FALSE == exmdb_client_write_socket(sockd, &tmp_bin)) {
		free(tmp_bin.pb);
		close(sockd);
		return -1;
	}
	free(tmp_bin.pb);
	exmdb_server_build_environment(FALSE, pserver->b_private, NULL);
	if (FALSE == exmdb_client_read_socket(sockd, &tmp_bin)) {
		exmdb_server_free_environment();
		close(sockd);
		return -1;
	}
	response_code = tmp_bin.pb[0];
	if (response_code == exmdb_response::SUCCESS) {
		if (5 != tmp_bin.cb || 0 != *(uint32_t*)(tmp_bin.pb + 1)) {
			exmdb_server_free_environment();
			printf("[exmdb_client]: response format error "
			       "during connect to [%s]:%hu/%s\n",
			       pserver->host.c_str(), pserver->port, pserver->prefix.c_str());
			close(sockd);
			return -1;
		}
		exmdb_server_free_environment();
		return sockd;
	}
	printf("[exmdb_provider]: Failed to connect to [%s]:%hu/%s",
	       pserver->host.c_str(), pserver->port, pserver->prefix.c_str());
	exmdb_server_free_environment();
	switch (response_code) {
	case exmdb_response::ACCESS_DENY:
		printf(": access denied\n");
		break;
	case exmdb_response::MAX_REACHED:
		printf(": maximum connections reached in server\n");
		break;
	case exmdb_response::LACK_MEMORY:
		printf(": server out of memory\n");
		break;
	case exmdb_response::MISCONFIG_PREFIX:
		printf(": prefix not served by server\n");
		break;
	case exmdb_response::MISCONFIG_MODE:
		printf(": misconfigured prefix mode\n");
		break;
	default:
		printf(": error code %d\n", response_code);
		break;
	}
	close(sockd);
	return -1;
}

static void *scan_work_func(void *pparam)
{
	int tv_msec;
	time_t now_time;
	uint8_t resp_buff;
	uint32_t ping_buff;
	struct pollfd pfd_read;
	std::list<REMOTE_CONN> temp_list;
	
	ping_buff = 0;
	while (FALSE == g_notify_stop) {
		std::unique_lock sv_hold(g_server_lock);
		time(&now_time);
		for (auto &srv : g_server_list) {
			auto tail = &*srv.conn_list.rbegin();
			while (srv.conn_list.size() > 0) {
				auto pconn = &srv.conn_list.front();
				if (now_time - pconn->last_time >= SOCKET_TIMEOUT - 3)
					temp_list.splice(temp_list.end(), srv.conn_list, srv.conn_list.begin());
				else
					srv.conn_list.splice(srv.conn_list.end(), srv.conn_list, srv.conn_list.begin());
				if (pconn == tail)
					break;
			}
		}
		sv_hold.unlock();

		while (temp_list.size() > 0) {
			auto pconn = &temp_list.front();
			if (TRUE == g_notify_stop) {
				close(pconn->sockd);
				temp_list.pop_front();
				continue;
			}
			if (sizeof(uint32_t) != write(pconn->sockd,
				&ping_buff, sizeof(uint32_t))) {
				close(pconn->sockd);
				pconn->sockd = -1;
				sv_hold.lock();
				g_lost_list.splice(g_lost_list.end(), temp_list, temp_list.begin());
				sv_hold.unlock();
				continue;
			}
			tv_msec = SOCKET_TIMEOUT * 1000;
			pfd_read.fd = pconn->sockd;
			pfd_read.events = POLLIN|POLLPRI;
			if (1 != poll(&pfd_read, 1, tv_msec) ||
				1 != read(pconn->sockd, &resp_buff, 1) ||
			    resp_buff != exmdb_response::SUCCESS) {
				close(pconn->sockd);
				pconn->sockd = -1;
				sv_hold.lock();
				g_lost_list.splice(g_lost_list.end(), temp_list, temp_list.begin());
				sv_hold.unlock();
			} else {
				time(&pconn->last_time);
				sv_hold.lock();
				pconn->psvr->conn_list.splice(pconn->psvr->conn_list.end(), temp_list, temp_list.begin());
				sv_hold.unlock();
			}
		}

		sv_hold.lock();
		temp_list = std::move(g_lost_list);
		sv_hold.unlock();

		while (temp_list.size() > 0) {
			auto pconn = &temp_list.front();
			if (TRUE == g_notify_stop) {
				close(pconn->sockd);
				temp_list.pop_front();
				continue;
			}
			pconn->sockd = exmdb_client_connect_exmdb(pconn->psvr, FALSE);
			if (-1 != pconn->sockd) {
				time(&pconn->last_time);
				sv_hold.lock();
				pconn->psvr->conn_list.splice(pconn->psvr->conn_list.end(), temp_list, temp_list.begin());
				sv_hold.unlock();
			} else {
				sv_hold.lock();
				g_lost_list.splice(g_lost_list.end(), temp_list, temp_list.begin());
				sv_hold.unlock();
			}
		}
		sleep(1);
	}
	return NULL;
}

static void *thread_work_func(void *pparam)
{
	int i;
	int tv_msec;
	int read_len;
	BINARY tmp_bin;
	uint32_t offset;
	uint8_t resp_code;
	uint32_t buff_len;
	uint8_t buff[0x8000];
	AGENT_THREAD *pagent;
	struct pollfd pfd_read;
	DB_NOTIFY_DATAGRAM notify;
	
	pagent = (AGENT_THREAD*)pparam;
	while (FALSE == g_notify_stop) {
		pagent->sockd = exmdb_client_connect_exmdb(
							pagent->pserver, TRUE);
		if (-1 == pagent->sockd) {
			sleep(1);
			continue;
		}
		buff_len = 0;
		while (TRUE) {
			tv_msec = SOCKET_TIMEOUT * 1000;
			pfd_read.fd = pagent->sockd;
			pfd_read.events = POLLIN|POLLPRI;
			if (1 != poll(&pfd_read, 1, tv_msec)) {
				close(pagent->sockd);
				pagent->sockd = -1;
				break;
			}
			if (0 == buff_len) {
				if (sizeof(uint32_t) != read(pagent->sockd,
					&buff_len, sizeof(uint32_t))) {
					close(pagent->sockd);
					pagent->sockd = -1;
					break;
				}
				/* ping packet */
				if (0 == buff_len) {
					resp_code = exmdb_response::SUCCESS;
					if (1 != write(pagent->sockd, &resp_code, 1)) {
						close(pagent->sockd);
						pagent->sockd = -1;
						break;
					}
				}
				offset = 0;
				continue;
			}
			read_len = read(pagent->sockd, buff + offset, buff_len - offset);
			if (read_len <= 0) {
				close(pagent->sockd);
				pagent->sockd = -1;
				break;
			}
			offset += read_len;
			if (offset == buff_len) {
				tmp_bin.cb = buff_len;
				tmp_bin.pb = buff;
				exmdb_server_build_environment(FALSE,
					pagent->pserver->b_private, NULL);
				if (EXT_ERR_SUCCESS == exmdb_ext_pull_db_notify(
					&tmp_bin, &notify)) {
					resp_code = exmdb_response::SUCCESS;
				} else {
					resp_code = exmdb_response::PULL_ERROR;
				}
				if (1 != write(pagent->sockd, &resp_code, 1)) {
					close(pagent->sockd);
					pagent->sockd = -1;
					exmdb_server_free_environment();
					break;
				}
				if (resp_code == exmdb_response::SUCCESS) {
					for (i=0; i<notify.id_array.count; i++) {
						exmdb_server_event_proc(notify.dir,
							notify.b_table, notify.id_array.pl[i],
							&notify.db_notify);
					}
				}
				exmdb_server_free_environment();
				buff_len = 0;
			}
		}
	}
	return nullptr;
}

static REMOTE_CONN_floating exmdb_client_get_connection(const char *dir)
{
	REMOTE_CONN_floating fc;
	auto i = std::find_if(g_server_list.begin(), g_server_list.end(),
	         [&](const REMOTE_SVR &s) { return strncmp(dir, s.prefix.c_str(), s.prefix.size()) == 0; });
	if (i == g_server_list.end()) {
		printf("[exmdb_provider]: cannot find remote server for %s\n", dir);
		return fc;
	}
	std::unique_lock sv_hold(g_server_lock);
	if (i->conn_list.size() == 0) {
		sv_hold.unlock();
		printf("[exmdb_provider]: no alive connection for [%s]:%hu/%s\n",
		       i->host.c_str(), i->port, i->prefix.c_str());
		return fc;
	}
	fc.tmplist.splice(fc.tmplist.end(), i->conn_list, i->conn_list.begin());
	return fc;
}

void REMOTE_CONN_floating::reset(bool lost)
{
	if (tmplist.size() == 0)
		return;
	auto pconn = &tmplist.front();
	if (!lost) {
		std::unique_lock sv_hold(g_server_lock);
		pconn->psvr->conn_list.splice(pconn->psvr->conn_list.end(), tmplist, tmplist.begin());
	} else {
		close(pconn->sockd);
		pconn->sockd = -1;
		std::unique_lock sv_hold(g_server_lock);
		g_lost_list.splice(g_lost_list.end(), tmplist, tmplist.begin());
	}
	tmplist.clear();
}

REMOTE_CONN_floating::REMOTE_CONN_floating(REMOTE_CONN_floating &&o)
{
	reset(true);
	tmplist = std::move(o.tmplist);
}

void exmdb_client_init(int conn_num, int threads_num)
{
	g_notify_stop = TRUE;
	g_conn_num = conn_num;
	g_threads_num = threads_num;
}

int exmdb_client_run(const char *config_path)
{
	BOOL b_private;
	
	auto plist = list_file_initd("exmdb_list.txt", config_path,
	             /* EXMDB_ITEM */ "%s:256%s:16%s:32%d");
	if (NULL == plist) {
		printf("[exmdb_provider]: list_file_initd exmdb_list.txt: %s\n", strerror(errno));
		return 1;
	}
	g_notify_stop = FALSE;
	auto list_num = plist->get_size();
	auto pitem = static_cast<EXMDB_ITEM * >(plist->get_list());
	for (decltype(list_num) i = 0; i < list_num; ++i) {
		if (0 == strcasecmp(pitem[i].type, "private")) {
			b_private = TRUE;
		} else if (0 == strcasecmp(pitem[i].type, "public")) {
			b_private = FALSE;
		} else {
			printf("[exmdb_provider]: unknown type \"%s\", only"
				"can be \"private\" or \"public\"!", pitem[i].type);
			g_notify_stop = TRUE;
			return 2;
		}
		if (gx_peer_is_local(pitem[i].ip_addr)) try {
			LOCAL_SVR lcl;
			lcl.prefix = pitem[i].prefix;
			lcl.b_private = b_private;
			g_local_list.push_back(std::move(lcl));
			continue;
		} catch (const std::bad_alloc &) {
			printf("[exmdb_provider]: Failed to allocate memory\n");
			g_notify_stop = TRUE;
			return 3;
		}
		if (0 == g_conn_num) {
			printf("[exmdb_provider]: there's remote store media "
				"in exmdb list, but rpc proxy connection number is 0\n");
			g_notify_stop = TRUE;
			return 4;
		}

		try {
			g_server_list.push_back(REMOTE_SVR{});
			auto &srv = g_server_list.back();
			srv.prefix = pitem[i].prefix;
			srv.b_private = b_private;
			srv.host = pitem[i].ip_addr;
			srv.port = pitem[i].port;
		} catch (const std::bad_alloc &) {
			printf("[exmdb_provider]: Failed to allocate memory for exmdb\n");
			g_notify_stop = TRUE;
			return 5;
		}
		auto &srv = g_server_list.back();
		for (decltype(g_conn_num) j = 0; j < g_conn_num; ++j) {
			REMOTE_CONN conn;
			conn.sockd = -1;
			static_assert(std::is_same_v<decltype(g_server_list), std::list<decltype(g_server_list)::value_type>>,
				"addrof REMOTE_SVRs must not change; REMOTE_CONN/AGENT_THREAD has a pointer to it");
			conn.psvr = &srv;
			try {
				g_lost_list.push_back(std::move(conn));
			} catch (const std::bad_alloc &) {
				printf("[exmdb_provider]: fail to "
					"allocate memory for exmdb\n");
				g_notify_stop = TRUE;
				return 6;
			}
		}
		for (decltype(g_threads_num) j = 0; j < g_threads_num; ++j) {
			try {
				g_agent_list.push_back(AGENT_THREAD{});
			} catch (const std::bad_alloc &) {
				printf("[exmdb_provider]: fail to "
					"allocate memory for exmdb\n");
				g_notify_stop = TRUE;
				return 7;
			}
			auto &ag = g_agent_list.back();
			ag.pserver = &srv;
			ag.sockd = -1;
			static_assert(std::is_same_v<decltype(g_agent_list), std::list<decltype(g_agent_list)::value_type>>,
				"addrof AGENT_THREADs must not change; other thread has its address in use");
			if (pthread_create(&ag.thr_id, nullptr, thread_work_func, &ag) != 0) {
				printf("[exmdb_provider]: fail to "
					"create agent thread for exmdb\n");
				g_notify_stop = TRUE;
				g_agent_list.pop_back();
				return 8;
			}
			char buf[32];
			snprintf(buf, sizeof(buf), "exmdbcl/%u", i);
			pthread_setname_np(ag.thr_id, buf);
		}
	}
	plist.reset();
	if (0 == g_conn_num) {
		return 0;
	}
	int ret = pthread_create(&g_scan_id, nullptr, scan_work_func, nullptr);
	if (ret != 0) {
		printf("[exmdb_provider]: failed to create proxy scan thread: %s\n", strerror(ret));
		g_notify_stop = TRUE;
		return 9;
	}
	pthread_setname_np(g_scan_id, "exmdbcl/scan");
	return 0;
}

int exmdb_client_stop()
{
	if (0 != g_conn_num) {
		if (FALSE == g_notify_stop) {
			g_notify_stop = TRUE;
			pthread_join(g_scan_id, NULL);
		}
	}
	g_notify_stop = TRUE;
	for (auto &ag : g_agent_list) {
		pthread_cancel(ag.thr_id);
		if (ag.sockd >= 0)
			close(ag.sockd);
	}
	for (auto &srv : g_server_list)
		for (auto &conn : srv.conn_list)
			close(conn.sockd);
	return 0;
}

BOOL exmdb_client_check_local(const char *prefix, BOOL *pb_private)
{
	auto i = std::find_if(g_local_list.cbegin(), g_local_list.cend(),
	         [&](const LOCAL_SVR &s) { return strncmp(s.prefix.c_str(), prefix, s.prefix.size()) == 0; });
	if (i == g_local_list.cend())
		return false;
	*pb_private = i->b_private;
	return TRUE;
}

BOOL exmdb_client_do_rpc(const char *dir,
	const EXMDB_REQUEST *prequest, EXMDB_RESPONSE *presponse)
{
	BINARY tmp_bin;
	
	if (EXT_ERR_SUCCESS != exmdb_ext_push_request(prequest, &tmp_bin)) {
		return FALSE;
	}
	auto pconn = exmdb_client_get_connection(dir);
	if (pconn == nullptr) {
		free(tmp_bin.pb);
		return FALSE;
	}
	if (FALSE == exmdb_client_write_socket(pconn->sockd, &tmp_bin)) {
		free(tmp_bin.pb);
		return FALSE;
	}
	free(tmp_bin.pb);
	if (FALSE == exmdb_client_read_socket(pconn->sockd, &tmp_bin)) {
		return FALSE;
	}
	time(&pconn->last_time);
	pconn.reset();
	if (tmp_bin.cb < 5 || tmp_bin.pb[0] != exmdb_response::SUCCESS)
		return FALSE;
	presponse->call_id = prequest->call_id;
	tmp_bin.cb -= 5;
	tmp_bin.pb += 5;
	if (EXT_ERR_SUCCESS != exmdb_ext_pull_response(&tmp_bin, presponse)) {	
		return FALSE;
	}
	return TRUE;
}

/* Caution. This function is not a common exmdb service,
	it only can be called by message_rule_new_message to
	pass a message to the delegate's mailbox. */
BOOL exmdb_client_relay_delivery(const char *dir,
	const char *from_address, const char *account,
	uint32_t cpid, const MESSAGE_CONTENT *pmsg,
	const char *pdigest, uint32_t *presult)
{
	BOOL b_result;
	BOOL b_private;
	EXMDB_REQUEST request;
	EXMDB_RESPONSE response;
	const char *orignal_dir;
	
	if (TRUE == exmdb_client_check_local(dir, &b_private)) {
		orignal_dir = exmdb_server_get_dir();
		exmdb_server_set_dir(dir);
		b_result = exmdb_server_delivery_message(
					dir, from_address, account,
					cpid, pmsg, pdigest, presult);
		exmdb_server_set_dir(orignal_dir);
		return b_result;
	}
	request.call_id = exmdb_callid::DELIVERY_MESSAGE;
	request.dir = deconst(dir);
	request.payload.delivery_message.from_address = deconst(from_address);
	request.payload.delivery_message.account = deconst(account);
	request.payload.delivery_message.cpid = cpid;
	request.payload.delivery_message.pmsg = deconst(pmsg);
	request.payload.delivery_message.pdigest = deconst(pdigest);
	if (FALSE == exmdb_client_do_rpc(dir, &request, &response)) {
		return FALSE;
	}
	*presult = response.payload.delivery_message.result;
	return TRUE;
}
