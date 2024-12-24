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
#include <ctype.h>
#include <fcntl.h>
#include <mysql/plugin.h>
#include <mysql_version.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <regex>

#include "m_string.h"  // strlen
#include "my_dbug.h"
#include "my_dir.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "my_sys.h"  // my_write, my_malloc
#include "my_thread.h"
#include "mysql/psi/mysql_memory.h"
#include "sql/sql_plugin.h"  // st_plugin_int
#include "sql/sql_initialize.h" // opt_initialize_insecure
#include "ctc_log.h"
#include "ha_ctc.h"
#include "decimal_convert.h"

struct mysql_cantian_context {
  my_thread_handle cantian_startup_thread;
};

extern "C" {
const char *cantiand_get_dbversion()
{
  return "NONE";
}
}

extern "C" int cantiand_lib_main(int argc, char *argv[]);
#ifdef WITH_CANTIAN
extern "C" int register_sql_intf(sql_engine_intf *sql_intf);
#endif
extern "C" void ct_singlep_shutdown();

static std::string get_cantiand_mode() {
  std::string mode = "nomount";
  char *tmp_mode = getenv("CANTIAND_MODE");
  if (tmp_mode != NULL && strlen(tmp_mode) > 0) {
    mode = tmp_mode;
  }

  return mode;
}

static std::string get_cantiand_home_dir() {
  std::string home_dir = "/home/regress/data";
  char *tmp_home_dir = getenv("CANTIAND_HOME_DIR");
  if (tmp_home_dir != NULL && strlen(tmp_home_dir) > 0) {
    home_dir = tmp_home_dir;
  }
  ctc_log_system("get cantiand home_dir:%s", home_dir.c_str());
  return home_dir;
}

#ifdef WITH_CANTIAN
static sql_engine_intf g_local_sql_intf;
sql_engine_intf *get_local_sql_intf()
{
    g_local_sql_intf.ctc_invalidate_mysql_dd_cache = ctc_invalidate_mysql_dd_cache;
    g_local_sql_intf.ctc_execute_rewrite_open_conn = ctc_execute_rewrite_open_conn;
    g_local_sql_intf.ctc_ddl_execute_update = ctc_ddl_execute_update;
    g_local_sql_intf.ctc_ddl_execute_set_opt = ctc_ddl_execute_set_opt;
    g_local_sql_intf.close_mysql_connection = close_mysql_connection;
    g_local_sql_intf.ctc_ddl_execute_lock_tables = ctc_ddl_execute_lock_tables;
    g_local_sql_intf.ctc_ddl_execute_unlock_tables = ctc_ddl_execute_unlock_tables;
    g_local_sql_intf.ctc_set_cluster_role_by_cantian = ctc_set_cluster_role_by_cantian;
    ctc_log_system("get local sql function success.");
    return &g_local_sql_intf;
}
#endif

static void *mysql_cantian_startup_thread(void *p) {
  DBUG_TRACE;
  struct mysql_cantian_context *con = (struct mysql_cantian_context *)p;
  if(con->cantian_startup_thread.thread == 0) {
    ctc_log_error("please create the nomont thread first!");
    return nullptr;
  }

  std::string mode = get_cantiand_mode();
  std::string home_dir = get_cantiand_home_dir();
  int ret;
#ifdef WITH_CANTIAN
  ret = register_sql_intf(get_local_sql_intf());
  if (ret != CT_SUCCESS) {
    ctc_log_error("register_sql_intf error");
    return nullptr;
  }
#endif
  if (mode != "open") {
    char const *argv[] = {"/home/regress/install/bin/cantiand", mode.c_str(),
                          "-D", home_dir.c_str()};
    ret = cantiand_lib_main(4, const_cast<char **>(argv));
  } else {
    char const *argv[] = {"/home/regress/install/bin/cantiand", "-D",
                          home_dir.c_str()};
    ret = cantiand_lib_main(3, const_cast<char **>(argv));
  }
  ctc_log_system("init cantian mode:%s,home_dir:%s, ret:%d", mode.c_str(),
                 home_dir.c_str(), ret);
  return nullptr;
}

struct mysql_cantian_context *cantian_context = NULL;
int daemon_cantian_plugin_init() {
  DBUG_TRACE;

  // mysql with nometa does not need to start cantian startup thread in multiple process when initializing
  // but single process needs to start up cantian thread in both meat and nometa when initializing
  if (!is_single_run_mode())  {
    if (opt_initialize_insecure) {
      ctc_log_warning("initialize-insecure mode no need start the cantian startup thread.");
      return 0;
    }

    const char *se_name = "ctc_ddl_rewriter";
    const LEX_CSTRING name = {se_name, strlen(se_name)};
    if (!plugin_is_ready(name, MYSQL_AUDIT_PLUGIN)) {
      ctc_log_error("ctc_ddl_rewriter plugin install failed.");
      return -1;
    }
  }
  
  if (cantian_context != NULL) {
    ctc_log_error("daemon_cantian_plugin_init cantian_context:%p not NULL", cantian_context);
    return 0;
  }

  cantian_context = (struct mysql_cantian_context *)my_malloc(
    PSI_NOT_INSTRUMENTED,
    sizeof(struct mysql_cantian_context), MYF(0));
  if (cantian_context == nullptr) {
    ctc_log_error("alloc mem failed, cantian_context size(%lu)", sizeof(struct mysql_cantian_context));
    return -1;
  }
  my_thread_attr_t startup_attr; /* Thread attributes */
  my_thread_attr_init(&startup_attr);
  my_thread_attr_setdetachstate(&startup_attr, MY_THREAD_CREATE_JOINABLE);
  /* now create the startup thread */
  if (my_thread_create(&cantian_context->cantian_startup_thread, &startup_attr, mysql_cantian_startup_thread,
                       (void *)cantian_context) != 0) {
    ctc_log_error("Could not create cantian startup thread!");
    return -1;
  }
  return 0;
}

int daemon_cantian_plugin_deinit() {
  DBUG_TRACE;
  void *dummy_retval;

  if (cantian_context == nullptr || cantian_context->cantian_startup_thread.thread == 0) {
    ctc_log_system("startup thread not started");
    return 0;
  }

  ct_singlep_shutdown();
  my_thread_join(&cantian_context->cantian_startup_thread, &dummy_retval);
  my_free(cantian_context);
  cantian_context = NULL;
  return 0;
}

int (*ctc_init)() = daemon_cantian_plugin_init;
int (*ctc_deinit)() = daemon_cantian_plugin_deinit;