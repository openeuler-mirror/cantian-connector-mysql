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

#include <mutex>
#include "ha_ctc_pq.h"
#include "srv_mq_msg.h"
#include "decimal_convert.h" // CT_ERROR
#include "my_alloc.h"
#include "sql/mysqld_thd_manager.h"
#include "datatype_cnvrtr.h"
#include "ctc_error.h"
#include "sql/table.h"
#include "mysql/plugin.h"
#include "ctc_log.h"
#include "ha_ctcpart.h"
#include "ha_ctc.h"

constexpr uint64 INVALID_VALUE64 = 0xFFFFFFFFFFFFFFFFULL;
static std::mutex parallel_reader_thread_mutex;
static int parallel_reader_thread_running = 0;
constexpr int MAX_PARALLEL_READ_THREADS = 256;
int CtcParallelReaderHandler::open_table(ctc_handler_t *new_tch)
{
  // assuming all table sent to this PRHandler as non-temp, non-system table,
  // so a simple split over normalized_path would suffice.
  // following logic is a shameless copy from ha_ctc::open with following difference:
  //   a. no longer use information from thd or sess_ctx
  //   b. don't write back updated tch to sess_ctx
  char db_name[SMALL_RECORD_SIZE] = {0};
  char table_name[SMALL_RECORD_SIZE] = {0};
  bool is_tmp_table = false;
  ctc_split_normalized_name(m_table->s->normalized_path.str, db_name,
                            SMALL_RECORD_SIZE, table_name, SMALL_RECORD_SIZE, &is_tmp_table);
  ctc_copy_name(table_name, m_table->s->table_name.str, SMALL_RECORD_SIZE);

  ct_errno_t ret = (ct_errno_t) ctc_open_table(new_tch, table_name, db_name);
  return ret;
}

ctc_handler_t CtcParallelReaderHandler::construct_new_tch(ctc_handler_t origin_tch)
{
  return {
    .inst_id = origin_tch.inst_id,
    // fetch a thd_id from Global_THD_manager, shall be return in deinitialize
    .thd_id = Global_THD_manager::get_instance()->get_new_thread_id(),
    // part_no, differs according to splitted task
    .part_id = INVALID_PART_ID,
    // subpart_no, same as above
    .subpart_id = INVALID_PART_ID,
    .query_id = origin_tch.query_id,
    // sess_addr, will be allocated through
    //   ctc_open_table() -> ctc_get_or_new_session()
    .sess_addr = INVALID_VALUE64,
    // ctx_addr, will be allocated through
    //   ctc_open_table() -> init_ctc_ctx_and_open_dc()
    .ctx_addr = INVALID_VALUE64,
    // cursor_addr, a INVALID_VALUE64 for uninitialized session
    .cursor_addr = INVALID_VALUE64,
    // pre_sess_addr, always zero as we don't care about
    //   recover from other session etc thing
    .pre_sess_addr = 0,
    // cursor_ref, the cursor count, zero for uninitialized session
    .cursor_ref = 0,
    // bind_core, would be assigned by cantian side
    .bind_core = 0,
    .sql_command = origin_tch.sql_command,
    .sql_stat_start = origin_tch.sql_stat_start,
    .change_data_capture = origin_tch.change_data_capture,
    // cursor_valid set to false
    .cursor_valid = false,
    // is_broadcast kept false as only non-temp, non system query will be here.
    .is_broadcast = false,
    // should always be false since no sys table request
    //   shall be route to CtcParallelReaderHandler.
    .read_only_in_ct = origin_tch.read_only_in_ct,
    .msg_buf = nullptr
  };
}

int CtcParallelReaderHandler::save_task(ctc_index_paral_range_t *result_paral_range,
    uint32_t part_id, uint32_t subpart_id, uint64_t query_scn, uint64_t ssn)
{
  lock_guard<mutex> lock(m_work_mutex);
  for (uint32_t i = 0; i < result_paral_range->workers; i++) {
    auto node = (LIST *) my_malloc(PSI_NOT_INSTRUMENTED, sizeof(LIST), MYF(MY_WME | MY_ZEROFILL));
    auto data = (ctcpart_scan_range_t *) my_malloc(PSI_NOT_INSTRUMENTED,
                                                   sizeof(ctcpart_scan_range_t), MYF(MY_WME));
    if (node == nullptr || data == nullptr) {
      my_free(node);
      my_free(data);
      return CT_ERROR;
    }
    memcpy(&data->range, result_paral_range->range[i], sizeof(ctc_scan_range_t));
    data->part = {
      .part_id = part_id,
      .subpart_id = subpart_id,
    };
    data->query_scn = query_scn;
    data->ssn = ssn;
    node->data = data;
    m_paral_range = list_add(m_paral_range, node);
  };
  return CT_SUCCESS;
};

