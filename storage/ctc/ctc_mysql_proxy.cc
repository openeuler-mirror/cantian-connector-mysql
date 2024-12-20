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
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "my_md5.h"
#include "my_md5_size.h"
#include "mysql.h"
#include "sql/mysqld.h" // mysql_port, my_localhost
#include "ctc_log.h"
#include "ctc_srv.h"
#include "ctc_util.h"
#include "ha_ctc.h"
#include "ctc_meta_data.h"
#include "ctc_proxy_util.h"
#include "sql/sql_table.h"
#include "my_dir.h"
#include "sql/sql_handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/dd/types/procedure.h"
#include "sql/dd/types/function.h"
#include "sql/dd/types/routine.h"
#include "sql/mdl.h"
#include "sql/dd/types/event.h"
#include "sql/dd/types/resource_group.h"
#include "sql/dd/types/trigger.h"
#include "sql/auth/auth_common.h"
#include "sql/sys_vars_shared.h"  // intern_find_sys_var
#include "sql/sql_lex.h"          // lex_start/lex_end
#include "sql/handler.h"          // ha_ctc_commit

using namespace std;

__attribute__((visibility("default"))) mutex m_ctc_cluster_role_mutex;
__attribute__((visibility("default"))) int32_t ctc_cluster_role = (int32_t)dis_cluster_role::DEFAULT;
struct ctc_mysql_conn_info {
    MYSQL*                    conn;
    set<pair<string, string>> table_lock_info;  // 连接上已存在的表锁 (db, table)
    bool                      has_explicit_table_lock;  // 连接上是否存在显式的表锁
    uint32_t                  name_locks;  // 连接上持有的命名锁数量
};

static map<uint64_t, ctc_mysql_conn_info*> g_mysql_conn_map;
static mutex m_ctc_mysql_proxy_mutex;

static ctc_mysql_conn_info* init_ctc_mysql_conn(MYSQL* curr_conn) {
  ctc_mysql_conn_info *ctc_conn_info = new ctc_mysql_conn_info();
  ctc_conn_info->conn = curr_conn;
  ctc_conn_info->table_lock_info = set<pair<string, string>>();
  ctc_conn_info->has_explicit_table_lock = false;
  ctc_conn_info->name_locks = 0;
  return ctc_conn_info;
}

static void set_explicit_table_lock(uint64_t conn_map_key, uint8_t sql_command) {
  lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
  if (sql_command == SQLCOM_LOCK_TABLES) {
    g_mysql_conn_map[conn_map_key]->has_explicit_table_lock = true;
  } else if (sql_command == SQLCOM_UNLOCK_TABLES) {
    g_mysql_conn_map[conn_map_key]->has_explicit_table_lock = false;
  }
  return;
}

int ctc_select_db(MYSQL *curr_conn, const char *db) {
  if (CM_IS_EMPTY_STR(db)) {
    return 0;
  }

  int ret = mysql_ping(curr_conn);
  if (ret != 0) {
    ctc_log_error("ctc_select_db: mysql server has gone. db:%s error_code:%d, reconnecting.", db, ret);
  }
  
  ret = mysql_select_db(curr_conn, db);
  if (ret != 0) {
    ctc_log_error("select db:%s failed,ret:%d, error_code:%d,desc:%s", db, ret,
                  mysql_errno(curr_conn), mysql_error(curr_conn));
    return mysql_errno(curr_conn);
  }
  return 0;
}

static void ctc_drop_proxy_user(MYSQL *agent_conn, const string &proxy_user_name)
{
  string drop_user_sql = "drop user if exists '" + proxy_user_name + "';";
  
  if (ctc_mysql_query(agent_conn, drop_user_sql.c_str())) {
    ctc_log_error("ctc_drop_proxy_user failed, drop user=%s, sql=%s, err_code=%d, err_msg=%s",
                  proxy_user_name.c_str(), drop_user_sql.c_str(), mysql_errno(agent_conn), mysql_error(agent_conn));
    assert(0);
  }
}

