// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <libHX/ctype_helper.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include "engine.h"
#include "file_operation.h"
#include <gromox/gateway_control.h>
#include "data_source.h"
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

static std::atomic<bool> g_notify_stop{false};
static pthread_t g_thread_id1;
static char g_domainlist_path[256];
static char g_aliasaddress_path[256];
static char g_unchkusr_path[256];

static void *adap_thrwork(void *);

void engine_init(const char *domainlist_path,
    const char *aliasaddress_path, const char *unchkusr_path)
{
	gx_strlcpy(g_domainlist_path, domainlist_path, GX_ARRAY_SIZE(g_domainlist_path));
	gx_strlcpy(g_aliasaddress_path, aliasaddress_path, GX_ARRAY_SIZE(g_aliasaddress_path));
	gx_strlcpy(g_unchkusr_path, unchkusr_path, GX_ARRAY_SIZE(g_unchkusr_path));
}


int engine_run()
{
	g_notify_stop = false;
	auto ret = pthread_create(&g_thread_id1, nullptr, adap_thrwork, nullptr);
	if (ret != 0) {
		g_notify_stop = true;
		printf("[engine]: failed to create work thread: %s\n", strerror(ret));
		return -1;
	}
	pthread_setname_np(g_thread_id1, "work/1");
	return 0;
}

void engine_trig()
{
	pthread_kill(g_thread_id1, SIGALRM);
}

int engine_stop()
{
	if (!g_notify_stop) {
		g_notify_stop = true;
		pthread_kill(g_thread_id1, SIGALRM);
		pthread_join(g_thread_id1, NULL);
	}
	return 0;
}

static void *adap_thrwork(void *param)
{
	int fd, len;
	char temp_domain[257];
	char temp_line[1024];
	char temp_path[256];
	DOMAIN_ITEM *pdomain_item;
	ALIAS_ITEM *palias_item;
	DATA_COLLECT *pcollect;

	while (!g_notify_stop) {
		fprintf(stderr, "[engine]: starting data collection\n");
		pcollect = data_source_collect_init();
	
		if (NULL == pcollect) {
			goto NEXT_LOOP;
		}
		
		if (FALSE == data_source_get_domain_list(pcollect)) {
			data_source_collect_free(pcollect);
			goto NEXT_LOOP;
		}
		snprintf(temp_path, GX_ARRAY_SIZE(temp_path), "%s.tmp", g_domainlist_path);
		fd = open(temp_path, O_CREAT|O_TRUNC|O_WRONLY, DEF_MODE);
		if (-1 == fd) {
			data_source_collect_free(pcollect);
			goto NEXT_LOOP;
		}
	
		for (data_source_collect_begin(pcollect);
			!data_source_collect_done(pcollect);
			data_source_collect_forward(pcollect)) {
			pdomain_item = (DOMAIN_ITEM*)data_source_collect_get_value(
							pcollect);
			len = sprintf(temp_domain, "%s\n", pdomain_item->domainname);
			write(fd, temp_domain, len);
		}
		close(fd);

		if (0 != file_operation_compare(temp_path, g_domainlist_path)) {
			if (rename(temp_path, g_domainlist_path) < 0)
				fprintf(stderr, "E-1402: rename %s %s: %s\n",
				        temp_path, g_domainlist_path, strerror(errno));
			gateway_control_notify("libgxs_domain_list.so reload",
				NOTIFY_SMTP|NOTIFY_DELIVERY);
		}
		
		data_source_collect_clear(pcollect);

		if (FALSE == data_source_get_alias_list(pcollect)) {
			data_source_collect_free(pcollect);
			goto NEXT_LOOP;
		}
		
		snprintf(temp_path, GX_ARRAY_SIZE(temp_path), "%s.tmp", g_aliasaddress_path);
		fd = open(temp_path, O_CREAT|O_TRUNC|O_WRONLY, DEF_MODE);
		if (-1 == fd) {
			data_source_collect_free(pcollect);
			goto NEXT_LOOP;
		}
		
		for (data_source_collect_begin(pcollect);
			!data_source_collect_done(pcollect);
			data_source_collect_forward(pcollect)) {
			palias_item = (ALIAS_ITEM*)data_source_collect_get_value(pcollect);
			len = sprintf(temp_line, "%s\t%s\n", palias_item->aliasname,
				palias_item->mainname);
			if (strchr(palias_item->aliasname, '@') != nullptr)
				write(fd, temp_line, len);
		}
		close(fd);

		if (0 != file_operation_compare(temp_path, g_aliasaddress_path)) {
			if (rename(temp_path, g_aliasaddress_path) < 0)
				fprintf(stderr, "E-1423: rename %s %s: %s\n",
				        temp_path, g_aliasaddress_path, strerror(errno));
			gateway_control_notify("libgxm_alias_translator.so reload addresses",
				NOTIFY_DELIVERY);
		}

		data_source_collect_clear(pcollect);
 NEXT_LOOP:
		sleep(30);
	}
	return NULL;
}