ctcpart_scan_range_t* CtcParallelReaderHandler::fetch_range()
{
  lock_guard<mutex> lock(m_work_mutex);
  if (m_paral_range == nullptr) {
    return nullptr;
  }
  ctcpart_scan_range_t *scan_range = static_cast<ctcpart_scan_range_t *>(m_paral_range->data);
  LIST *old_node = m_paral_range;
  m_paral_range = list_delete(m_paral_range, m_paral_range);
  my_free(old_node);
  return scan_range;
};

// If we don't have enough thread count left, it's EXPECTED to exit early.
void CtcParallelReaderHandler::acquire_parallel_thread_count_from_global(
    int expected_dop, int dop_thd_var, int global_dop_var)
{
  m_worker_count = 0;
  int worker_count_to_acquire = 0;

  if (expected_dop != 0) {
     // future-proof to potential EverSQL side change.
    worker_count_to_acquire = expected_dop > MAX_PARALLEL_READ_THREADS ? MAX_PARALLEL_READ_THREADS: expected_dop;
  } else {
    // according to current status of EverSQL side's code
    //    expected_dop will always be zero and it's left to us to decide dop,
    //    thus a ctc_parallel_read_threads thd variable is introduced
    worker_count_to_acquire = dop_thd_var;
  }

  if (worker_count_to_acquire > global_dop_var) {
    // fool-proof to potential bad user provided sysvar
    worker_count_to_acquire = global_dop_var;
  }

  std::unique_lock<mutex> parallel_reader_thread_mutex_lock(parallel_reader_thread_mutex);
  if (worker_count_to_acquire + parallel_reader_thread_running > global_dop_var) {
    return ;
  }

  parallel_reader_thread_running += worker_count_to_acquire;
  m_worker_count = worker_count_to_acquire;
  return ;
}

void CtcParallelReaderHandler::release_parallel_thread_count_back_to_global()
{
  std::lock_guard<mutex> parallel_reader_thread_mutex_lock(parallel_reader_thread_mutex);
  parallel_reader_thread_running -= m_worker_count;
}