static int ctc_create_proxy_user(MYSQL *agent_conn, const char*user_name, const char*user_ip,
                                 string &proxy_user_name, string &proxy_user_password)
{
  string create_user_sql = "CREATE USER '" + proxy_user_name + \
                           "' IDENTIFIED WITH 'mysql_native_password' BY RANDOM PASSWORD;";
  int ret = ctc_mysql_query(agent_conn, create_user_sql.c_str());  // ret: success: 0, fail: 1
  
  int err_code = mysql_errno(agent_conn);
  char err_msg[ERROR_MESSAGE_LEN] = {0};
  strncpy(err_msg, mysql_error(agent_conn), ERROR_MESSAGE_LEN - 1);
  if (ret != 0 || err_code != 0) {
    ctc_log_error("ctc_create_proxy_user failed to create user %s, "
                  "sql=%s, ret=%d, err_code=%d, err_msg=%s",
                  proxy_user_name.c_str(), create_user_sql.c_str(), ret, err_code, err_msg);
    return err_code;
  }

  MYSQL_RES *result = mysql_store_result(agent_conn);
  if (result == nullptr) {
    ctc_log_error("ctc_create_proxy_user store result failed, user name:%s", user_name);
    ctc_drop_proxy_user(agent_conn, proxy_user_name);
    return -1;
  }
  
  MYSQL_ROW row = mysql_fetch_row(result);
  if (row == nullptr) {
    ctc_log_error("ctc_create_proxy_user mysql_fetch_row failed, user name:%s", user_name);
    ctc_drop_proxy_user(agent_conn, proxy_user_name);
    return -1;
  }
  
  proxy_user_password = row[2];
  mysql_free_result(result);
  
  string username(user_name);
  username = ctc_escape_single_quotation_str(username);

  string grant_user_sql = "grant proxy on '" + username +
                          "'@'" + string(user_ip) + "' to '" + proxy_user_name + "';";
  ret = ctc_mysql_query(agent_conn, grant_user_sql.c_str());
  
  err_code = mysql_errno(agent_conn);
  strncpy(err_msg, mysql_error(agent_conn), ERROR_MESSAGE_LEN - 1);
  if (ret != 0 || err_code != 0) {
    ctc_log_error("ctc_create_proxy_user failed to grant %s to %s, "
                  "sql=%s, ret=%d, err_code=%d, err_msg=%s",
                  user_name, proxy_user_name.c_str(), grant_user_sql.c_str(), ret, err_code, err_msg);
    ctc_drop_proxy_user(agent_conn, proxy_user_name);
    return err_code;
  }
  return 0;
}

static int ctc_init_proxy_client(MYSQL *&curr_conn, uint64_t conn_map_key,
                                 const char *user_name, const char *user_ip)
{
  MYSQL *agent_conn = NULL;
  int ret = ctc_init_agent_client(agent_conn);
  if (ret != 0) {
    return ret;
  }

  SENSI_INFO string proxy_user_password;
  string proxy_user_name = "proxy_" + std::to_string(conn_map_key);

  ret = ctc_create_proxy_user(agent_conn, user_name, user_ip, proxy_user_name, proxy_user_password);
  if (ret != 0) {
    ctc_log_error("ctc_init_proxy_client ctc_create_proxy_user failed, ret=%d", ret);
    ctc_close_mysql_conn(&agent_conn);
    return ret;
  }

  const char *con_host = my_localhost;
  if (strcmp(user_ip, "%") != 0) {
    con_host = user_ip;
  }
  ret = ctc_mysql_conn(curr_conn, con_host, proxy_user_name.c_str(), proxy_user_password.c_str());
  proxy_user_password.clear();
  if (ret) {
    ctc_log_error("ctc_init_proxy_client ctc_mysql_conn failed, user=%s, err_code=%d, err_msg=%s.",
                  proxy_user_name.c_str(), mysql_errno(curr_conn), mysql_error(curr_conn));
    ctc_close_mysql_conn(&curr_conn);
  }

  ctc_drop_proxy_user(agent_conn, proxy_user_name);
  ctc_close_mysql_conn(&agent_conn);
  return ret;
}

static void get_ctc_mysql_conn(uint64_t conn_map_key, MYSQL *&curr_conn) {
  lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
  auto iter = g_mysql_conn_map.find(conn_map_key);
  if (iter != g_mysql_conn_map.end()) {
    curr_conn = iter->second->conn;
  }
}

