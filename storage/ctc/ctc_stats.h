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

class ctc_stats {
  private:
    ctc_stats(void) = default;
    ~ctc_stats() = default;

    ctc_stats(const ctc_stats&) = delete;
    ctc_stats& operator=(const ctc_stats&) = delete;
  
  public:
    static ctc_stats& get_instance(void) noexcept;
    bool get_statistics_enabled(void);
    void set_statistics_enabled(const bool val);
    void gather_stats(const enum CTC_FUNC_TYPE& type, const uint64_t use_time);
    void print_stats(THD *thd, stat_print_fn *stat_print);
    void print_cost_times(std::string &ctc_srv_monitor_str);
    void print_shm_usage(std::string &ctc_srv_monitor_str);
  private:
    bool m_statistics_enabled = false;

    std::atomic_uint64_t m_calls[CTC_FUNC_TYPE_NUMBER];
    std::atomic_uint64_t m_use_time[CTC_FUNC_TYPE_NUMBER];
};

#endif