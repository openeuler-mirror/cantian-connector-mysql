/*
  Copyright (C) 2023. Huawei Technologies Co., Ltd. All rights reserved.
  
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA 
*/
#include "my_global.h"
#include "tse_proxy_util.h"
#include <atomic>
#include "mysql/psi/psi.h"
#include "mysql/psi/psi_memory.h"
#include "tse_log.h"
#include "sql/sql_connect.h"
//#include "compression.h"
#include "ha_tse.h"
#include "sql/mysqld.h"
#include "ctc_meta_data.h"
#include "errmsg.h" // CR_OUT_OF_MEMORY

#define TSE_CONN_MAX_RETRY_TIMES 10
#define TSE_PASSWORD_BUFFER_SIZE (uint32)512

#ifndef SENSI_INFO
#define SENSI_INFO
#endif

void ctc_my_md5(unsigned char*, const char*, size_t)
{
        return;
}
void ctc_my_md5_multi(unsigned char*, ...)
{
        return;
}

size_t ctc_my_md5_context_size()
{
        return 0;
}

void ctc_my_md5_init(void *context)
{
        return;
}

void ctc_my_md5_input(void *context, const unsigned char *buf, size_t len)
{
        return;
}

void ctc_my_md5_result(void *context, unsigned char *digest)
{
        return;
}

static struct my_md5_service_st my_md5_handler = {
  ctc_my_md5,
  ctc_my_md5_multi,
  ctc_my_md5_context_size,
  ctc_my_md5_init,
  ctc_my_md5_input,
  ctc_my_md5_result
};

struct my_md5_service_st *my_md5_service = &my_md5_handler;

static atomic<uint32_t> g_name_locks(0);  // 实例持有的命名锁总数，用于标记当前是否有 DDL 正在执行

uint32_t get_g_name_locks(void) {
    return g_name_locks.load();
}

uint32_t add_g_name_locks(uint32_t num) {
    uint32_t name_locks = g_name_locks.fetch_add(num);
    return name_locks + num;
}

uint32_t sub_g_name_locks(uint32_t num) {
    uint32_t name_locks = g_name_locks.fetch_sub(num);
    return name_locks - num;
}

static bool is_dangerous_pwd(const char *password) {

  bool danger_pwd_flag = false;

  for(int i = 0; password[i] != '\0'; i++) {
    if (!isalnum(password[i]) && password[i] != '+' && password[i] != '/' && password[i] != '=' &&
        !(password[i] == '\n' && password[i+1] == '\0')) {
      danger_pwd_flag = true;
      break;
    }
  }
  return danger_pwd_flag;
}

static int tse_get_agent_info(char *password, uint password_size) {
  char *user_password = getenv("MYSQL_AGENT_PASSWORD");
  if (user_password == NULL) {
    return -1;
  }
  if (is_dangerous_pwd(user_password)) {
    tse_log_error("checking pwd failed");
    return -1;
  }

  string cmd = "encrypt -d ";
  cmd += user_password;

  FILE *fp = popen(cmd.c_str(), "r");
  if (fp == NULL) {
    tse_log_error("encrypting cmd failed");
    return -1;
  }

  if (fgets(password, password_size, fp) == NULL) {
    tse_log_error("get password failed");
    pclose (fp);
    return -1;
  }

  size_t passwd_len = strlen(password);
  passwd_len = passwd_len >= password_size ? password_size - 1 : passwd_len;
  if (passwd_len > 0) {
    password[passwd_len] = '\0';
  }

  // remove \n from the passwd
  if(passwd_len > 0 && password[passwd_len -1] == '\n' ) {
    password[passwd_len -1] = '\0';
  }

  pclose (fp);
  return 0;
}

static void tse_inc_conn_count(int &dec_conn_count)
{
		/*
  Connection_handler_manager *conn_manager = Connection_handler_manager::get_instance();
  conn_manager->check_and_incr_conn_count(true);
  Connection_handler_manager::reset_max_used_connections();
  */
  dec_conn_count--;
  //tse_log_warning("[TSE_CONN_INC]:connection_count:%u, max_used_connections:%lu, max_connections:%lu, dec_conn_count:%d",
      //            Connection_handler_manager::connection_count, Connection_handler_manager::max_used_connections, max_connections, dec_conn_count);
}

static void tse_dec_conn_count(int &dec_conn_count)
{
  //Connection_handler_manager::dec_connection_count();
  dec_conn_count++;
  //tse_log_warning("[TSE_CONN_DEC]:connection_count:%u, max_used_connections:%lu, max_connections:%lu, dec_conn_count:%d",
    //              Connection_handler_manager::connection_count, Connection_handler_manager::max_used_connections, max_connections, dec_conn_count);
}