int ctc_init_mysql_client(uint64_t conn_map_key, const char *db, MYSQL *&curr_conn,
                          const char *user_name, const char *user_ip, bool use_proxy) {

  if (opt_noacl && strcmp(user_name, "skip-grants user") == 0) {
    use_proxy = false;
  }

  get_ctc_mysql_conn(conn_map_key, curr_conn);

  int ret = 0;
  if (curr_conn != NULL) {
    ret = ctc_select_db(curr_conn, db);
    if (ret == 0) {
      return 0;
    }

    ctc_log_error("ctc_init_mysql_client select db failed, err_code=%d, err_msg=%s.",
                  mysql_errno(curr_conn), mysql_error(curr_conn));
    lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
    delete g_mysql_conn_map[conn_map_key];
    g_mysql_conn_map.erase(conn_map_key);
    ctc_close_mysql_conn(&curr_conn);
  }

  assert(curr_conn == nullptr);
  while(!mysqld_server_started) {
    ctc_log_system("[CTC_INIT]:ctc_init_mysql_client wait mysql server start!!!!");
    sleep(1);
  }

  if (use_proxy) {
    ret = ctc_init_proxy_client(curr_conn, conn_map_key, user_name, user_ip);
  } else {
    ret = ctc_init_agent_client(curr_conn);
  }
  
  if (ret) {
    ctc_log_error("init mysql client failed ret=%d", ret);
    return ret;
  }

  ret = ctc_mysql_query(curr_conn, "set lock_wait_timeout = 1;");
  if (ret != 0 || mysql_errno(curr_conn) != 0) {
    ctc_log_error("set lock_wait_timeout = 1 failed. err_code:%u, err_msg:%s",
      mysql_errno(curr_conn), mysql_error(curr_conn));
  }

  ret = ctc_select_db(curr_conn, db);
  if (ret != 0) {
    ctc_log_error("ctc_init_mysql_client select db failed, err_code=%d, err_msg=%s.",
                  mysql_errno(curr_conn), mysql_error(curr_conn));
    ctc_close_mysql_conn(&curr_conn);
    return ret;
  }

  {
    lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
    g_mysql_conn_map[conn_map_key] = init_ctc_mysql_conn(curr_conn);
  }
  return 0;
}

static void close_mysql_conn_by_key(uint64_t conn_map_key) {
  /* 存在并发场景 map操作加锁 */
  lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
  auto iter = g_mysql_conn_map.find(conn_map_key);
  
  if (iter == g_mysql_conn_map.end()) {
    ctc_log_system("[CTC_CLOSE_CONN]: Connection has already been closed or not exists (key=%lu)", conn_map_key);
    return;
  }

  MYSQL *mysql_conn = iter->second->conn;
  ctc_close_mysql_conn(&mysql_conn);

  uint32_t name_locks = sub_g_name_locks(iter->second->name_locks);
  ctc_log_system("[CTC_CLOSE_CONN]: Close connect by key=%lu, current global name locks=%u", conn_map_key, name_locks);
  
  delete g_mysql_conn_map[conn_map_key];
  g_mysql_conn_map.erase(conn_map_key);
}

static MYSQL* get_mysql_conn_by_key(uint64_t conn_map_key) {
  /* 存在并发场景 map操作加锁 */
  lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
  auto iter = g_mysql_conn_map.find(conn_map_key);
  
  if (iter == g_mysql_conn_map.end()) {
    ctc_log_system("get mysql Connection has already been closed or not exists (key=%lu)", conn_map_key);
    return NULL;
  }
  return iter->second->conn;
}

static void close_mysql_conn_by_inst_id(uint32_t inst_id, bool by_mysql_inst) {
  lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
  for (auto iter = g_mysql_conn_map.begin(); iter != g_mysql_conn_map.end(); ) {
    uint32_t find_id = by_mysql_inst ? ctc_get_inst_id_from_conn_key(iter->first) :
                                       ctc_get_cantian_id_from_conn_key(iter->first);
    if (find_id == inst_id) {
        MYSQL *mysql_conn = iter->second->conn;
        ctc_close_mysql_conn(&mysql_conn);
        uint32_t name_locks = sub_g_name_locks(iter->second->name_locks);
        ctc_log_system("[CTC_CLOSE_CONN]: Close connects by mysql_id/cantian_id = %d, instance_id=%u, key=%lu,"
                       "current global name locks=%u", by_mysql_inst, inst_id, iter->first, name_locks);
        
        delete g_mysql_conn_map[iter->first];
        iter = g_mysql_conn_map.erase(iter);
    } else {
      ++iter;
    }
  }
}

static inline bool is_backup_lock_op(uint8_t sql_command) {
  return sql_command == SQLCOM_LOCK_INSTANCE ||
         sql_command == SQLCOM_UNLOCK_INSTANCE;
}