// CtcParallelReaderHandler is pretty much useless without an initialize call, DO remember to call this
int CtcParallelReaderHandler::initialize(TABLE *table, int expected_dop,
                                         ctc_handler_t origin_tch, int dop_thd_var, int global_dop_var)
{
  m_table = table;

  acquire_parallel_thread_count_from_global(expected_dop, dop_thd_var, global_dop_var);
  if (m_worker_count == 0) {
    return CT_ERROR;
  }

  m_work_tuples = (pq_work_tuple *) my_malloc(PSI_NOT_INSTRUMENTED,
                                              sizeof(pq_work_tuple) * m_worker_count, MYF(MY_WME | MY_ZEROFILL));

  m_my_buf_for_read = (char **) my_malloc(PSI_NOT_INSTRUMENTED,
                                          sizeof(char *) * m_worker_count, MYF(MY_WME | MY_ZEROFILL));
  m_buf_for_read_count = m_worker_count - 1;

  if (m_work_tuples == nullptr || m_my_buf_for_read == nullptr) {
    // free up here to make sure we don't mixed up mem alloc
    //    failure with other fail path
    my_free(m_work_tuples);
    my_free(m_my_buf_for_read);
    m_buf_for_read_count = 0;
    return CT_ERROR;
  }

  // in the initialize call, we shall still be in the mysql context,
  // the following steps CtcParallelReaderHandler shall be done before any rnd_next_block call:
  //   0. initialize all field of CtcParallelReaderHandler
  m_paral_range = nullptr;

  //   1. create workers (mimicked through different tch & multiple thread
  //    from a thread pool within duckdb context)
  //   2. make ctc_open_table call for each tch so it have a
  //    valid (sess_addr, ctx_addr) tuple for use
  for (int i = 0; i < m_worker_count; i++) {
    ctc_handler_t new_worker_tch = construct_new_tch(origin_tch);
    open_table(&new_worker_tch);

    m_work_tuples[i] = {
      .tch = new_worker_tch,
      .prefetch_buf = nullptr,
      .current_offset = 0,
      .current_entry_index = 0,
      .record_lens = {0},
      .fetched_num = 0,
      .in_use = false,
    };
  }

  //   3. prepare the mysql column format
  m_col_offsets = (ulong *) my_malloc(PSI_NOT_INSTRUMENTED,
                                      sizeof(ulong) * CTC_MAX_COLUMNS, MYF(MY_WME | MY_ZEROFILL));
  m_null_byte_offsets = (ulong *) my_malloc(PSI_NOT_INSTRUMENTED,
                                            sizeof(ulong) * CTC_MAX_COLUMNS, MYF(MY_WME | MY_ZEROFILL));
  m_null_bitmasks = (ulong *) my_malloc(PSI_NOT_INSTRUMENTED,
                                        sizeof(ulong) * CTC_MAX_COLUMNS, MYF(MY_WME | MY_ZEROFILL));
  if (m_col_offsets == nullptr || m_null_byte_offsets == nullptr || m_null_bitmasks == nullptr) {
    return CT_ERROR;
  }
  m_ncols = 0;
  m_row_len = 0;
  generate_mysql_record_row_desc(*m_table, &m_ncols, &m_row_len, m_col_offsets,
                                 m_null_bitmasks, m_null_byte_offsets, m_table->read_set);
  m_cantian_rec_len = get_cantian_record_length(m_table);

  // 1. prepare a char* for parallel_reader_data_frame_t->m_buf so we can avoid
  //    repetitive malloc-and-free, this shall only store record converted to
  //    msyql representation, aligned with mysql_row_desc_t.m_row_len
  CTC_RETURN_IF_NOT_ZERO(my_alloc_data_frame_buf());
  // 2. allocate m_prefetch_buf for rnd_next_block use, directly from cantian
  //    side with cantian representation, similar to
  //    prefetch_and_fill_record_buffer(buf, ctc_rnd_prefetch), length may be varied
  CTC_RETURN_IF_NOT_ZERO(ctc_alloc_ctc_prefetch_buf());
  return CT_SUCCESS;
}

void CtcParallelReaderHandler::deinitialize()
{
  release_parallel_thread_count_back_to_global();
  // free all mysql desc etc
  if (m_col_offsets != nullptr) {
    my_free(m_col_offsets);
    m_col_offsets = nullptr;
  }
  if (m_null_byte_offsets != nullptr) {
    my_free(m_null_byte_offsets);
    m_null_byte_offsets = nullptr;
  }
  if (m_null_bitmasks != nullptr) {
    my_free(m_null_bitmasks);
    m_null_bitmasks = nullptr;
  }
  std::lock_guard<mutex> worker_lock(m_worker_mutex);
  for (long i = 0; i < m_worker_count; i++) {
    my_free(m_my_buf_for_read[i]);
    ctc_free_buf(&(m_work_tuples[i].tch), m_work_tuples[i].prefetch_buf);

    if (m_work_tuples[i].tch.cursor_valid) {
      ctc_rnd_end(&(m_work_tuples[i].tch));
    }
    ctc_close_table(&(m_work_tuples[i].tch));
    ctc_close_session(&(m_work_tuples[i].tch));
    // symmetrical to get_new_thread_id in construct_new_tch
    Global_THD_manager::get_instance()->release_thread_id(m_work_tuples[i].tch.thd_id);
  }
  my_free(m_work_tuples);
  my_free(m_my_buf_for_read);

  std::lock_guard<mutex> work_lock(m_work_mutex);
  // ideally we should have exhausted all work entry in m_paral_range for a
  //    full table by-page scan, but LIMIT or similar sub-clause may cause
  //    leftovers in m_paral_range
  list_free(m_paral_range, true);
  m_paral_range = nullptr;
  return ;
}

int CtcParallelReaderHandler::rnd_init()
{
  return CT_SUCCESS;
}

void CtcParallelReaderHandler::get_work_tuple(int *work_tuple_index)
{
  for (int i = 0; i < m_worker_count; i++) {
    if (m_work_tuples[i].tch.cursor_valid && m_work_tuples[i].fetched_num != 0 && m_work_tuples[i].in_use == false) {
      // found a fetched range setup in this work tuple, leftover record still
      //    in buffer and didn't have an EOF signal for this range yet.
      // continue reading from this work tuple
      *work_tuple_index = i;
      return ;
    }
  }

  // didn't find any work tuple with fetched range, just find any one not in use
  for (int i = 0; i < m_worker_count; i++) {
    if (m_work_tuples[i].in_use == false) {
      *work_tuple_index = i;
      return ;
    }
  }
}