int tse_mysql_conn(MYSQL *&con, const char *host, const char *user, const char *passwd) {
  con = mysql_init(NULL);
  if (con == nullptr) {
    tse_log_error("tse_mysql_conn mysql_init failed, error_code:%d,error:%s", mysql_errno(con), mysql_error(con));
    my_thread_end();
    if (!mysql_errno(con)) {
      return CR_OUT_OF_MEMORY;
    }
    return mysql_errno(con);
  }

  const bool opt_reconnect = true;
  (void)mysql_options(con, MYSQL_OPT_RECONNECT, &opt_reconnect);
  (void)mysql_options(con, MYSQL_DEFAULT_AUTH, "mysql_native_password");

  // do init sql commands for new con.
  // for ddl do not go to ctc SE. this command should be first executed. it should be placed firstly at mysql_options MYSQL_INIT_COMMAND.
  (void)mysql_options(con, MYSQL_INIT_COMMAND, "set @ctc_ddl_local_enabled = true;");
  /* proxy的连接强制解除只读属性 
    proxy的连接应该永远都是没有只读状态才对的，因为如果发起端连接只读，ddl不会转到远端，
    但是如果发起端没有只读属性，远端proxy建立的连接有只读属性，远端执行ddl就会报错，节点被踢掉
  */
  (void)mysql_options(con, MYSQL_INIT_COMMAND, "SET SESSION TRANSACTION_READ_ONLY = 0;");

  // 设为 1 年 365 * 86400 = 31536000，保证连接闲时等待不返回超时
  const uint32_t opt_timeout = 31536000;
  (void)mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &opt_timeout);
  (void)mysql_options(con, MYSQL_OPT_READ_TIMEOUT, &opt_timeout);
  (void)mysql_options(con, MYSQL_OPT_WRITE_TIMEOUT, &opt_timeout);

  int retry_time = TSE_CONN_MAX_RETRY_TIMES;
  int dec_conn_count = 0; 
  while (!mysql_real_connect(con, host, user, passwd, NULL, mysqld_port, mysqld_unix_port, CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS | CLIENT_REMEMBER_OPTIONS | CLIENT_INTERACTIVE) && retry_time > 0) {
    int err = mysql_errno(con);
    if (err == ER_CON_COUNT_ERROR) {
      tse_dec_conn_count(dec_conn_count);
    }
#if 0
	// mariadb doesn't support wire compression
    // 3922 errcode, server has open compress conn, client need to open.
    if (err == ER_WRONG_COMPRESSION_ALGORITHM_CLIENT) {
      if ((con->server_capabilities & CLIENT_COMPRESS)) { // server open zlib conn
        mysql_options(con, MYSQL_OPT_COMPRESSION_ALGORITHMS, COMPRESSION_ALGORITHM_ZLIB);
      } else if((con->server_capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM)) { // server open zstd conn
        mysql_options(con, MYSQL_OPT_COMPRESSION_ALGORITHMS, COMPRESSION_ALGORITHM_ZSTD);
      }
      retry_time--;
      continue;
    }
#endif
    tse_log_error("tse_mysql_conn create internal connection failed, retry_time=%d, user=%s, err_code=%d, err_msg=%s.",
                  retry_time, user, err, mysql_error(con));
    retry_time--;
  }

  while (dec_conn_count != 0) {
    tse_inc_conn_count(dec_conn_count);
  }
  my_thread_end();
  return con == nullptr;
}

int tse_init_agent_client(MYSQL *&curr_conn) {
  SENSI_INFO char agent_password[TSE_PASSWORD_BUFFER_SIZE] = {0};
  string agent_user = "root";
  if (!tse_get_agent_info(agent_password, TSE_PASSWORD_BUFFER_SIZE)) {
    agent_user = "RDS_agent";
  }
  const char *password = (agent_user == "root") ? NULL : agent_password;
  int ret = tse_mysql_conn(curr_conn, my_localhost, agent_user.c_str(), password);
  memset(agent_password, 0, TSE_PASSWORD_BUFFER_SIZE);
  if (ret) {
    tse_log_error("tse_init_agent_client tse_mysql_conn failed, err_code=%d, err_msg=%s.",
                  mysql_errno(curr_conn), mysql_error(curr_conn));
    tse_close_mysql_conn(&curr_conn);
    return ret;
  }

  return 0;
}

int tse_mysql_query(MYSQL *mysql, const char *query) {
  reset_mqh(nullptr, 0);

  int ret = mysql_ping(mysql);
  if (ret != 0) {
    tse_log_error("tse_mysql_query: mysql server has gone. error_code:%d, reconnecting.", ret);
  }

  /* 
     SUCCESS:                 0
     CR_COMMANDS_OUT_OF_SYNC: 2014
     CR_SERVER_GONE_ERROR:    2006 
     CR_SERVER_LOST:          2013 
     CR_UNKNOWN_ERROR:        2000
  */

  if (tse_get_cluster_role() == (int32_t)dis_cluster_role::STANDBY) {
    tse_reset_mysql_read_only();
  }

  do {
    mysql_free_result(mysql_use_result(mysql));
  } while (!mysql_next_result(mysql));

  ret = mysql_query(mysql, query);
  if (tse_get_cluster_role() == (int32_t)dis_cluster_role::STANDBY) {
    tse_set_mysql_read_only();
  }
  if (ret != 0) {
    tse_log_error("[TSE_MYSQL_QUERY]:ret:%d, err_code=%d, err_msg=%s, query_str:%s.", ret, mysql_errno(mysql), mysql_error(mysql), query);
  }

  return ret;  // success: 0, fail: other
}

void tse_close_mysql_conn(MYSQL **curr_conn) {
  mysql_close(*curr_conn);
  *curr_conn = NULL;
}