static inline bool ctc_use_proxy(uint8_t sql_command) {
  bool is_slave = ctc_get_cluster_role() == (int32_t)dis_cluster_role::STANDBY;
  ctc_log_system("[Disaster Recovery] is_slave: %d, sql_command=%d, use_proxy:%d", is_slave, sql_command, (!is_slave && !is_backup_lock_op(sql_command)));
  return !is_slave && !is_backup_lock_op(sql_command);
}

extern uint32_t ctc_instance_id;
__attribute__((visibility("default"))) int ctc_execute_rewrite_open_conn(uint32_t thd_id, ctc_ddl_broadcast_request *broadcast_req) {
  // 相同节点不用执行
  if (broadcast_req->mysql_inst_id == ctc_instance_id) {
    return 0;
  }

  bool use_proxy = ctc_use_proxy(broadcast_req->sql_command);
  uint64_t conn_map_key = ctc_get_conn_key(broadcast_req->mysql_inst_id, thd_id, use_proxy);

  MYSQL *curr_conn = NULL;
  int ret = ctc_init_mysql_client(conn_map_key, broadcast_req->db_name, curr_conn,
                                  broadcast_req->user_name, broadcast_req->user_ip, use_proxy);
  if (ret != 0) {
    broadcast_req->err_code = ret;
    ctc_log_error("[CTC_REWRITE_CONN]:init_mysql_client failed, ret:%d, conn_map_key:%lu, sql:%s", ret, conn_map_key,
      sql_without_plaintext_password(broadcast_req).c_str());
    return ret;
  }

  ctc_log_system("[CTC_REWRITE_CONN]: remote open conn for sql=%s, user_name:%s, success, mysql_inst_id=%u,"
    "conn_map_key:%lu", sql_without_plaintext_password(broadcast_req).c_str(), broadcast_req->user_name,
    broadcast_req->mysql_inst_id, conn_map_key);
  
  mysql_free_result(mysql_store_result(curr_conn));
  return 0;
}

__attribute__((visibility("default"))) int ctc_ddl_execute_update(uint32_t thd_id, ctc_ddl_broadcast_request *broadcast_req,  bool *allow_fail) {
  // 相同节点不用执行
  if(broadcast_req->mysql_inst_id == ctc_instance_id) {
    ctc_log_note("ctc_ddl_execute_update curnode not need execute,mysql_inst_id:%u", broadcast_req->mysql_inst_id);
    return 0;
  }

  if (IS_METADATA_NORMALIZATION() &&
    (broadcast_req->sql_command != SQLCOM_SET_OPTION ||
    !(broadcast_req->options & CTC_SET_VARIABLE_WITH_SUBSELECT))) {
    return 0;
  }

  bool use_proxy = ctc_use_proxy(broadcast_req->sql_command);
  uint64_t conn_map_key = ctc_get_conn_key(broadcast_req->mysql_inst_id, thd_id, use_proxy);

  MYSQL *curr_conn = NULL;
  int ret = ctc_init_mysql_client(conn_map_key, broadcast_req->db_name, curr_conn,
                                  broadcast_req->user_name, broadcast_req->user_ip, use_proxy);
  if (ret != 0) {
    broadcast_req->err_code = ret;
    ctc_log_error("[CTC_DDL]:init_mysql_client failed, ret:%d, conn_id:%u, sql_str:%s", ret, thd_id,
      sql_without_plaintext_password(broadcast_req).c_str());

    return ret;
  }

  // 设置随机密码seed
  if (broadcast_req->sql_command == SQLCOM_CREATE_USER || broadcast_req->sql_command == SQLCOM_ALTER_USER || broadcast_req->sql_command == SQLCOM_SET_PASSWORD) {
    ret = ctc_mysql_query(curr_conn, ("set @random_password_seed = " + std::to_string(thd_id) + ";").c_str());
    if (ret != 0) {
      ctc_log_error("ctc_init_proxy_client set @random_password_seed failed, error_code:%d", mysql_errno(curr_conn));
      return ret;
    }
  }

  if (broadcast_req->options & CTC_OPEN_NO_CHECK_FK_FOR_CURRENT_SQL) {
    if (ctc_mysql_query(curr_conn, "SET SESSION foreign_key_checks = 0;")) {
      ctc_log_error("ctc_init_proxy_client SET SESSION foreign_key_checks = 0; failed, error_code:%d,error:%s", mysql_errno(curr_conn), mysql_error(curr_conn));
      broadcast_req->err_code = mysql_errno(curr_conn);
      ctc_close_mysql_conn(&curr_conn);
      return broadcast_req->err_code;
    }
  }
  ret = ctc_mysql_query(curr_conn, broadcast_req->sql_str);  // ret: success: 0, fail: 1
  int error_code = mysql_errno(curr_conn);
  if (ret != 0 || error_code != 0) {
    broadcast_req->err_code = error_code;
    strncpy(broadcast_req->err_msg, mysql_error(curr_conn), ERROR_MESSAGE_LEN - 1);
    ctc_log_error("[CTC_DDL]:mysql query exectue failed. err_code:%d, err_msg:%s, sql_str:%s, user_name:%s,"
      " conn_id:%u, allow_fail:%d",error_code, broadcast_req->err_msg,
      sql_without_plaintext_password(broadcast_req).c_str(), broadcast_req->user_name, thd_id, *allow_fail);
    return broadcast_req->err_code;
  }

  if (broadcast_req->options & CTC_OPEN_NO_CHECK_FK_FOR_CURRENT_SQL) {
    if (ctc_mysql_query(curr_conn, "SET SESSION foreign_key_checks = 1;")) {
      ctc_log_error("ctc_init_proxy_client SET SESSION foreign_key_checks = 1; failed, error_code:%d,error:%s", mysql_errno(curr_conn), mysql_error(curr_conn));
      broadcast_req->err_code = mysql_errno(curr_conn);
      ctc_close_mysql_conn(&curr_conn);
      return broadcast_req->err_code;
    }
  }

  ctc_log_system("[CTC_DDL]: remote execute sql=%s, user_name:%s, success, mysql_inst_id=%u, conn_map_key:%lu",
    sql_without_plaintext_password(broadcast_req).c_str(), broadcast_req->user_name, broadcast_req->mysql_inst_id, conn_map_key);

  set_explicit_table_lock(conn_map_key, broadcast_req->sql_command);

  // 存在执行成功的节点，后续流程不再允许失败
  *allow_fail = false;
  mysql_free_result(mysql_store_result(curr_conn));
  return 0;
}