int CtcParallelReaderHandler::prepare_work_tuple(pq_work_tuple *work_tuple, bool need_new_range)
{
  int ret = CT_SUCCESS;
  if (work_tuple->tch.cursor_valid && (!need_new_range)) {
    // valid cursor and we don't explicitly request a new range
    return CT_SUCCESS;
  }

  ctcpart_scan_range_t *scan_range = fetch_range();
  if (scan_range == nullptr) {
    // exhausted all potential scan range
    return CT_ERROR;
  }

  // close existing one we have on work_tuple.tch and open a new one
  ret = ctc_rnd_end(&work_tuple->tch);
  if (ret != CT_SUCCESS) {
    my_free(scan_range);
    return ret;
  }

  work_tuple->tch.part_id = scan_range->part.part_id;
  work_tuple->tch.subpart_id = scan_range->part.subpart_id;
  // the last parameter nullptr is left for condition pushdown
  ret = ctc_rnd_init(&work_tuple->tch, EXP_CURSOR_ACTION_SELECT, SELECT_ORDINARY, nullptr);
  if (ret != CT_SUCCESS) {
    my_free(scan_range);
    return ret;
  }

  ret = ctc_pq_set_cursor_range(&work_tuple->tch, scan_range->range.l_page, scan_range->range.r_page,
                                scan_range->query_scn, scan_range->ssn);
  my_free(scan_range);
  return ret;
}

void CtcParallelReaderHandler::convert_prefetch_buffer_to_mysql(parallel_reader_data_frame_t *df,
                                                                pq_work_tuple *work_tuple)
{
  df->m_actual_data_size = 0;
  df->m_buf_size = 0;
  // convert leftovers or freshly fetched entry to mysql side representation
  for (;work_tuple->current_entry_index < work_tuple->fetched_num;) {
    index_info_t index = {
      .active_index = MAX_INDEXES,
      .max_col_idx = (uint) m_ncols,
    };
    int cantian_record_buf_size = work_tuple->record_lens[work_tuple->current_entry_index];
    record_buf_info_t record_buf = {
      .cantian_record_buf = work_tuple->prefetch_buf + work_tuple->current_offset,
      .mysql_record_buf = (uchar *) df->m_buf + df->m_actual_data_size,
      .cantian_record_buf_size = &cantian_record_buf_size,
    };
    cantian_record_to_mysql_record(*m_table, &index, &record_buf, work_tuple->tch, nullptr);

    df->m_actual_data_size += m_row_len;
    work_tuple->current_offset += work_tuple->record_lens[work_tuple->current_entry_index];
    work_tuple->current_entry_index ++;

    if (df->m_actual_data_size + m_row_len >= BIG_RECORD_SIZE) {
      // destination buffer can't handle more record
      break;
    }
  }
}

