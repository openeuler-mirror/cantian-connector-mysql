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
#include "tse_stats.h"
#include "tse_log.h"
#include <sstream>

const char *ctc_interface_strs[] = {
  "TSE_FUNC_TYPE_OPEN_TABLE",
  "TSE_FUNC_TYPE_CLOSE_TABLE",
  "TSE_FUNC_TYPE_CLOSE_SESSION",
  "TSE_FUNC_TYPE_WRITE_ROW",
  "TSE_FUNC_TYPE_WRITE_THROUGH_ROW",
  "TSE_FUNC_TYPE_UPDATE_ROW",
  "TSE_FUNC_TYPE_DELETE_ROW",
  "TSE_FUNC_TYPE_RND_INIT",
  "TSE_FUNC_TYPE_RND_END",
  "TSE_FUNC_TYPE_RND_NEXT",
  "TSE_FUNC_TYPE_RND_PREFETCH",
  "TSE_FUNC_TYPE_SCAN_RECORDS",
  "TSE_FUNC_TYPE_TRX_COMMIT",
  "TSE_FUNC_TYPE_TRX_ROLLBACK",
  "TSE_FUNC_TYPE_TRX_BEGIN",
  "TSE_FUNC_TYPE_LOCK_TABLE",
  "TSE_FUNC_TYPE_UNLOCK_TABLE",
  "TSE_FUNC_TYPE_INDEX_END",
  "TSE_FUNC_TYPE_SRV_SET_SAVEPOINT",
  "TSE_FUNC_TYPE_SRV_ROLLBACK_SAVEPOINT",
  "TSE_FUNC_TYPE_SRV_RELEASE_SAVEPOINT",
  "TSE_FUNC_TYPE_GENERAL_FETCH",
  "TSE_FUNC_TYPE_GENERAL_PREFETCH",
  "TSE_FUNC_TYPE_FREE_CURSORS",
  "TSE_FUNC_TYPE_GET_INDEX_NAME",
  "TSE_FUNC_TYPE_INDEX_READ",
  "TSE_FUNC_TYPE_RND_POS",
  "TSE_FUNC_TYPE_POSITION",
  "TSE_FUNC_TYPE_DELETE_ALL_ROWS",
  "TSE_FUNC_TYPE_GET_CBO_STATS",
  "TSE_FUNC_TYPE_GET_HUGE_PART_TABLE_CBO_STATS",
  "TSE_FUNC_TYPE_WRITE_LOB",
  "TSE_FUNC_TYPE_READ_LOB",
  "TSE_FUNC_TYPE_CREATE_TABLE",
  "TSE_FUNC_TYPE_TRUNCATE_TABLE",
  "TSE_FUNC_TYPE_TRUNCATE_PARTITION",
  "TSE_FUNC_TYPE_RENAME_TABLE",
  "TSE_FUNC_TYPE_ALTER_TABLE",
  "TSE_FUNC_TYPE_GET_SERIAL_VALUE",
  "TSE_FUNC_TYPE_DROP_TABLE",
  "TSE_FUNC_TYPE_EXCUTE_MYSQL_DDL_SQL",
  "TSE_FUNC_TYPE_BROADCAST_REWRITE_SQL",
  "TSE_FUNC_TYPE_CREATE_TABLESPACE",
  "TSE_FUNC_TYPE_ALTER_TABLESPACE",
  "TSE_FUNC_TYPE_DROP_TABLESPACE",
  "TSE_FUNC_TYPE_BULK_INSERT",
  "TSE_FUNC_TYPE_ANALYZE",
  "TSE_FUNC_TYPE_GET_MAX_SESSIONS",
  "TSE_FUNC_LOCK_INSTANCE",
  "TSE_FUNC_UNLOCK_INSTANCE",
  "TSE_FUNC_CHECK_TABLE_EXIST",
  "TSE_FUNC_SEARCH_METADATA_SWITCH",
  "TSE_FUNC_PRE_CREATE_DB",
  "TSE_FUNC_TYPE_DROP_TABLESPACE_AND_USER",
  "TSE_FUNC_DROP_DB_PRE_CHECK",
  "TSE_FUNC_KILL_CONNECTION",
  "TSE_FUNC_TYPE_INVALIDATE_OBJECT",
  "TSE_FUNC_TYPE_RECORD_SQL",
  "TSE_FUNC_TYPE_REGISTER_INSTANCE",
  "TSE_FUNC_TYPE_WAIT_CONNETOR_STARTUPED",
  "TSE_FUNC_TYPE_MYSQL_EXECUTE_UPDATE",
  "TSE_FUNC_TYPE_CLOSE_MYSQL_CONNECTION",
  "TSE_FUNC_TYPE_LOCK_TABLES",
  "TSE_FUNC_TYPE_UNLOCK_TABLES",
  "TSE_FUNC_TYPE_EXECUTE_REWRITE_OPEN_CONN",
  "TSE_FUNC_TYPE_INVALIDATE_OBJECTS",
};

ctc_stats& ctc_stats::get_instance() noexcept {
  static ctc_stats m_ctc_stats;
  return m_ctc_stats;
}

bool ctc_stats::get_stats_enabled() {
  return m_stats_enabled;
}

void ctc_stats::set_stats_enabled(const bool val) {
  if (val && !m_stats_enabled) {
    for (int i = 0; i < TSE_FUNC_TYPE_NUMBER; i++) {
      m_calls[i] = 0;
      m_use_time[i] = 0;
    }
  }
  
  m_stats_enabled = val;
}

void ctc_stats::gather_stats(const enum TSE_FUNC_TYPE& type, const uint64_t use_time) {
  m_calls[type]++;
  m_use_time[type] += use_time;
}

void ctc_stats::print_stats(THD *thd, stat_print_fn *stat_print) {
  char *ctc_srv_monitor;
  std::string ctc_srv_monitor_str;
  if ((sizeof(ctc_interface_strs) / sizeof(ctc_interface_strs[0])) != TSE_FUNC_TYPE_NUMBER) {
    ctc_srv_monitor_str = "[CTC_STATS]: ctc_interface_strs number must be same as total ctc interfaces.";
    ctc_srv_monitor = &ctc_srv_monitor_str[0];
    stat_print(thd, "ctc",
                       static_cast<uint>(strlen("ctc")),
                       STRING_WITH_LEN(""), ctc_srv_monitor, (uint)ctc_srv_monitor_str.length());
    return;
  }

  ctc_srv_monitor_str = "\n======================================CTC_STATS=====================================\n";
  ctc_srv_monitor_str += "Interface:   Call counter    Used Time    Average Time\n";
  for (int i = 0; i < TSE_FUNC_TYPE_NUMBER; i++) {
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
  ctc_srv_monitor = &ctc_srv_monitor_str[0];
  stat_print(thd, "ctc",
                       static_cast<uint>(strlen("ctc")),
                       STRING_WITH_LEN(""), ctc_srv_monitor, (uint)ctc_srv_monitor_str.length());
}
