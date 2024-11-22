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
#ifndef __CTC_STATS_H__
#define __CTC_STATS_H__

#include <cmath>
#include <atomic>
#include "../../sql/handler.h"
#include "../../sql/sql_class.h"
#include "../../sql/sql_profile.h"
#include "my_config.h"
#include "ctc_srv.h"
#include "ha_ctc.h"

#define CPUINFO_PATH "/proc/cpuinfo"

#define BEGIN_RECORD_STATS    \
    uint64_t start_clock = 0;   \
    if (ctc_stats::get_instance()->get_statistics_enabled()) {     \
        start_clock = rdtsc();    \
    }

#define END_RECORD_STATS(type)    \
    if (ctc_stats::get_instance()->get_statistics_enabled()) {   \
        ctc_stats::get_instance()->gather_stats(type, start_clock);    \
    }

#define CTC_STATS_TABLE_COL_WIDTH 25
#define EVENT_TRACKING_GROUP 1024
#define EVENT_TRACKING_HASH_FUNC(CYCLES) (CYCLES >> 1) % EVENT_TRACKING_GROUP
#define LINUX_SYSINFO_LEN 100

enum EVENT_TRACKING {
    // DATABASE
    EVENT_TYPE_PRE_CREATE_DB,
    EVENT_TYPE_DROP_DB,
    // TABLE
    EVENT_TYPE_OPEN_TABLE,
    EVENT_TYPE_CLOSE_TABLE,
    EVENT_TYPE_CREATE_TABLE,
    EVENT_TYPE_RENAME_TABLE,
    EVENT_TYPE_DROP_TABLE,
    EVENT_TYPE_INPLACE_ALTER_TABLE,
    // FETCH
    EVENT_TYPE_RND_INIT,
    EVENT_TYPE_RND_NEXT,
    EVENT_TYPE_POSITION,
    EVENT_TYPE_RND_POS,
    EVENT_TYPE_RND_END,
    // DML
    EVENT_TYPE_WRITE_ROW,
    EVENT_TYPE_UPDATE_ROW,
    EVENT_TYPE_DELETE_ROW,
    EVENT_TYPE_DELETE_ALL_ROWS,
    // INDEX
    EVENT_TYPE_INDEX_INIT,
    EVENT_TYPE_INDEX_END,
    EVENT_TYPE_INDEX_READ,
    EVENT_TYPE_INDEX_FETCH,
    // CBO
    EVENT_TYPE_INITIALIZE_DBO,
    EVENT_TYPE_FREE_CBO,
    EVENT_TYPE_GET_CBO,
    EVENT_TYPE_CBO_ANALYZE,
    EVENT_TYPE_CBO_RECORDS,
    EVENT_TYPE_CBO_RECORDS_IN_RANGE,
    // TRANSCTIONS
    EVENT_TYPE_COMMIT,
    EVENT_TYPE_ROLLBACK,
    EVENT_TYPE_BEGIN_TRX,
    EVENT_TYPE_RELEASE_SAVEPOINT,
    EVENT_TYPE_SET_SAVEPOINT,
    EVENT_TYPE_ROLLBACK_SAVEPOINT,
    // CONNECTION
    EVENT_TYPE_KILL_CONNECTION,
    EVENT_TYPE_CLOSE_CONNECTION,

    EVENT_TYPE_COUNT,
};

uint64_t rdtsc();

class ctc_stats {
    public:
        static ctc_stats* get_instance(void) noexcept;
        bool get_statistics_enabled(void);
        void set_statistics_enabled(const bool val);
        void gather_stats(const enum EVENT_TRACKING type, const uint64_t start_cycles);
        void print_stats(THD *thd, stat_print_fn *stat_print);
        void print_cost_times(std::string &ctc_srv_monitor_str);
        void print_shm_usage(std::string &ctc_srv_monitor_str);
    private:
        ctc_stats(void) = default;
        ~ctc_stats() = default;

        ctc_stats(const ctc_stats&) = delete;
        ctc_stats& operator=(const ctc_stats&) = delete;
        
        static ctc_stats *m_ctc_stats;
        std::atomic<bool> m_statistics_enabled{false};
        uint64_t clock_frequency = 0;

        void initialize_clock_freq();

        std::atomic_uint64_t m_calls[EVENT_TYPE_COUNT][EVENT_TRACKING_GROUP];
        std::atomic_uint64_t m_use_time[EVENT_TYPE_COUNT][EVENT_TRACKING_GROUP];
};

#endif