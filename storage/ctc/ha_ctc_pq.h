/*
  Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

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

#ifndef __HA_CTC_PQ_H__
#define __HA_CTC_PQ_H__

#include <sys/types.h>
#include <mutex>
#include <vector>
#include "my_inttypes.h"
#include "my_list.h"
#include "my_base.h"
#include "ha_ctc.h"
#include "ctc_log.h"

// The following exception is for one-shot use inside ha_ctc_pq.{h/cpp}
//  and shall NOT be used anywhere else.
// This method shall be eliminated post stable-integration with index scan and
//   range index scan.
class CtcPQNotImplemented : public std::logic_error {
public:
  CtcPQNotImplemented() : std::logic_error("Function not yet implemented")
  {
  }
};

struct pq_work_tuple {
  ctc_handler_t tch;
  uchar *prefetch_buf;
  uint64_t current_offset;
  uint32_t current_entry_index;
  uint16_t record_lens[MAX_BATCH_FETCH_NUM];
  uint32_t fetched_num;
  bool in_use;
};

class CtcParallelReaderHandler : public ParallelReaderHandler {
public:
  // acquired worker count
  int m_worker_count;
  int m_cantian_rec_len;
  CtcParallelReaderHandler() = default;
  ~CtcParallelReaderHandler() = default;
  int initialize(TABLE *table, int expected_dop, ctc_handler_t origin_tch,
    int dop_thd_var, int global_dop_var);
  void deinitialize();

  // fetch row info, this shall be called only once in mysql context
  ParallelReaderHandler::mysql_row_desc_t get_row_info_for_parse() override;

  // free up parallel_reader_data_frame_t allocated in
  //  {index/rnd/read_range}_next_block, shall be called once every time
  void reset_data_frame(parallel_reader_data_frame_t *df) override;

  // by-page scan related method
  // start of a by-page scan, only get called in an mysql context for once
  int rnd_init() override;

  // this shall be called by executor thread in parallel, return through a
  //  locally allocated parallel_reader_data_frame_t
  int rnd_next_block(parallel_reader_data_frame_t *df) override;

  // end of a by-page scan
  int rnd_end() override;

  int save_task(ctc_index_paral_range_t *result_paral_range, uint32_t part_id,
    uint32_t subpart_id, uint64_t query_scn, uint64_t ssn);

  ctc_handler_t construct_new_tch(ctc_handler_t origin_tch);
  int open_table(ctc_handler_t *new_tch);

  // the following are just api placeholder for future use

  // ranged scan
  int read_range_init(uint keynr, key_range *min_key, key_range *max_key) override
  {
    UNUSED_PARAM(keynr);
    UNUSED_PARAM(min_key);
    UNUSED_PARAM(max_key);
    throw CtcPQNotImplemented();
  }

  int read_range_next_block(parallel_reader_data_frame_t *df) override
  {
    UNUSED_PARAM(df);
    throw CtcPQNotImplemented();
  }

  int read_range_end() override
  {
    throw CtcPQNotImplemented();
  }

  int condition_pushdown(Item *cond, Item *left) override
  {
    UNUSED_PARAM(cond);
    UNUSED_PARAM(left);
    throw CtcPQNotImplemented();
  }

  // index based full table scan
  int index_init(int keyno, bool asc) override
  {
    UNUSED_PARAM(keyno);
    UNUSED_PARAM(asc);
    throw CtcPQNotImplemented();
  }

  int index_next_block(parallel_reader_data_frame_t *df) override
  {
    UNUSED_PARAM(df);
    throw CtcPQNotImplemented();
  }

  int index_end() override
  {
    throw CtcPQNotImplemented();
  }

private:
  // fields saved for mysql_row_desc_t
  ulong m_ncols;
  ulong m_row_len;
  ulong *m_col_offsets;
  ulong *m_null_byte_offsets;
  ulong *m_null_bitmasks;

  // the array to all ctc_handler_t that we are calling cantian side on behalf of
  // to get a valid worker every {}_next_block call, mutex_guard
  //  worker_mutex and pop one from ctc_handler_t
  std::mutex m_worker_mutex;
  pq_work_tuple *m_work_tuples;

  // m_buf_for_read_count always point to next available m_my_buf_for_read
  int m_buf_for_read_count;
  char **m_my_buf_for_read;

  std::mutex m_work_mutex;
  LIST *m_paral_range;
  TABLE *m_table; // TABLE saved for ease of converting record

  ctcpart_scan_range_t *fetch_range();
  void free_list();

  int my_alloc_data_frame_buf();
  int ctc_alloc_ctc_prefetch_buf();
  void acquire_parallel_thread_count_from_global(int expected_dop, int dop_thd_var, int global_dop_var);
  void release_parallel_thread_count_back_to_global();
  void get_work_tuple(int *work_tuple_index);
  int prepare_work_tuple (pq_work_tuple *work_tuple, bool need_new_range);
  void convert_prefetch_buffer_to_mysql(parallel_reader_data_frame_t *df, pq_work_tuple *work_tuple);
};

#endif