__attribute__((visibility("default"))) int ctc_ddl_execute_set_opt(uint32_t thd_id,
                                                                   ctc_set_opt_request *broadcast_req,
                                                                   bool allow_fail) {
  // 相同节点不用执行
  if (broadcast_req->mysql_inst_id == ctc_instance_id) {
    ctc_log_note("ctc_ddl_execute_set_opt curnode not need execute, mysql_inst_id:%u, conn_id:%u",
                 broadcast_req->mysql_inst_id, thd_id);
    return 0;
  }

  int ret = ctc_set_sys_var(broadcast_req);
  if (ret != 0) {
    ctc_log_note("ctc_ddl_execute_set_opt curnode execute fail, cur_mysql_inst_id:%u, conn_id:%u, allow_fail:%d",
                 ctc_instance_id, thd_id, allow_fail);
  }

  return ret;
}

__attribute__((visibility("default"))) void ctc_set_mysql_read_only() {
  ctc_log_system("[Disaster Recovecy] starting or initializing");
  super_read_only = true;
  read_only = true;
  opt_readonly = true;
  ctc_log_system("[Disaster Recovery] set super_read_only = true.");
}

__attribute__((visibility("default"))) void ctc_reset_mysql_read_only() {
  ctc_log_system("[Disaster Recovecy] starting or initializing");
  super_read_only = false;
  read_only = false;
  opt_readonly = false;
  ctc_log_system("[Disaster Recovery] set super_read_only = false.");
}

__attribute__((visibility("default"))) int ctc_set_cluster_role_by_cantian(bool is_slave) {
  lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
  if (is_slave) {
    ctc_cluster_role = (int32_t)dis_cluster_role::STANDBY;
    ctc_set_mysql_read_only();
  } else {
    ctc_cluster_role = (int32_t)dis_cluster_role::PRIMARY;
    ctc_reset_mysql_read_only();
  }
  return 0;
}

