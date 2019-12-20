#include <sys/stat.h>
#include <libHX/string.h>
#include <gromox/paths.h>
#include <gromox/system_log.h>
#include "midb_client.h"
#include "message_lookup.h"
#include "data_source.h"
#include <gromox/session_client.h>
#include "backup_ui.h"
#include "config_file.h"
#include "util.h"
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, const char **argv)
{
	int valid_days;
	const char *str_value;
	char temp_path[256];
	char lang_path[256];
	char cidb_path[256];
	char data_path[256];
	char session_ip[16];
	int session_port;
	char mysql_host[256];
	int mysql_port;
	char mysql_user[256];
	char *mysql_passwd;
	char db_name[256];
	CONFIG_FILE *pconfig;

	umask(0);
	HX_strlcpy(temp_path, PKGSYSCONFDIR "/posidon.cfg", sizeof(temp_path));
	pconfig = config_file_init2(NULL, temp_path);
	if (NULL == pconfig) {
		return 1;
	}
	str_value = config_file_get_value(pconfig, "DATA_FILE_PATH");
	if (NULL == str_value) {
		HX_strlcpy(data_path, PKGDATADADIR, sizeof(data_path));
	} else {
		strcpy(data_path, str_value);
	}
	snprintf(temp_path, sizeof(temp_path), "%s/midb_list.txt", data_path);
	midb_client_init(temp_path);
	snprintf(cidb_path, sizeof(cidb_path), "%s/cidb_list.txt", data_path);
	message_lookup_init(cidb_path);
	
	str_value = config_file_get_value(pconfig, "LOG_FILE_PATH");
	if (NULL == str_value) {
		str_value = PKGLOGDIR "/posidon_log.txt";
	}
	system_log_init(str_value);
	
	str_value = config_file_get_value(pconfig, "SESSION_LISTEN_IP");
	if (NULL == str_value) {
		strcpy(session_ip, "127.0.0.1");
	} else {
		strncpy(session_ip, str_value, 16);
	}

	str_value = config_file_get_value(pconfig, "SESSION_LISTEN_PORT");
	if (NULL == str_value) {
		session_port = 9999;
	} else {
		session_port = atoi(str_value);
		if (session_port <= 0) {
			session_port = 9999;
		}
	}
	session_client_init(session_ip, session_port);

	str_value = config_file_get_value(pconfig, "MYSQL_HOST");
	if (NULL == str_value) {
		strcpy(mysql_host, "localhost");
	} else {
		strcpy(mysql_host, str_value);
	}

	str_value = config_file_get_value(pconfig, "MYSQL_PORT");
	if (NULL == str_value) {
		mysql_port = 3306;
	} else {
		mysql_port = atoi(str_value);
		if (mysql_port <= 0) {
			mysql_port = 3306;
		}
	}

	str_value = config_file_get_value(pconfig, "MYSQL_USERNAME");
	if (NULL == str_value) {
		mysql_user[0] = '\0';
	} else {
		strcpy(mysql_user, str_value);
	}

	mysql_passwd = config_file_get_value(pconfig, "MYSQL_PASSWORD");

	str_value = config_file_get_value(pconfig, "MYSQL_DBNAME");
	if (NULL == str_value) {
		strcpy(db_name, "email");
	} else {
		strcpy(db_name, str_value);
	}

	data_source_init(mysql_host, mysql_port, mysql_user, mysql_passwd, db_name);
	
	str_value = config_file_get_value(pconfig, "BACKUP_VALID_DAYS");
	if (NULL == str_value) {
		valid_days = 365;
	} else {
		valid_days = atoi(str_value);
		if (valid_days < 1) {
			valid_days = 90;
		}
	}
	
	str_value = config_file_get_value(pconfig, "LOGO_LINK");
	if (NULL == str_value) {
		str_value = "http://www.gridware.com.cn";
	}

	snprintf(lang_path, sizeof(lang_path), "%s/group_archive", data_path);
	backup_ui_init(valid_days, str_value, lang_path);

	config_file_free(pconfig);
	if (0 != system_log_run()) {
		return 2;
	}
	if (0 != midb_client_run()) {
		return 3;
	}
	if (0 != message_lookup_run()) {
		return 4;
	}
	if (0 != session_client_run()) {
		return 5;
	}
	if (0 != data_source_run()) {
		return 6;
	}
	if (0 != backup_ui_run()) {
		return 7;
	}
	backup_ui_stop();
	backup_ui_free();
	data_source_stop();
	data_source_free();
	session_client_stop();
	session_client_free();
	message_lookup_stop();
	message_lookup_free();
	midb_client_stop();
	midb_client_free();
	system_log_stop();
	system_log_free();
	exit(0);
}

