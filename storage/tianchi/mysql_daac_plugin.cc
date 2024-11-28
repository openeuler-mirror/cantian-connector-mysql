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

#include "my_global.h"
#include "m_string.h"  // strlen
#include "my_dbug.h"
#include "my_dir.h"
#include <cstdint>
#include "my_io.h"
//#include "my_psi_config.h"
#include "my_sys.h"  // my_write, my_malloc
//#include "my_thread.h"
#include "mysql/psi/mysql_memory.h"
#include "sql/sql_plugin.h"  // st_plugin_int
//#include "sql/sql_initialize.h" // opt_initialize_insecure
#include "tse_log.h"
#include "ha_tse.h"
#include "decimal_convert.h"

struct mysql_daac_context {
  my_thread_handle daac_startup_thread;
};

extern "C" {
const char *cantiand_get_dbversion()
{
  return "NONE";
}
}

extern "C" int cantiand_lib_main(int argc, char *argv[]);
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
  tse_log_system("get cantiand home_dir:%s", home_dir.c_str());
  return home_dir;
}

static void *mysql_daac_startup_thread(void *p) {
  
  struct mysql_daac_context *con = (struct mysql_daac_context *)p;
  if(con->daac_startup_thread.thread == 0) {
    tse_log_error("please create the nomont thread first!");
    return nullptr;
  }

  std::string mode = get_cantiand_mode();
  std::string home_dir = get_cantiand_home_dir();
  int ret;
  if (mode != "open") {
    char const *argv[] = {"/home/regress/install/bin/cantiand", mode.c_str(),
                          "-D", home_dir.c_str()};
    ret = cantiand_lib_main(4, const_cast<char **>(argv));
  } else {
    char const *argv[] = {"/home/regress/install/bin/cantiand", "-D",
                          home_dir.c_str()};
    ret = cantiand_lib_main(3, const_cast<char **>(argv));
  }
  tse_log_system("init daac mode:%s,home_dir:%s, ret:%d", mode.c_str(),
                 home_dir.c_str(), ret);
  return nullptr;
}

struct mysql_daac_context *daac_context = NULL;
int daemon_daac_plugin_init() {

  // mysql with nometa does not need to start cantian startup thread in multiple process when initializing
  // but single process needs to start up cantian thread in both meat and nometa when initializing
  if (!is_single_run_mode())  {
    if (opt_initialize_insecure) {
      tse_log_warning("initialize-insecure mode no need start the daac startup thread.");
      return 0;
    }

    const char *se_name = "ctc_ddl_rewriter";
    const LEX_CSTRING name = {se_name, strlen(se_name)};
    if (!plugin_is_ready(name, MYSQL_AUDIT_PLUGIN)) {
      tse_log_error("tse_ddl_rewriter plugin install failed.");
      return -1;
    }
  }
  
  if (daac_context != NULL) {
    tse_log_error("daemon_daac_plugin_init daac_context:%p not NULL", daac_context);
    return 0;
  }

  daac_context = (struct mysql_daac_context *)my_malloc(
    PSI_NOT_INSTRUMENTED,
    sizeof(struct mysql_daac_context), MYF(0));
  if (daac_context == nullptr) {
    tse_log_error("alloc mem failed, daac_context size(%lu)", sizeof(struct mysql_daac_context));
    return -1;
  }
  my_thread_attr_t startup_attr; /* Thread attributes */
  my_thread_attr_init(&startup_attr);
  my_thread_attr_setdetachstate(&startup_attr, MY_THREAD_CREATE_JOINABLE);
  /* now create the startup thread */
  if (my_thread_create(&daac_context->daac_startup_thread, &startup_attr, mysql_daac_startup_thread,
                       (void *)daac_context) != 0) {
    tse_log_error("Could not create daac startup thread!");
    return -1;
  }
  return 0;
}

int daemon_daac_plugin_deinit() {
  
  void *dummy_retval;

  if (daac_context == nullptr || daac_context->daac_startup_thread.thread == 0) {
    tse_log_system("startup thread not started");
    return 0;
  }

  ct_singlep_shutdown();
  my_thread_join(&daac_context->daac_startup_thread, &dummy_retval);
  my_free(daac_context);
  daac_context = NULL;
  return 0;
}

int (*tse_init)() = daemon_daac_plugin_init;
int (*tse_deinit)() = daemon_daac_plugin_deinit;