// EverSQL side always segfault for non CT_SUCCESS ret value for rnd_next_block, always return CT_SUCCESS
// To indicate a failed fetch or an EOF, use m_actual_data_size = 0 as a signal.
//    df->m_buf_size and df->m_buf_id serves no use for this scenario.
int CtcParallelReaderHandler::rnd_next_block(parallel_reader_data_frame_t *df)
{
  m_worker_mutex.lock();
  if (m_buf_for_read_count < 0) {
    // exhausted all m_my_buf_for_read, scenario would be like below:
    //   1. reset_data_frame hasn't been called yet
    //   2. EverSQL has bigger parallel degree than we have.
    // either way, we need to return an EOF to calm them down.
    df->m_actual_data_size = 0;
    df->m_buf = nullptr;
    m_worker_mutex.unlock();
    return CT_SUCCESS;
  }

  int work_tuple_index = -1;
  get_work_tuple(&work_tuple_index);
  if (work_tuple_index == -1) {
    // eversql side use more dop than guidance scenario
    df->m_actual_data_size = 0;
    df->m_buf = nullptr;
    m_worker_mutex.unlock();
    return CT_SUCCESS;
  }

  m_work_tuples[work_tuple_index].in_use = true;
  pq_work_tuple work_tuple = m_work_tuples[work_tuple_index];

  // df->m_buf will be returned to CtcParallelReaderHandler by reset_data_frame
  df->m_buf = m_my_buf_for_read[m_buf_for_read_count];
  m_buf_for_read_count--;

  m_worker_mutex.unlock();

  bool fetched = false;
  bool need_new_range = false;
  if (work_tuple.fetched_num <= work_tuple.current_entry_index || work_tuple.fetched_num == 0) {
    // the prefetch_buf in work_tuple exhausted and should be fetching new now
    while (!fetched) {
      int ret = prepare_work_tuple(&work_tuple, need_new_range);
      if (ret != CT_SUCCESS) {
        // preparation step for ctc_rnd_prefetch failed, this mostly
        //    indicate an OOM scenario or corrupted tch, return an
        //    EOF to clean up this worker thread
        break;
      }

      // attempt to do a ctc_rnd_prefetch to do bulk read here;
      // rowid array placeholders which would be disposed immediately
      //    as won't have position() call during parallel query.
      uint64_t rowid_placeholder[MAX_BATCH_FETCH_NUM];
      ret = ctc_rnd_prefetch(&work_tuple.tch, work_tuple.prefetch_buf, &work_tuple.record_lens[0],
                             &work_tuple.fetched_num, rowid_placeholder, m_cantian_rec_len);
      if (work_tuple.fetched_num == 0 && (ret == CT_SUCCESS)) {
        need_new_range = true;
      } else {
        fetched = true;
        work_tuple.current_entry_index = 0;
        work_tuple.current_offset = 0;
      }
    }
  }

  convert_prefetch_buffer_to_mysql(df, &work_tuple);

  m_worker_mutex.lock();
  work_tuple.in_use = false;
  m_work_tuples[work_tuple_index] = work_tuple;
  m_worker_mutex.unlock();

  // EverSQL side WON'T call reset_data_frame when df->m_actual_data_size == 0, return df->m_buf back here
  // otherwise, the buffer shall be add back in reset_data_frame
  if (df->m_actual_data_size == 0) {
    reset_data_frame(df);
  }
  return CT_SUCCESS;
}

int CtcParallelReaderHandler::my_alloc_data_frame_buf()
{
  // unavoidable to do malloc here as the converted result always need a place
  //  to store in, no matter it's in single run mode or not
  std::lock_guard<mutex> lock(m_worker_mutex);
  for (int i = 0; i < m_worker_count; i++) {
    m_my_buf_for_read[i] = (char *) my_malloc(PSI_NOT_INSTRUMENTED,
                                              sizeof(char) * BIG_RECORD_SIZE, MYF(MY_WME | MY_ZEROFILL));
    if (m_my_buf_for_read[i] == nullptr) {
      return convert_ctc_error_code_to_mysql(ERR_ALLOC_MEMORY);
    }
  }
  return CT_SUCCESS;
}

// ctc_alloc_buf prefetch_buf for temporary space between ctc and kernel
int CtcParallelReaderHandler::ctc_alloc_ctc_prefetch_buf()
{
  std::lock_guard<mutex> lock(m_worker_mutex);
  for (int i = 0; i < m_worker_count; i++) {
    m_work_tuples[i].prefetch_buf = (uchar *)ctc_alloc_buf(&(m_work_tuples[i].tch), BIG_RECORD_SIZE);
    if (m_work_tuples[i].prefetch_buf == nullptr) {
      return convert_ctc_error_code_to_mysql(ERR_ALLOC_MEMORY);
    }
  }
  return CT_SUCCESS;
}

int CtcParallelReaderHandler::rnd_end()
{
  return CT_SUCCESS;
}

ParallelReaderHandler::mysql_row_desc_t CtcParallelReaderHandler::get_row_info_for_parse()
{
  return ParallelReaderHandler::mysql_row_desc_t {
    .m_ncols = m_ncols,
    .m_row_len = m_row_len,
    .m_col_offsets = m_col_offsets,
    .m_null_byte_offsets = m_null_byte_offsets,
    .m_null_bitmasks = m_null_bitmasks,
  };
}

void CtcParallelReaderHandler::reset_data_frame(parallel_reader_data_frame_t *df)
{
  std::lock_guard<mutex> lock(m_worker_mutex);
  if (df->m_buf != nullptr) {
    m_buf_for_read_count ++;
    m_my_buf_for_read[m_buf_for_read_count] = df->m_buf;
    df->m_buf = nullptr;
  }
  return;
};