static int ctc_ddl_get_lock(MYSQL *curr_conn, const uint64_t &conn_map_key, const char *lock_name, int *err_code) {
  uchar digest[MD5_HASH_SIZE];
  compute_md5_hash(pointer_cast<char *>(digest), lock_name, strlen(lock_name));

  // + 1 for the null terminator
  char output[(MD5_HASH_SIZE * 2) + 1];
  array_to_hex(output, digest, MD5_HASH_SIZE);
  output[(MD5_HASH_SIZE * 2)] = '\0';

  string lock_function_str = string("SELECT GET_LOCK('") + output + string("', 0);");
  int ret = ctc_mysql_query(curr_conn, lock_function_str.c_str());
  *err_code = mysql_errno(curr_conn);
  if (ret != 0 || *err_code != 0) {
    ctc_log_error("[CTC_LOCK_TABLE]:execute GET_LOCK() failed, "
      "return_err: %d, err_code:%d, err_msg:%s, lock_function_str:%s",
      ret, *err_code, mysql_error(curr_conn), lock_function_str.c_str());
    return *err_code;
  }

  MYSQL_RES *query_res = mysql_store_result(curr_conn);
  if (query_res == nullptr) {
    ctc_log_error("[CTC_LOCK_TABLE]:execute GET_LOCK() failed to store result, lock_name:%s, err_msg:%s",
                  lock_name, mysql_error(curr_conn));
    return 1;
  }

  MYSQL_ROW row_res = mysql_fetch_row(query_res);
  /* If the return value of get_lock() is 0, obtaining the lock times out. */
  if (atoi(*row_res) == 0) {
    *err_code = ER_DISALLOWED_OPERATION;
    mysql_free_result(query_res);
    return -1;
  }
  mysql_free_result(query_res);

  {
    lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
    auto iter = g_mysql_conn_map.find(conn_map_key);
    if (iter != g_mysql_conn_map.end()) {
      iter->second->name_locks++;
      uint32_t name_locks = add_g_name_locks(1);
      ctc_log_note("[CTC_LOCK_TABLE]: conn key=%lu, its name locks=%u, current global name locks=%u",
                   conn_map_key, iter->second->name_locks, name_locks);
    }
  }
  return 0;
}

int32_t ctc_check_table_exist(MYSQL *curr_conn_proxy, const char *db_name, const char *table_name, int *err_code) {
  string sql_str = "SELECT EXISTS (SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA LIKE '" +
                   string(db_name) + "' AND TABLE_NAME = '" + string(table_name) + "');";

  MYSQL_RES *query_res = nullptr;
  if (ctc_mysql_query(curr_conn_proxy, sql_str.c_str()) || !(query_res = mysql_store_result(curr_conn_proxy))) {
    *err_code = mysql_errno(curr_conn_proxy);
    ctc_log_error("[CTC_LOCK_TABLE]:err_msg:%s, db:%s, table:%s", mysql_error(curr_conn_proxy), db_name, table_name);
    return -1;
  }

  MYSQL_ROW row_res = mysql_fetch_row(query_res);
  int32_t res = atoi(*row_res);

  mysql_free_result(query_res);
  return res;
}

