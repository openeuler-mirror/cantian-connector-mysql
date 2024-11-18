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
#include "ctc_stats.h"
#include "ctc_log.h"
#include <sstream>

const char *ctc_interface_strs[] = {
  "CTC_FUNC_TYPE_OPEN_TABLE",
  "CTC_FUNC_TYPE_CLOSE_TABLE",
  "CTC_FUNC_TYPE_CLOSE_SESSION",
  "CTC_FUNC_TYPE_WRITE_ROW",
  "CTC_FUNC_TYPE_UPDATE_JOB",
  "CTC_FUNC_TYPE_UPDATE_ROW",
  "CTC_FUNC_TYPE_DELETE_ROW",
  "CTC_FUNC_TYPE_RND_INIT",
  "CTC_FUNC_TYPE_RND_END",
  "CTC_FUNC_TYPE_RND_NEXT",
  "CTC_FUNC_TYPE_RND_PREFETCH",
  "CTC_FUNC_TYPE_SCAN_RECORDS",
  "CTC_FUNC_TYPE_TRX_COMMIT",
  "CTC_FUNC_TYPE_TRX_ROLLBACK",
  "CTC_FUNC_TYPE_TRX_BEGIN",
  "CTC_FUNC_TYPE_LOCK_TABLE",
  "CTC_FUNC_TYPE_UNLOCK_TABLE",
  "CTC_FUNC_TYPE_INDEX_END",
  "CTC_FUNC_TYPE_SRV_SET_SAVEPOINT",
  "CTC_FUNC_TYPE_SRV_ROLLBACK_SAVEPOINT",
  "CTC_FUNC_TYPE_SRV_RELEASE_SAVEPOINT",
  "CTC_FUNC_TYPE_GENERAL_FETCH",
  "CTC_FUNC_TYPE_GENERAL_PREFETCH",
  "CTC_FUNC_TYPE_FREE_CURSORS",
  "CTC_FUNC_TYPE_GET_INDEX_NAME",
  "CTC_FUNC_TYPE_INDEX_READ",
  "CTC_FUNC_TYPE_RND_POS",
  "CTC_FUNC_TYPE_POSITION",
  "CTC_FUNC_TYPE_DELETE_ALL_ROWS",
  "CTC_FUNC_TYPE_GET_CBO_STATS",
  "CTC_FUNC_TYPE_WRITE_LOB",
  "CTC_FUNC_TYPE_READ_LOB",
  "CTC_FUNC_TYPE_CREATE_TABLE",
  "CTC_FUNC_TYPE_TRUNCATE_TABLE",
  "CTC_FUNC_TYPE_TRUNCATE_PARTITION",
  "CTC_FUNC_TYPE_RENAME_TABLE",
  "CTC_FUNC_TYPE_ALTER_TABLE",
  "CTC_FUNC_TYPE_GET_SERIAL_VALUE",
  "CTC_FUNC_TYPE_DROP_TABLE",
  "CTC_FUNC_TYPE_EXCUTE_MYSQL_DDL_SQL",
  "CTC_FUNC_TYPE_BROADCAST_REWRITE_SQL",
  "CTC_FUNC_TYPE_CREATE_TABLESPACE",
  "CTC_FUNC_TYPE_ALTER_TABLESPACE",
  "CTC_FUNC_TYPE_DROP_TABLESPACE",
  "CTC_FUNC_TYPE_BULK_INSERT",
  "CTC_FUNC_TYPE_ANALYZE",
  "CTC_FUNC_TYPE_GET_MAX_SESSIONS",
  "CTC_FUNC_LOCK_INSTANCE",
  "CTC_FUNC_UNLOCK_INSTANCE",
  "CTC_FUNC_CHECK_TABLE_EXIST",
  "CTC_FUNC_SEARCH_METADATA_SWITCH",
  "CTC_FUNC_QUERY_SHM_USAGE",
  "CTC_FUNC_QUERY_CLUSTER_ROLE",
  "CTC_FUNC_SET_CLUSTER_ROLE_BY_CANTIAN",
  "CTC_FUNC_PRE_CREATE_DB",
  "CTC_FUNC_TYPE_DROP_TABLESPACE_AND_USER",
  "CTC_FUNC_DROP_DB_PRE_CHECK",
  "CTC_FUNC_KILL_CONNECTION",
  "CTC_FUNC_TYPE_INVALIDATE_OBJECT",
  "CTC_FUNC_TYPE_RECORD_SQL",
  "CTC_FUNC_TYPE_REGISTER_INSTANCE",
  "CTC_FUNC_QUERY_SHM_FILE_NUM",
  "CTC_FUNC_TYPE_WAIT_CONNETOR_STARTUPED",
  "CTC_FUNC_TYPE_MYSQL_EXECUTE_UPDATE",
  "CTC_FUNC_TYPE_CLOSE_MYSQL_CONNECTION",
  "CTC_FUNC_TYPE_LOCK_TABLES",
  "CTC_FUNC_TYPE_UNLOCK_TABLES",
  "CTC_FUNC_TYPE_EXECUTE_REWRITE_OPEN_CONN",
  "CTC_FUNC_TYPE_INVALIDATE_OBJECTS",
  "CTC_FUNC_TYPE_INVALIDATE_ALL_OBJECTS",
  "CTC_FUNC_TYPE_UPDATE_DDCACHE",

};

