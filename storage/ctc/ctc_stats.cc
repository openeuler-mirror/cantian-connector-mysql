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

ctc_stats *ctc_stats ::m_ctc_stats = NULL;

const char *event_tracking_messages[] = {
  //DATABASE
  "EVENT_TYPE_PRE_CREATE_DB",
  "EVENT_TYPE_DROP_DB",
  //TABLE
  "EVENT_TYPE_OPEN_TABLE",
  "EVENT_TYPE_CLOSE_TABLE",
  "EVENT_TYPE_CREATE_TABLE",
  "EVENT_TYPE_RENAME_TABLE",
  "EVENT_TYPE_DROP_TABLE",
  "EVENT_TYPE_INPLACE_ALTER_TABLE",
  //DML-FETCH
  "EVENT_TYPE_RND_INIT",
  "EVENT_TYPE_RND_NEXT",
  "EVENT_TYPE_POSITION",
  "EVENT_TYPE_RND_POS",
  "EVENT_TYPE_RND_END",
  //DML
  "EVENT_TYPE_WRITE_ROW",
  "EVENT_TYPE_UPDATE_ROW",
  "EVENT_TYPE_DELETE_ROW",
  "EVENT_TYPE_DELETE_ALL_ROWS",
  //INDEX
  "EVENT_TYPE_INDEX_INIT",
  "EVENT_TYPE_INDEX_END",
  "EVENT_TYPE_INDEX_READ",
  "EVENT_TYPE_INDEX_FETCH",
  //CBO
  "EVENT_TYPE_INITIALIZE_DBO",
  "EVENT_TYPE_FREE_CBO",
  "EVENT_TYPE_GET_CBO",
  "EVENT_TYPE_CBO_ANALYZE",
  "EVENT_TYPE_CBO_RECORDS",
  "EVENT_TYPE_CBO_RECORDS_IN_RANGE",
  //TRANSCTIONS
  "EVENT_TYPE_COMMIT",
  "EVENT_TYPE_ROLLBACK",
  "EVENT_TYPE_BEGIN_TRX",
  "EVENT_TYPE_RELEASE_SAVEPOINT",
  "EVENT_TYPE_SET_SAVEPOINT",
  "EVENT_TYPE_ROLLBACK_SAVEPOINT",
  //CONNECTION
  "EVENT_TYPE_KILL_CONNECTION",
  "EVENT_TYPE_CLOSE_CONNECTION",

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

ctc_stats* ctc_stats::get_instance() noexcept {
  if(m_ctc_stats == NULL) {
      m_ctc_stats = new ctc_stats();
      return m_ctc_stats; 
    } else {   
      return m_ctc_stats;
    }
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
  initialize_clock_freq();
  m_statistics_enabled = val;
}

void ctc_stats::gather_stats(const enum EVENT_TRACKING type, const uint64_t use_time) {
  if (clock_frequency == 0) {
    ctc_log_error("[TSE STATS] clock frequency is not initialized.");
    return;
  }
  m_calls[type]++;
  m_use_time[type] += (use_time * 1e6)/clock_frequency;
}

void ctc_stats::print_cost_times(std::string &ctc_srv_monitor_str) {
  if ((sizeof(event_tracking_messages) / sizeof(event_tracking_messages[0])) != EVENT_TYPE_COUNT) {
    ctc_srv_monitor_str += "[CTC_STATS]: ctc_interface_strs number must be same as total ctc interfaces.\n";
    return;
  }

  ctc_srv_monitor_str += "\n===================================================CTC_STATS===================================================\n";
  size_t size = EVENT_TYPE_COUNT;
  auto longestStr = std::max_element(event_tracking_messages, event_tracking_messages + size, 
        [](const char* a, const char* b) {
            return std::strlen(a) < std::strlen(b);
        });
  int interf_width = strlen(*longestStr);
  ctc_srv_monitor_str += "Interface" + std::string(interf_width - strlen("Interface"), ' ');
  ctc_srv_monitor_str += "Call counter             Used Time                Average Time\n";
                                                                   
  for (int i = 0; i < EVENT_TYPE_COUNT; i++) {
    uint64_t calls = m_calls[i];
    uint64_t use_time = m_use_time[i];
    if (calls == 0) {
      continue;
    }

    uint64_t average_time = use_time / calls;
    ctc_srv_monitor_str += event_tracking_messages[i];
    std::string calls_str = std::to_string(calls);
    std::string total_str = std::to_string(use_time);
    std::string avg_str = std::to_string(average_time);
    ctc_srv_monitor_str += std::string(interf_width - strlen(event_tracking_messages[i]), ' ') + calls_str + std::string(CTC_STATS_TABLE_COL_WIDTH - calls_str.size(), ' ') 
          + total_str + std::string(CTC_STATS_TABLE_COL_WIDTH - total_str.size(), ' ')  +  avg_str + "\n";
  }

  ctc_srv_monitor_str += "\n====================================================CTC_STATS================================================\n";
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

#if (defined __x86_64__)
void ctc_stats::initialize_clock_freq()
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        ctc_log_error("[IO RECORD] Failed to open 'proc/cpuinfo");
        my_error(ER_INTERNAL_ERROR, MYF(0), "Clock frequency initialization failed: unable to open cpuinfo.");
        return;
    }
    char line[100];
    double freq = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "cpu MHz : %lf", &freq) == 1) {
            fclose(fp);
            clock_frequency = freq * 1e6;
            return;
        }
    }
    ctc_log_error("[io record] failed to get cpu frequency.");
    my_error(ER_INTERNAL_ERROR, MYF(0), "Clock frequency initialization failed: unable to get cpu frequency.");
}
#else
void ctc_stats::initialize_clock_freq()
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    clock_frequency = freq;
}
#endif

#if (defined __x86_64__)
#include <x86intrin.h>
uint64_t rdtsc(){
    return __rdtsc();
}
#else
uint64_t rdtsc()
{
    uint64_t tsc;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (tsc));
    return tsc;
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