__attribute__((visibility("default"))) int ctc_ddl_execute_lock_tables(ctc_handler_t *tch,
                                                                       char *db_name,
                                                                       ctc_lock_table_info *lock_info,
                                                                       int *err_code)
{
  if (IS_METADATA_NORMALIZATION()) {
    if (lock_info->sql_type == SQLCOM_LOCK_TABLES) {
      if (ctc_ddl_execute_lock_tables_by_req(tch, lock_info, err_code)) {
        return *err_code;
      }
    } else if (ctc_mdl_lock_thd(tch, lock_info, err_code)) {
      return *err_code;
    }
    return 0;
  }

  bool is_same_node = (tch->inst_id == ctc_instance_id);
  uint64_t conn_map_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, !is_same_node);

  MYSQL *curr_conn = NULL;
  int ret = ctc_init_mysql_client(conn_map_key, db_name, curr_conn, lock_info->user_name, lock_info->user_ip, !is_same_node);
  if (ret != 0) {
    *err_code = ret;
    ctc_log_error("[CTC_LOCK_TABLE]:ctc_init_mysql_client failed, ret:%d, conn_id:%u, ctc_instance_id:%u",
        ret, tch->thd_id, tch->inst_id);
    return ret;
  }

  /* Operations on the Database, only need to lock the database. */
  if (strlen(lock_info->table_name) == 0) {
    return ctc_ddl_get_lock(curr_conn, conn_map_key, lock_info->db_name, err_code);
  }
  
  string lock_name_str;
  lock_name_str.append(lock_info->db_name);
  lock_name_str.append(".");
  lock_name_str.append(lock_info->table_name);
  ret = ctc_ddl_get_lock(curr_conn, conn_map_key, lock_name_str.c_str(), err_code);
  if (ret != 0) {
    return ret;
  }

  /* Do not run lock_table on the same node. */
  if(is_same_node) {
    ctc_log_note("[CTC_LOCK_TABLE]:curnode not need execute, mysql_inst_id:%u", tch->inst_id);
    return 0;
  }

  if (lock_info->sql_type == SQLCOM_CREATE_TABLE ||
      lock_info->sql_type == SQLCOM_DROP_VIEW ||
      lock_info->sql_type == SQLCOM_CREATE_VIEW) {
    ctc_log_note("[CTC_LOCK_TABLE]:Skip lock_table. sql_cmd:%d", lock_info->sql_type);
    return 0;
  }

  if (g_mysql_conn_map[conn_map_key]->has_explicit_table_lock) {
    ctc_log_system("[CTC_LOCK_TABLE]: curnode doesn't need execute, conn_map_key=%lu, mysql_thd_id=%u, mysql_inst_id=%u",
                   conn_map_key, tch->thd_id, tch->inst_id);
    return ret;
  }

  int32_t row_res = ctc_check_table_exist(curr_conn, lock_info->db_name, lock_info->table_name, err_code);
  if (row_res > 0) {
    lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
    g_mysql_conn_map[conn_map_key]->table_lock_info.insert(make_pair(lock_info->db_name, lock_info->table_name));
  }

  string lock_str = "lock tables ";
  {
    lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
    for (auto iter : g_mysql_conn_map[conn_map_key]->table_lock_info) {
      row_res = ctc_check_table_exist(curr_conn, iter.first.c_str(), iter.second.c_str(), err_code);
      if (row_res <= 0) {
        ctc_log_warning("[CTC_LOCK_TABLE]:Table not exist. row_res:%d, db:%s, table:%s", row_res, iter.first.c_str(), iter.second.c_str());
        return 0;
      }

      string db = cnvrt_name_for_sql(iter.first);
      string table = cnvrt_name_for_sql(iter.second);
      lock_str += "`" + db + "`.`" + table + "` write, ";
    }
  }
  lock_str.erase(lock_str.end() - 2); /* 2 is comma and space len in string end */
  lock_str += ";";

  if (g_mysql_conn_map[conn_map_key]->table_lock_info.size() > 0) {
    ret = ctc_mysql_query(curr_conn, lock_str.c_str());
    if (ret != 0 || mysql_errno(curr_conn) != 0) {
      *err_code = mysql_errno(curr_conn);
      ctc_log_error("[CTC_LOCK_TABLE]:return_err:%d, err_code:%d, err_msg:%s, lock_sql_str:%s, conn_id:%u, ctc_instance_id:%u",
        ret, *err_code, mysql_error(curr_conn), lock_str.c_str(), tch->thd_id, tch->inst_id);
    }
  }

  return ret;
}