#ifndef WITH_CANTIAN
typedef struct tag_mem_class_cfg_s {
    uint32_t size; // align to 8 bytes
    uint32_t num;
} mem_class_cfg_t;

mem_class_cfg_t g_mem_class_cfg[MEM_CLASS_NUM] = {
    {8,       16000},
    {16,      16000},
    {32,      16000},
    {40,      16000},
    {48,      16000},
    {56,      16000},
    {64,      16000},
    {128,     16000},
    {256,     16000},
    {384,     8000},
    {512,     400},
    {1024,    400},
    {2048,    400},
    {4096,    400},
    {8192,    400},
    {12288,   1600},
    {16384,   1200},
    {40960,   4000},
    {65536,   14000},
    {66632,   20000},
    {82224,   1000},
    {102400,  800},
    {204800,  800},
    {491520,  800},
    {1048576, 40},
    {2097152, 100},
    {4194304, 200},
};
#endif

ctc_stats& ctc_stats::get_instance() noexcept {
  static ctc_stats m_ctc_stats;
  return m_ctc_stats;
}

bool ctc_stats::get_statistics_enabled() {
  return m_statistics_enabled;
}

void ctc_stats::set_statistics_enabled(const bool val) {
  if (val && !m_statistics_enabled) {
    for (int i = 0; i < CTC_FUNC_TYPE_NUMBER; i++) {
      m_calls[i] = 0;
      m_use_time[i] = 0;
    }
  }
  
  m_statistics_enabled = val;
}

void ctc_stats::gather_stats(const enum CTC_FUNC_TYPE& type, const uint64_t use_time) {
  m_calls[type]++;
  m_use_time[type] += use_time;
}

void ctc_stats::print_cost_times(std::string &ctc_srv_monitor_str) {
  if ((sizeof(ctc_interface_strs) / sizeof(ctc_interface_strs[0])) != CTC_FUNC_TYPE_NUMBER) {
    ctc_srv_monitor_str += "[CTC_STATS]: ctc_interface_strs number must be same as total ctc interfaces.\n";
    return;
  }

  ctc_srv_monitor_str += "\n======================================CTC_STATS=====================================\n";
  ctc_srv_monitor_str += "Interface:   Call counter    Used Time    Average Time\n";
  for (int i = 0; i < CTC_FUNC_TYPE_NUMBER; i++) {
    uint64_t calls = m_calls[i];
    uint64_t use_time = m_use_time[i];
    if (calls == 0) {
      continue;
    }

    double average_time = (double) use_time / calls;
    ctc_srv_monitor_str += ctc_interface_strs[i];
    ctc_srv_monitor_str += ":   " + std::to_string(calls) + "   " + std::to_string(use_time) + "   "+  std::to_string(average_time)+"\n";
  }

  ctc_srv_monitor_str += "\n======================================CTC_STATS=====================================\n";
}

#ifndef WITH_CANTIAN
extern uint32_t g_shm_file_num;
void ctc_stats::print_shm_usage(std::string &ctc_srv_monitor_str) {
  uint32_t *ctc_shm_usage = (uint32_t *)my_malloc(PSI_NOT_INSTRUMENTED, (g_shm_file_num + 1) * MEM_CLASS_NUM * sizeof(uint32_t), MYF(MY_WME));
  if (ctc_get_shm_usage(ctc_shm_usage) != CT_SUCCESS) {
    my_free(ctc_shm_usage);
    return;
  }
  ctc_srv_monitor_str += "\n=====================================SHARE MEMORY USAGE STATISTICS=====================================\n";
  ctc_srv_monitor_str += "SIZE:\t" ;
  for (uint32_t j = 0; j < MEM_CLASS_NUM; j++) {
    ctc_srv_monitor_str += std::to_string(g_mem_class_cfg[j].size) + "\t" ;
  }
  ctc_srv_monitor_str += "\nNUM:\t";
  for (uint32_t j = 0; j < MEM_CLASS_NUM; j++) {
    ctc_srv_monitor_str += std::to_string(g_mem_class_cfg[j].num) + "\t" ;
  }
  ctc_srv_monitor_str += "\n------------------------------------------------------------------------------------------------------\n";

  int idx = 0;
  for (uint32_t i = 0; i < g_shm_file_num + 1; i++) {
    ctc_srv_monitor_str += "FILE" + std::to_string(i)  + ":\t" ;
    for (uint32_t j = 0; j < MEM_CLASS_NUM; j++) {
      ctc_srv_monitor_str += std::to_string(ctc_shm_usage[idx++]) + "\t" ; // ctc_shm_usage[idx++] / g_mem_class_cfg[j].num
    }
    ctc_srv_monitor_str += "\n";
  }
  my_free(ctc_shm_usage);
}
#endif

void ctc_stats::print_stats(THD *thd, stat_print_fn *stat_print) {
  char *ctc_srv_monitor;
  std::string ctc_srv_monitor_str = "";

  if (get_statistics_enabled()) {
    print_cost_times(ctc_srv_monitor_str);
  }
#ifndef WITH_CANTIAN
  print_shm_usage(ctc_srv_monitor_str);
#endif
  
  ctc_srv_monitor = &ctc_srv_monitor_str[0];
  stat_print(thd, "ctc", static_cast<uint>(strlen("ctc")), STRING_WITH_LEN(""),
             ctc_srv_monitor, (uint)ctc_srv_monitor_str.length());
}