__attribute__((visibility("default"))) int ctc_ddl_execute_unlock_tables(ctc_handler_t *tch, uint32_t mysql_inst_id, ctc_lock_table_info *lock_info)
{
  if (IS_METADATA_NORMALIZATION()) {
    UNUSED_PARAM(mysql_inst_id);
    if (lock_info->sql_type == SQLCOM_UNLOCK_TABLES) {
      ctc_mdl_unlock_tables_thd(tch);
    }
    ctc_mdl_unlock_thd(tch, lock_info);
    return 0;
  }

  bool is_same_node = (tch->inst_id == ctc_instance_id);
  uint64_t conn_map_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, !is_same_node);
  MYSQL *curr_conn = get_mysql_conn_by_key(conn_map_key);
  if (curr_conn == NULL) {
    ctc_log_system("[CTC_UNLOCK_TABLE]: curr_conn is NULL, conn_map_key=%lu, conn_id=%u, ctc_instance_id=%u",
                   conn_map_key, tch->thd_id, tch->inst_id);
    return 0;
  }

  /* Releases all named locks held by the current session */
  if (ctc_mysql_query(curr_conn, "SELECT RELEASE_ALL_LOCKS();")) {
    ctc_log_error("[CTC_UNLOCK_TABLE]: RELEASE_ALL_LOCKS failed, close conn, conn_id=%u, err_code=%d, err_msg=%s",
                  tch->thd_id, mysql_errno(curr_conn), mysql_error(curr_conn));
    close_mysql_conn_by_key(conn_map_key);
    return 0;
  }

  mysql_free_result(mysql_store_result(curr_conn));
  
  {
    // 清空加锁信息
    lock_guard<mutex> lock(m_ctc_mysql_proxy_mutex);
    auto iter = g_mysql_conn_map.find(conn_map_key);
    if (iter != g_mysql_conn_map.end()) {
      iter->second->table_lock_info.clear();
      if (iter->second->name_locks > 0) {
        iter->second->name_locks--;
        uint32_t name_locks = sub_g_name_locks(1);
        ctc_log_note("[CTC_LOCK_TABLE]: conn key=%lu, its name locks=%u, current global name locks=%u",
                     conn_map_key, iter->second->name_locks, name_locks);
      }
    }
  }

  /* Do not run unlock_table on the same node. */
  if(is_same_node) {
    close_mysql_conn_by_key(conn_map_key);
    ctc_log_note("[CTC_UNLOCK_TABLE]: curnode doesn't need execute, conn_map_key=%lu, mysql_inst_id=%u", conn_map_key, tch->inst_id);
    return 0;
  }

  if (g_mysql_conn_map[conn_map_key]->has_explicit_table_lock) {
    ctc_log_system("[CTC_UNLOCK_TABLE]: curnode doesn't need execute, conn_map_key=%lu, mysql_thd_id=%u, mysql_inst_id=%u", conn_map_key, tch->thd_id, tch->inst_id);
    return 0;
  }

  if (ctc_mysql_query(curr_conn, "UNLOCK TABLES;")) {
    ctc_log_error("[CTC_UNLOCK_TABLE]: UNLOCK TABLES failed, close conn, conn_id=%u, err_code=%d, err_msg=%s",
                  tch->thd_id, mysql_errno(curr_conn), mysql_error(curr_conn));
    close_mysql_conn_by_key(conn_map_key);
    return 0;
  }
  mysql_free_result(mysql_store_result(curr_conn));
  return 0;
}

/* thd_id为0时，关闭实例id为mysql_inst_id的所有连接
*  mysql_inst_id:  高16位 --->  参天实例id
*                  低16位 --->  mysqld实例id, 由当前节点参天分配，取值范围(2-19)
*                  低16位全为1代表整个参天节点故障，清理与参天实例id相关的资源
*/
__attribute__((visibility("default"))) int close_mysql_connection(uint32_t thd_id, uint32_t mysql_inst_id) {
  if (IS_METADATA_NORMALIZATION()) {
    close_ctc_mdl_thd(thd_id, mysql_inst_id);
    return 0;
  }

  if (thd_id == 0) {
    if ((uint16_t)mysql_inst_id == (uint16_t)CANTIAN_DOWN_MASK) {
      /* 清理整个参天节点相关的连接 */
      ctc_log_system("[CTC_CLOSE_SESSION]:Close All connects on bad node by cantian_instance_id:%u",
        (mysql_inst_id >> 16));
      close_mysql_conn_by_inst_id((mysql_inst_id >> 16), false);
    } else {
      /* 清理整个mysqld节点相关的连接 */
      ctc_log_system("[CTC_CLOSE_SESSION]:Close All connects by ctc_instance_id:%u", mysql_inst_id);
      close_mysql_conn_by_inst_id(mysql_inst_id, true);
    }
  } else {
    /* 通过把mysql_inst_id左移32位 与 thd_id拼接在一起 用来唯一标识一个连接 */
    uint64_t proxy_conn_map_key = ctc_get_conn_key(mysql_inst_id, thd_id, true);
    ctc_log_note("[CTC_CLOSE_SESSION]: Close connect by conn_id=%u, ctc_instance_id=%u, proxy_conn_map_key=%lu",
                   thd_id, mysql_inst_id, proxy_conn_map_key);
    close_mysql_conn_by_key(proxy_conn_map_key);
    
    uint64_t agent_conn_map_key = ctc_get_conn_key(mysql_inst_id, thd_id, false);
    ctc_log_note("[CTC_CLOSE_SESSION]: Close connect by conn_id=%u, ctc_instance_id=%u, agent_conn_map_key=%lu",
                    thd_id, mysql_inst_id, agent_conn_map_key);
    close_mysql_conn_by_key(agent_conn_map_key);
  }
  return 0;
}

__attribute__((visibility("default"))) int ctc_invalidate_mysql_dd_cache(ctc_handler_t *tch,
                                                                         ctc_invalidate_broadcast_request *broadcast_req,
                                                                         int *err_code)
{
  return ctc_invalidate_mysql_dd_cache_req(tch, broadcast_req, err_code);
}