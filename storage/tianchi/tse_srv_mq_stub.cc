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

#include "srv_mq_msg.h"
#include "tse_srv_mq_module.h"
#include "message_queue/dsw_shm.h"
#include "tse_error.h"
#include "tse_log.h"
#include "tse_srv.h"
#include "tse_util.h"
#include "ha_tse.h"
#include "protobuf/tc_db.pb-c.h"
#include <sys/time.h>

#define OUTLINE_LOB_LOCATOR_SIZE 44  // 行外LOB数据结构体长度

#define TSE_RESET_SHM_REQ_ERROR_CODE(_req) \
  do {                                     \
    _req->error_code = 0;                  \
    _req->error_message[0] = 0;            \
  } while (0)

// 双进程模式在 tse_init 中已经提前获取 inst_id
int tse_alloc_inst_id(uint32_t *inst_id) { 
  *inst_id = ha_tse_get_inst_id();
  return tse_mq_register_func();
}

// 双进程模式在 clean_up_for_bad_mysql_proc 中释放 inst_id
int tse_release_inst_id(uint32_t ) {
  return CT_SUCCESS;
}

int tse_open_table(tianchi_handler_t *tch, const char *table_name, const char *user_name) {
  assert(strlen(table_name) + 1 < SMALL_RECORD_SIZE);
  assert(strlen(user_name) + 1 < SMALL_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  open_table_request *req = (open_table_request*)alloc_share_mem(shm_inst, sizeof(open_table_request));
  DBUG_EXECUTE_IF("open_table_shm_oom", { req = NULL; });
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(open_table_request));
    return ERR_ALLOC_MEMORY;
  }

  memcpy(req->table_name, table_name, strlen(table_name) + 1);
  memcpy(req->user_name, user_name, strlen(user_name) + 1);
  req->tch = *tch;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_OPEN_TABLE, req, tch->msg_buf);
  *tch = req->tch; // 此处不管参天处理成功与否，都需要拷贝一次，避免session泄漏
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_close_session(tianchi_handler_t *tch) {
  tse_log_note("close session");
  void *shm_inst = get_one_shm_inst(tch);
  close_session_request *req = (close_session_request*)alloc_share_mem(shm_inst, sizeof(close_session_request));
  if (req == NULL) {
    tse_log_error("[TSE_CLOSE_SESSION]:alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(close_session_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_CLOSE_SESSION, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

void tse_kill_session(tianchi_handler_t *tch) {
  tse_log_note("kill session");
  void *shm_inst = get_one_shm_inst(tch);
  close_session_request *req = (close_session_request*)alloc_share_mem(shm_inst, sizeof(close_session_request));
  if (req == NULL) {
    tse_log_error("[TSE_KILL_SESSION]:alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(close_session_request));
    return;
  }
  req->tch = *tch;

  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_KILL_CONNECTION, req, tch->msg_buf);
  if (ret != CT_SUCCESS) {
    tse_log_error("[TSE_KILL_SESSION]:Connection failed when sending kill stmt message to Cantian");
  }
  free_share_mem(shm_inst, req);
}

int tse_close_table(tianchi_handler_t *tch) {
  void *shm_inst = get_one_shm_inst(tch);
  close_table_request *req = (close_table_request*)alloc_share_mem(shm_inst, sizeof(close_table_request));
  DBUG_EXECUTE_IF("close_table_shm_oom", { req = NULL; });
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(close_table_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_CLOSE_TABLE, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_write_row(tianchi_handler_t *tch, const record_info_t *record_info,
  uint16_t serial_column_offset, uint64_t *last_insert_id, dml_flag_t flag) {
  void *shm_inst = get_one_shm_inst(tch);
  write_row_request *req = (write_row_request*)alloc_share_mem(shm_inst, sizeof(write_row_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(write_row_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->record_len = record_info->record_len;
  req->record = record_info->record;
  req->serial_column_offset = serial_column_offset;
  req->flag = flag;
  int result = ERR_CONNECTION_FAILED;
  int ret = CT_SUCCESS;
  if (req->flag.write_through) {
    ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_WRITE_THROUGH_ROW, req, tch->msg_buf);
  } else {
    ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_WRITE_ROW, req, tch->msg_buf);
  }
  tch->sql_stat_start = req->tch.sql_stat_start;
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
    *last_insert_id = req->last_insert_id;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_bulk_write(tianchi_handler_t *tch, const record_info_t *record_info, uint64_t rec_num,
                   uint32_t *err_pos, dml_flag_t flag, ctc_part_t *part_ids) {
  void *shm_inst = get_one_shm_inst(tch);
  assert(record_info->record_len * rec_num <= MAX_RECORD_SIZE);
  bulk_write_request *req = (bulk_write_request*)alloc_share_mem(shm_inst, sizeof(bulk_write_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(bulk_write_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->record_len = record_info->record_len;
  req->record_num = rec_num;
  req->flag = flag;
  memcpy(req->record, record_info->record, record_info->record_len * rec_num);
  if (part_ids != nullptr) {
    memcpy(req->part_ids, part_ids, rec_num * sizeof(ctc_part_t));
  }
  
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_BULK_INSERT, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
    if (result == ERR_DUPLICATE_KEY) {
      *err_pos = req->err_pos;
    }
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_update_row(tianchi_handler_t *tch, uint16_t new_record_len, const uint8_t *new_record,
                   const uint16_t *upd_cols, uint16_t col_num, dml_flag_t flag) {
  assert(new_record_len < BIG_RECORD_SIZE);
  assert(col_num <= TSE_MAX_COLUMNS);
  void *shm_inst = get_one_shm_inst(tch);
  update_row_request *req = (update_row_request*)alloc_share_mem(shm_inst, sizeof(update_row_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(update_row_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->new_record_len = new_record_len;
  req->col_num = col_num;
  req->new_record = const_cast<uint8_t *>(new_record);
  req->flag = flag;
  memcpy(req->upd_cols, upd_cols, sizeof(uint16_t) * col_num);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_UPDATE_ROW, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_delete_row(tianchi_handler_t *tch, uint16_t record_len, dml_flag_t flag) {
  assert(record_len < BIG_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  delete_row_request *req = (delete_row_request*)alloc_share_mem(shm_inst, sizeof(delete_row_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(delete_row_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->record_len = record_len;
  req->flag = flag;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_DELETE_ROW, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_scan_records(tianchi_handler_t *tch, uint64_t *num_rows, char *index_name) {
  void *shm_inst = get_one_shm_inst(tch);
  scan_records_request *req = (scan_records_request*)alloc_share_mem(shm_inst, sizeof(scan_records_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(scan_records_request));
    return ERR_ALLOC_MEMORY;
  }
  memset(req, 0, sizeof(scan_records_request));
  if (index_name != nullptr) {
    memcpy(req->index_name, index_name, strlen(index_name) + 1);
  }
  req->tch = *tch;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_SCAN_RECORDS, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    if (req->result == CT_SUCCESS) {
      *num_rows = req->num_rows;
    }
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_rnd_next(tianchi_handler_t *tch, record_info_t *record_info) {
  void *shm_inst = get_one_shm_inst(tch);
  rnd_next_request *req = (rnd_next_request*)alloc_share_mem(shm_inst, sizeof(rnd_next_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(rnd_next_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->record = record_info->record;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RND_NEXT, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    if(req->result == CT_SUCCESS) {
      record_info->record_len = req->record_len;
      assert(record_info->record_len < BIG_RECORD_SIZE);
    }
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_rnd_prefetch(tianchi_handler_t *tch, uint8_t *records, uint16_t *record_lens,
                     uint32_t *recNum, uint64_t *rowids, int32_t max_row_size)
{
  void *shm_inst = get_one_shm_inst(tch);
  rnd_prefetch_request *req = (rnd_prefetch_request*)alloc_share_mem(shm_inst, sizeof(rnd_prefetch_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(rnd_prefetch_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->max_row_size = max_row_size;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RND_PREFETCH, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    if (req->result == CT_SUCCESS) {
      *recNum = *(req->recNum);
      if (*recNum != 0) {
        uint32_t record_len = 0;
        for(uint8_t i = 0; i < *recNum; i ++){
          record_len += req->record_lens[i];
        }
        memcpy(records, req->records, record_len);
        memcpy(record_lens, req->record_lens, *recNum * sizeof(uint16_t));
        memcpy(rowids, req->rowids, *recNum * sizeof(uint64_t));
      }
    }
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_rnd_init(tianchi_handler_t *tch, expected_cursor_action_t action, tse_select_mode_t mode, tse_conds *cond) {
  void *shm_inst = get_one_shm_inst(tch);
  rnd_init_request *req = (rnd_init_request*)alloc_share_mem(shm_inst, sizeof(rnd_init_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(rnd_init_request));
    return ERR_ALLOC_MEMORY;
  }

  req->tch = *tch;
  req->action = action;
  req->mode = mode;
  req->cond = cond;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RND_INIT, req, tch->msg_buf);
  *tch = req->tch;
  tch->sql_stat_start = req->tch.sql_stat_start;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_rnd_end(tianchi_handler_t *tch) {
  void *shm_inst = get_one_shm_inst(tch);
  rnd_end_request *req = (rnd_end_request*)alloc_share_mem(shm_inst, sizeof(rnd_end_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(rnd_end_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RND_END, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_position(tianchi_handler_t *tch, uint8_t *position, uint16_t pos_length) {
  assert(pos_length < SMALL_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  position_request *req = (position_request*)alloc_share_mem(shm_inst, sizeof(position_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(position_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->pos_length = pos_length;
  memcpy(req->position, position, pos_length);

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_POSITION, req, tch->msg_buf);
  if (ret == CT_SUCCESS) {
    memcpy(position, req->position, pos_length);
  }
  result = req->result;
  free_share_mem(shm_inst, req);
  return result;
}

int tse_rnd_pos(tianchi_handler_t *tch, uint16_t pos_length, uint8_t *position, record_info_t *record_info) {
  assert(pos_length < SMALL_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  rnd_pos_request *req = (rnd_pos_request*)alloc_share_mem(shm_inst, sizeof(rnd_pos_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(rnd_pos_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->record = record_info->record;
  req->pos_length = pos_length;
  memcpy(req->position, position, pos_length);

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RND_POS, req, tch->msg_buf);
  if (ret == CT_SUCCESS) {
    if(req->result == CT_SUCCESS) {
      record_info->record_len = req->record_len;
      assert(record_info->record_len < BIG_RECORD_SIZE);
    }
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_delete_all_rows(tianchi_handler_t *tch, dml_flag_t flag) {
  void *shm_inst = get_one_shm_inst(tch);
  delete_all_rows_request *req = (delete_all_rows_request*)alloc_share_mem(shm_inst, sizeof(delete_all_rows_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(delete_all_rows_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->flag = flag;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_DELETE_ALL_ROWS, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_index_end(tianchi_handler_t *tch) { 
  void *shm_inst = get_one_shm_inst(tch);
  index_end_request *req = (index_end_request*)alloc_share_mem(shm_inst, sizeof(index_end_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(index_end_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_INDEX_END, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

static void copy_index_info_to_req(const index_key_info_t *index_info, index_read_request *req) {
  uint32_t left_offset = 0;
  uint32_t right_offset = 0;
  uint8_t *pLeft = req->left_key_record;
  uint8_t *pRight = req->right_key_record;
  for (uint i = 0; i < MAX_KEY_COLUMNS; ++i) {
    req->is_key_null[i] = true;
  }

  for (int i = 0; i < index_info->key_num; ++i) {
    req->is_key_null[i] = index_info->key_info[i].is_key_null;
    if (index_info->key_info[i].left_key != nullptr) {
      assert(left_offset + index_info->key_info[i].left_key_len <= INDEX_KEY_SIZE);
      memcpy(pLeft, index_info->key_info[i].left_key, index_info->key_info[i].left_key_len);
      pLeft += index_info->key_info[i].left_key_len;
      req->left_key_info.key_offsets[i] = left_offset;
      req->left_key_info.key_lens[i] = index_info->key_info[i].left_key_len;
      left_offset += req->left_key_info.key_lens[i];
    } else {
      req->left_key_info.key_lens[i] = 0;
      req->left_key_info.key_offsets[i] = 0;
    }
 
    if (index_info->key_info[i].right_key != nullptr) {
      assert(right_offset + index_info->key_info[i].right_key_len <= INDEX_KEY_SIZE);
      memcpy(pRight, index_info->key_info[i].right_key, index_info->key_info[i].right_key_len);
      pRight += index_info->key_info[i].right_key_len;
      req->right_key_info.key_offsets[i] = right_offset;
      req->right_key_info.key_lens[i] = index_info->key_info[i].right_key_len;
      right_offset += req->right_key_info.key_lens[i];
    } else {
      req->right_key_info.key_lens[i] = 0;
      req->right_key_info.key_offsets[i] = 0;
    }
  }

  req->need_init = index_info->need_init;
  req->find_flag = index_info->find_flag;
  req->action = index_info->action;
  req->sorted = index_info->sorted;
  req->key_num = index_info->key_num;
  req->index_skip_scan = index_info->index_skip_scan;
  memcpy(req->index_name, index_info->index_name, strlen(index_info->index_name) + 1);

  return;
}

int tse_index_read(tianchi_handler_t *tch, record_info_t *record_info, index_key_info_t *index_info,
                   tse_select_mode_t mode, tse_conds *cond, const bool is_replace) {
  if (index_info == NULL) {
    return ERR_GENERIC_INTERNAL_ERROR;
  }

  void *shm_inst = get_one_shm_inst(tch);
  index_read_request *req = (index_read_request*)alloc_share_mem(shm_inst, sizeof(index_read_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(index_read_request));
    return ERR_ALLOC_MEMORY;
  }
 
  copy_index_info_to_req(index_info, req);

  req->tch = *tch;
  req->record = record_info->record;
  req->record_len = 0;
  req->mode = mode;
  req->cond = cond;
  req->is_replace = is_replace;
  req->result = 0;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_INDEX_READ, req, tch->msg_buf);
  *tch = req->tch;
  tch->sql_stat_start = req->tch.sql_stat_start;
  if (ret == CT_SUCCESS) {
    if(req->result == CT_SUCCESS) {
      record_info->record_len = req->record_len;
      assert(record_info->record_len < BIG_RECORD_SIZE);
    }
    result = req->result;
    index_info->need_init = req->need_init;
  }

  free_share_mem(shm_inst, req);

  return result;
}

int tse_trx_begin(tianchi_handler_t *tch, tianchi_trx_context_t trx_context, bool is_mysql_local) {
  void *shm_inst = get_one_shm_inst(tch);
  trx_begin_request *req = (trx_begin_request*)alloc_share_mem(shm_inst, sizeof(trx_begin_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(trx_begin_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->trx_context = trx_context;
  req->is_mysql_local = is_mysql_local;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_TRX_BEGIN, req, tch->msg_buf);
  *tch = req->tch; // 此处不管参天处理成功与否，都需要拷贝一次，避免session泄漏
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_trx_commit(tianchi_handler_t *tch, uint64_t *cursors, int32_t csize, bool *is_ddl_commit) {
  void *shm_inst = get_one_shm_inst(tch);
  trx_commit_request *req = (trx_commit_request*)alloc_share_mem(shm_inst, sizeof(trx_commit_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(trx_commit_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->csize = csize;
  req->cursors = cursors;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_TRX_COMMIT, req, tch->msg_buf);
  *is_ddl_commit = req->is_ddl_commit; 
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_trx_rollback(tianchi_handler_t *tch, uint64_t *cursors, int32_t csize) {
  void *shm_inst = get_one_shm_inst(tch);
  trx_rollback_request *req = (trx_rollback_request*)alloc_share_mem(shm_inst, sizeof(trx_rollback_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(trx_rollback_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->csize = csize;
  req->cursors = cursors;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_TRX_ROLLBACK, req, tch->msg_buf);
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_srv_set_savepoint(tianchi_handler_t *tch, const char *name) {
  assert(strlen(name) + 1 < SMALL_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  srv_set_savepoint_request *req = (srv_set_savepoint_request*)alloc_share_mem(shm_inst, sizeof(srv_set_savepoint_request));
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(srv_set_savepoint_request));
      return ERR_ALLOC_MEMORY;
  }
  memcpy(req->name, name, strlen(name) + 1);
  req->tch = *tch;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_SRV_SET_SAVEPOINT, req, tch->msg_buf);
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_srv_rollback_savepoint(tianchi_handler_t *tch, uint64_t *cursors, int32_t csize, const char *name) {
  assert(strlen(name) + 1 < SMALL_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  srv_rollback_savepoint_request *req = (srv_rollback_savepoint_request*)alloc_share_mem(shm_inst, sizeof(srv_rollback_savepoint_request));
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(srv_rollback_savepoint_request));
      return ERR_ALLOC_MEMORY;
  }
  memcpy(req->name, name, strlen(name) + 1);
  req->tch = *tch;
  req->csize = csize;
  req->cursors = cursors;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_SRV_ROLLBACK_SAVEPOINT, req, tch->msg_buf);
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_srv_release_savepoint(tianchi_handler_t *tch, const char *name) {
  assert(strlen(name) + 1 < SMALL_RECORD_SIZE);
  void *shm_inst = get_one_shm_inst(tch);
  srv_release_savepoint_request *req = (srv_release_savepoint_request*)alloc_share_mem(shm_inst, sizeof(srv_release_savepoint_request));
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(srv_release_savepoint_request));
      return ERR_ALLOC_MEMORY;
  }
  memcpy(req->name, name, strlen(name) + 1);
  req->tch = *tch;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_SRV_RELEASE_SAVEPOINT, req, tch->msg_buf);
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_general_fetch(tianchi_handler_t *tch, record_info_t *record_info) {
  void *shm_inst = get_one_shm_inst(tch);
  general_fetch_request *req = (general_fetch_request*)alloc_share_mem(shm_inst, sizeof(general_fetch_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(general_fetch_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->record = record_info->record;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_GENERAL_FETCH, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    if (req->result == CT_SUCCESS) {
      record_info->record_len = req->record_len;
      assert(record_info->record_len < BIG_RECORD_SIZE);
    }
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_free_session_cursors(tianchi_handler_t *tch, uint64_t *cursors, int32_t csize) {
  void *shm_inst = get_one_shm_inst(tch);
  free_session_cursors_request *req = (free_session_cursors_request*)alloc_share_mem(shm_inst, sizeof(free_session_cursors_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(free_session_cursors_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->csize = csize;
  req->cursors = cursors;
 
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_FREE_CURSORS, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_get_max_sessions_per_node(uint32_t *max_sessions) {
  void *shm_inst = get_one_shm_inst(NULL);
  get_max_session_request *req = (get_max_session_request*)alloc_share_mem(shm_inst, sizeof(get_max_session_request));
  DBUG_EXECUTE_IF("tse_get_max_sessions_shm_oom", { req = NULL; });
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(get_max_session_request));
    return ERR_ALLOC_MEMORY;
  }
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_GET_MAX_SESSIONS, req, nullptr);
  if (ret == CT_SUCCESS) {
    *max_sessions = req->max_sessions;
  }
  return ret;
}

int tse_analyze_table(tianchi_handler_t *tch, const char *db_name, const char *table_name, double sampling_ratio) {
  assert(strlen(db_name) + 1 <= SMALL_RECORD_SIZE);
  assert(strlen(table_name) + 1 <= SMALL_RECORD_SIZE);

  void *shm_inst = get_one_shm_inst(tch);
  analyze_table_request *req = (analyze_table_request*)alloc_share_mem(shm_inst, sizeof(analyze_table_request));
  DBUG_EXECUTE_IF("tse_analyze_table_shm_oom", { req = NULL; });
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(analyze_table_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->ratio = sampling_ratio;
  memcpy(req->table_name, table_name, strlen(table_name) + 1);
  memcpy(req->user_name, db_name, strlen(db_name) + 1);

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_ANALYZE, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

void ctc_cbo_stats_columns_copy(tse_cbo_stats_column_t *dst_columns, tse_cbo_stats_column_t *src_columns,
                                tianchi_cbo_stats_t *stats, uint num_columns) {
  for (uint j = 0; j < num_columns; j++) {
    dst_columns[j].num_null = src_columns[j].num_null;
    dst_columns[j].density = src_columns[j].density;
    dst_columns[j].hist_type = src_columns[j].hist_type;
    dst_columns[j].hist_count = src_columns[j].hist_count;
    if (stats->col_type[j] == true) {
      memcpy(dst_columns[j].high_value.v_str,
             src_columns[j].high_value.v_str, CBO_STRING_MAX_LEN);
      memcpy(dst_columns[j].low_value.v_str,
             src_columns[j].low_value.v_str, CBO_STRING_MAX_LEN);
    } else {
      memcpy(&dst_columns[j].high_value,
             &src_columns[j].high_value, sizeof(cache_variant_t));
      memcpy(&dst_columns[j].low_value,
             &src_columns[j].low_value, sizeof(cache_variant_t));
    }
    uint hist_count = src_columns[j].hist_count;
    for (uint k = 0; k < hist_count; k++) {
      dst_columns[j].column_hist[k].ep_number = src_columns[j].column_hist[k].ep_number;
      if (stats->col_type[j] == true) {
        memcpy(dst_columns[j].column_hist[k].ep_value.v_str,
               src_columns[j].column_hist[k].ep_value.v_str, CBO_STRING_MAX_LEN);
      } else {
        memcpy(&dst_columns[j].column_hist[k].ep_value,
               &src_columns[j].column_hist[k].ep_value, sizeof(cache_variant_t));
      }
    }
  }
}

void ctc_cbo_stats_copy_from_shm(tianchi_handler_t *tch, tse_cbo_stats_table_t *tse_cbo_stats_table,
                                 get_cbo_stats_request *req, tianchi_cbo_stats_t *stats) {
  bool is_part_table = stats->part_cnt ? true : false;
  stats->is_updated = req->stats->is_updated;
  stats->records = req->stats->records;
  stats->stats_version = req->stats->stats_version;
  memcpy(stats->ndv_keys, req->stats->ndv_keys, stats->key_len);
  uint num_columns = req->stats->msg_len / sizeof(tse_cbo_stats_column_t);
  if (!is_part_table) {
      *tch = req->tch;
      tse_cbo_stats_table->estimate_rows = req->tse_cbo_stats_table->estimate_rows;
      ctc_cbo_stats_columns_copy(tse_cbo_stats_table->columns,
                                 req->tse_cbo_stats_table->columns, stats, num_columns);
  } else {
    for (uint i = 0; i < req->num_part_fetch; i++) {
      tse_cbo_stats_table[i].estimate_rows = req->tse_cbo_stats_table[i].estimate_rows;
      ctc_cbo_stats_columns_copy(tse_cbo_stats_table[i].columns,
                                 req->tse_cbo_stats_table[i].columns, stats, num_columns);
    }
  }
}

int tse_get_cbo_stats(tianchi_handler_t *tch, tianchi_cbo_stats_t *stats, tse_cbo_stats_table_t *tse_cbo_stats_table, uint32_t first_partid, uint32_t num_part_fetch) {
  void *shm_inst_4_req = get_one_shm_inst(tch);
  get_cbo_stats_request *req = (get_cbo_stats_request*)alloc_share_mem(shm_inst_4_req, sizeof(get_cbo_stats_request));

  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst_4_req, sizeof(get_cbo_stats_request));
    return ERR_ALLOC_MEMORY;
  }
  void *shm_inst_4_stats = get_one_shm_inst(tch);
  req->stats = (tianchi_cbo_stats_t *)alloc_share_mem(shm_inst_4_stats, sizeof(tianchi_cbo_stats_t));
  if (req->stats == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst_4_stats, sizeof(tianchi_cbo_stats_t));
    free_share_mem(shm_inst_4_req, req);
    return ERR_ALLOC_MEMORY;
  }

  bool is_part_table = stats->part_cnt ? true : false;
  req->stats->msg_len = stats->msg_len;
  req->stats->part_cnt = stats->part_cnt;
  req->first_partid = first_partid;
  req->num_part_fetch = num_part_fetch;
  req->stats->stats_version = stats->stats_version;
  void *shm_inst_4_columns = get_one_shm_inst(tch);
  void *shm_inst_4_keys = get_one_shm_inst(tch);
  void *shm_inst_4_table = get_one_shm_inst(tch);
  void *shm_inst_4_str_stats = get_one_shm_inst(tch);
  char *shm_mem_4_str_stats_begin;
  tse_cbo_stats_column_t* part_columns;
  req->stats->ndv_keys = (uint32_t*)alloc_share_mem(shm_inst_4_keys, stats->key_len);
  if (req->stats->ndv_keys == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%u)", shm_inst_4_keys, stats->key_len);
    free_share_mem(shm_inst_4_stats, req->stats);
    free_share_mem(shm_inst_4_req, req);
    return ERR_ALLOC_MEMORY;
  }
  if (!is_part_table) {
    req->tse_cbo_stats_table = 
        (tse_cbo_stats_table_t*)alloc_share_mem(shm_inst_4_table, sizeof(tse_cbo_stats_table_t));
    if (req->tse_cbo_stats_table == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst_4_table, sizeof(tse_cbo_stats_table_t));
      free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
      free_share_mem(shm_inst_4_stats, req->stats);
      free_share_mem(shm_inst_4_req, req);
      return ERR_ALLOC_MEMORY;
    }
    req->tse_cbo_stats_table->columns = (tse_cbo_stats_column_t*)alloc_share_mem(shm_inst_4_columns, req->stats->msg_len);
    if (req->tse_cbo_stats_table->columns == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%u)", shm_inst_4_columns, req->stats->msg_len);
      free_share_mem(shm_inst_4_table, req->tse_cbo_stats_table);
      free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
      free_share_mem(shm_inst_4_stats, req->stats);
      free_share_mem(shm_inst_4_req, req);
      return ERR_ALLOC_MEMORY;
    }
    memset(req->tse_cbo_stats_table->columns, 0, req->stats->msg_len);

    shm_mem_4_str_stats_begin = (char*)alloc_share_mem(shm_inst_4_str_stats,
        stats->num_str_cols * (STATS_HISTGRAM_MAX_SIZE + 2) * CBO_STRING_MAX_LEN);
    if (shm_mem_4_str_stats_begin == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%u)", shm_inst_4_str_stats,
          stats->num_str_cols * (STATS_HISTGRAM_MAX_SIZE + 2) * CBO_STRING_MAX_LEN);
      free_share_mem(shm_inst_4_columns, req->tse_cbo_stats_table->columns);
      free_share_mem(shm_inst_4_table, req->tse_cbo_stats_table);
      free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
      free_share_mem(shm_inst_4_stats, req->stats);
      free_share_mem(shm_inst_4_req, req);
      return ERR_ALLOC_MEMORY;
    }
    char *shm_mem_4_str_stats = shm_mem_4_str_stats_begin;
    for (uint i = 0; i < req->stats->msg_len / sizeof(tse_cbo_stats_column_t); i++) {
      if (stats->col_type[i] == true) {
        req->tse_cbo_stats_table->columns[i].high_value.v_str = shm_mem_4_str_stats;
        req->tse_cbo_stats_table->columns[i].low_value.v_str = shm_mem_4_str_stats + CBO_STRING_MAX_LEN;
        shm_mem_4_str_stats = shm_mem_4_str_stats + CBO_STRING_MAX_LEN * 2;
        for (uint j = 0; j < STATS_HISTGRAM_MAX_SIZE; j++) {
          req->tse_cbo_stats_table->columns[i].column_hist[j].ep_value.v_str = shm_mem_4_str_stats;
          shm_mem_4_str_stats = shm_mem_4_str_stats + CBO_STRING_MAX_LEN;
        }
      }
    }
  } else {
    req->tse_cbo_stats_table = 
        (tse_cbo_stats_table_t*)alloc_share_mem(shm_inst_4_table, num_part_fetch * sizeof(tse_cbo_stats_table_t));
    if (req->tse_cbo_stats_table == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst_4_table, num_part_fetch * sizeof(tse_cbo_stats_table_t));
      free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
      free_share_mem(shm_inst_4_stats, req->stats);
      free_share_mem(shm_inst_4_req, req);
      return ERR_ALLOC_MEMORY;
    }
    part_columns = (tse_cbo_stats_column_t*)alloc_share_mem(shm_inst_4_columns, stats->msg_len * num_part_fetch);
    if (part_columns == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%u)", shm_inst_4_columns, stats->msg_len * num_part_fetch);
      free_share_mem(shm_inst_4_table, req->tse_cbo_stats_table);
      free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
      free_share_mem(shm_inst_4_stats, req->stats);
      free_share_mem(shm_inst_4_req, req);
      return ERR_ALLOC_MEMORY;
    }
    memset(part_columns, 0, stats->msg_len * num_part_fetch);
    for (uint i = 0; i < num_part_fetch; i++) {
      req->tse_cbo_stats_table[i].columns = part_columns + i * (stats->msg_len / sizeof(tse_cbo_stats_column_t));
    }

    shm_mem_4_str_stats_begin = (char *)alloc_share_mem(shm_inst_4_str_stats,
        stats->num_str_cols * num_part_fetch * ((STATS_HISTGRAM_MAX_SIZE + 2) * CBO_STRING_MAX_LEN));
    if (shm_mem_4_str_stats_begin == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%u)", shm_inst_4_str_stats,
                    stats->num_str_cols * num_part_fetch * ((STATS_HISTGRAM_MAX_SIZE + 2) * CBO_STRING_MAX_LEN));
      free_share_mem(shm_inst_4_table, req->tse_cbo_stats_table);
      free_share_mem(shm_inst_4_columns, part_columns);
      free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
      free_share_mem(shm_inst_4_stats, req->stats);
      free_share_mem(shm_inst_4_req, req);
      return ERR_ALLOC_MEMORY;
    }
    char *shm_mem_4_str_stats = shm_mem_4_str_stats_begin;
    for (uint i = 0; i < num_part_fetch; i++) {
      for (uint j = 0; j < req->stats->msg_len / sizeof(tse_cbo_stats_column_t); j++) {
        if (stats->col_type[j] == true) {
          req->tse_cbo_stats_table[i].columns[j].high_value.v_str = shm_mem_4_str_stats;
          req->tse_cbo_stats_table[i].columns[j].low_value.v_str = shm_mem_4_str_stats + CBO_STRING_MAX_LEN;
          shm_mem_4_str_stats = shm_mem_4_str_stats + CBO_STRING_MAX_LEN * 2;
          for (uint k = 0; k < STATS_HISTGRAM_MAX_SIZE; k++) {
            req->tse_cbo_stats_table[i].columns[j].column_hist[k].ep_value.v_str = shm_mem_4_str_stats;
            shm_mem_4_str_stats = shm_mem_4_str_stats + CBO_STRING_MAX_LEN;
          }
        }
      }
    }
  }

  req->tch = *tch;
  int result = 0;
  int ret = tse_mq_deal_func(shm_inst_4_req, TSE_FUNC_TYPE_GET_CBO_STATS, req, tch->msg_buf);
  if (ret == CT_SUCCESS && stats->stats_version != req->stats->stats_version) {
    if (req->result == CT_SUCCESS) {
      ctc_cbo_stats_copy_from_shm(tch, tse_cbo_stats_table, req, stats);
    }
    result = req->result;
  }
  free_share_mem(shm_inst_4_str_stats, shm_mem_4_str_stats_begin);
  if (!is_part_table) {
    free_share_mem(shm_inst_4_columns, req->tse_cbo_stats_table->columns);
  } else {
    free_share_mem(shm_inst_4_columns, part_columns);
  }
  free_share_mem(shm_inst_4_keys, req->stats->ndv_keys);
  free_share_mem(shm_inst_4_table, req->tse_cbo_stats_table);
  free_share_mem(shm_inst_4_stats, req->stats);
  free_share_mem(shm_inst_4_req, req);
  return ret == CT_SUCCESS ? result : ERR_CONNECTION_FAILED;
}

int tse_get_index_name(tianchi_handler_t *tch, char *index_name) {
  void *shm_inst = get_one_shm_inst(tch);
  get_index_slot_request *req = (get_index_slot_request*)alloc_share_mem(shm_inst, sizeof(get_index_slot_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%d)", shm_inst, REQUEST_SIZE);
    return ERR_ALLOC_MEMORY;
  }
  memset(req, 0, sizeof(get_index_slot_request));
  req->tch = *tch;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_GET_INDEX_NAME, req, tch->msg_buf);

  if (ret == CT_SUCCESS) {
    memcpy(index_name, req->index_name, strlen(req->index_name) + 1);
    result = req->result;
  } else {
    result = ret;
    tse_log_error("tse_get_index_name failed: %d", ret);
  }

  free_share_mem(shm_inst, req);
  return result;
}

uint8_t* tse_alloc_buf(tianchi_handler_t *tch, uint32_t buf_size) {
  if (buf_size == 0) {
    return nullptr;
  }
  void *shm_inst = get_one_shm_inst(tch);
  return (uint8_t*)alloc_share_mem(shm_inst, buf_size);
}

void tse_free_buf(tianchi_handler_t *tch, uint8_t *buf) {
  if (buf == nullptr) {
    return;
  }
  void *shm_inst = get_one_shm_inst(tch);
  free_share_mem(shm_inst, buf);
}

int tse_general_prefetch(tianchi_handler_t *tch, uint8_t *records, uint16_t *record_lens,
                         uint32_t *recNum, uint64_t *rowids, int32_t max_row_size) {
  void *shm_inst = get_one_shm_inst(tch);
  general_prefetch_request *req = (general_prefetch_request*)alloc_share_mem(shm_inst, sizeof(general_prefetch_request));
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(general_prefetch_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->max_row_size = max_row_size;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_GENERAL_PREFETCH, req, tch->msg_buf);
  *tch = req->tch;

  if (ret == CT_SUCCESS) {
    *recNum = *(req->recNum);
    if (*recNum != 0) {
      uint32_t record_len = 0;
      for(uint8_t i = 0; i < *recNum; i ++){
        record_len += req->record_lens[i];
      }
      memcpy(records, req->records, record_len);
      memcpy(record_lens, req->record_lens, *recNum * sizeof(uint16_t));
      memcpy(rowids, req->rowids, *recNum * sizeof(uint64_t));
    }
    result = req->result;
  } else {
    result = ret;
    tse_log_error("cantiand deal the message failed: %d", ret);
  }

  free_share_mem(shm_inst, req);
  return result;
}

int tse_drop_tablespace_and_user(tianchi_handler_t *tch,
                                 const char *db_name,
                                 const char *sql_str,
                                 const char *user_name,
                                 const char *user_ip,
                                 int *error_code,
                                 char *error_message)
{
  void *shm_inst = get_one_shm_inst(tch);
  drop_tablespace_and_user_request *req = (drop_tablespace_and_user_request*)alloc_share_mem(shm_inst, sizeof(drop_tablespace_and_user_request));
  DBUG_EXECUTE_IF("drop_tablespace_and_user_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(drop_tablespace_and_user_request));
      return ERR_ALLOC_MEMORY;
  }
  assert(strlen(db_name) + 1 <= sizeof(req->db_name));
  memcpy(req->db_name, db_name, strlen(db_name) + 1);
  assert(strlen(sql_str) + 1 <= sizeof(req->sql_str));
  memcpy(req->sql_str, sql_str, strlen(sql_str) + 1);

  assert(strlen(user_name) + 1 <= sizeof(req->user_name));
  memcpy(req->user_name, user_name, strlen(user_name) + 1);
  assert(strlen(user_ip) + 1 <= sizeof(req->user_ip));
  memcpy(req->user_ip, user_ip, strlen(user_ip) + 1);
  TSE_RESET_SHM_REQ_ERROR_CODE(req);
  req->tch = *tch;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_DROP_TABLESPACE_AND_USER, req, tch->msg_buf);
  *tch = req->tch;
  if(req->error_message != NULL && strlen(req->error_message) > 0) {
    *error_code = req->error_code;
    memcpy(error_message, req->error_message, ERROR_MESSAGE_LEN);
  }
  free_share_mem(shm_inst, req);
  int result = ERR_CONNECTION_FAILED;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  return result;
}

int tse_drop_db_pre_check(tianchi_handler_t *tch, const char *db_name, int *error_code, char *error_message) {
  void *shm_inst = get_one_shm_inst(tch);
  drop_db_pre_check_request *req = (drop_db_pre_check_request*)alloc_share_mem(shm_inst, sizeof(drop_db_pre_check_request));
  req->tch = *tch;
  assert(strlen(db_name) + 1 <= sizeof(req->db_name));
  memcpy(req->db_name, db_name, strlen(db_name) + 1);
  TSE_RESET_SHM_REQ_ERROR_CODE(req);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_DROP_DB_PRE_CHECK, req, tch->msg_buf);
  *tch = req->tch;
  *error_code = req->error_code;
  strncpy(error_message, req->error_message, ERROR_MESSAGE_LEN - 1);
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_lock_table(tianchi_handler_t *tch, const char *db_name, tse_lock_table_info *lock_info,
    int *error_code) {
  void *shm_inst = get_one_shm_inst(tch);
  lock_table_request *req = (lock_table_request*)alloc_share_mem(shm_inst, sizeof(lock_table_request));
  DBUG_EXECUTE_IF("lock_table_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(lock_table_request));
      return ERR_ALLOC_MEMORY;
  }
  
  req->db_name[0] = '\0';
  if (db_name != nullptr) {
    strncpy(req->db_name, db_name, strlen(db_name) + 1);
  }
  
  req->tch = *tch;
  req->lock_info = *lock_info;
  req->mysql_inst_id = tch->inst_id;
  TSE_RESET_SHM_REQ_ERROR_CODE(req);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_LOCK_TABLE, req, tch->msg_buf);
  *tch = req->tch;
  *error_code = req->error_code;
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_pre_create_db(tianchi_handler_t *tch, const char *sql_str, tse_db_infos_t *db_infos,
                      int *error_code, char *error_message)
{
  void *shm_inst = get_one_shm_inst(tch);
  pre_create_db_request *req = (pre_create_db_request*)alloc_share_mem(shm_inst, sizeof(pre_create_db_request));
  DBUG_EXECUTE_IF("create_tablespace_and_user_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(pre_create_db_request));
      return ERR_ALLOC_MEMORY;
  }
  assert(strlen(sql_str) + 1 <= sizeof(req->sql_str));
  memcpy(req->sql_str, sql_str, strlen(sql_str) + 1);
  assert(strlen(db_infos->name) + 1 <= sizeof(req->db_name));
  memcpy(req->db_name, db_infos->name, strlen(db_infos->name) + 1);

  req->ctc_db_datafile_size = db_infos->datafile_size;
  req->ctc_db_datafile_autoextend = db_infos->datafile_autoextend;
  req->ctc_db_datafile_extend_size = db_infos->datafile_extend_size;
  req->tch = *tch;
  TSE_RESET_SHM_REQ_ERROR_CODE(req);
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_PRE_CREATE_DB, req, tch->msg_buf);
  *tch = req->tch;
  if(req->error_message != NULL && strlen(req->error_message) > 0) {
    *error_code = req->error_code;
    memcpy(error_message, req->error_message, ERROR_MESSAGE_LEN);
  }
  free_share_mem(shm_inst, req);
  int result = ERR_CONNECTION_FAILED;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  return result;
}

int tse_unlock_table(tianchi_handler_t *tch, uint32_t mysql_insert_id, tse_lock_table_info *lock_info) {
  void *shm_inst = get_one_shm_inst(tch);
  tse_unlock_tables_request *req = (tse_unlock_tables_request*)alloc_share_mem(shm_inst, sizeof(tse_unlock_tables_request));
  DBUG_EXECUTE_IF("unlock_table_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(tse_unlock_tables_request));
      return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->mysql_inst_id = mysql_insert_id;
  req->lock_info = *lock_info;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_UNLOCK_TABLE, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_knl_write_lob(tianchi_handler_t *tch, char* locator, uint32_t locator_size, int column_id,
                      void* data, uint32_t data_len, bool force_outline)
{
  void *shm_inst = get_one_shm_inst(tch);
  int reqSize = sizeof(knl_write_lob_request) + data_len;
  uchar* reqBuf = new uchar[reqSize];
  knl_write_lob_request *req = (knl_write_lob_request *)reqBuf;
  if (req == NULL) {
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  memcpy(req->locator, locator, locator_size);
  memcpy(req->data, data, data_len);
  req->data_len = data_len;
  req->column_id = column_id;
  req->force_outline = force_outline;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_batch_send_message(shm_inst, TSE_FUNC_TYPE_WRITE_LOB, reqBuf, reqSize, reqSize);
  *tch = req->tch;
  if (ret != CT_SUCCESS) {
    tse_log_error("tse_mq_batch_send_message failed in write lob: %d", ret);
  } else {
    if (req->result == CT_SUCCESS) {
      memcpy(locator, req->locator, locator_size);
    }
    result = req->result;
  }
  delete[] reqBuf;
  return result;
}

int tse_knl_read_lob(tianchi_handler_t *tch, char* locator, uint32_t offset, void *buf, uint32_t size, uint32_t *read_size)
{
  void *shm_inst = get_one_shm_inst(tch);
  int reqSize = sizeof(knl_read_lob_request) + size;
  uchar* reqBuf = new uchar[reqSize];
  knl_read_lob_request *req = (knl_read_lob_request *)reqBuf;
  if (req == NULL) {
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  memcpy(req->locator, locator, OUTLINE_LOB_LOCATOR_SIZE);
  req->offset = offset;
  req->size = size;

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_batch_send_message(shm_inst, TSE_FUNC_TYPE_READ_LOB, reqBuf, reqSize, reqSize);
  if (ret != CT_SUCCESS) {
    tse_log_error("tse_mq_batch_send_message failed in read lob: %d", ret);
  } else {
    if(req->result == CT_SUCCESS) {
      *read_size = (uint32_t)(req->read_size);
      assert(*read_size <= size);
      memcpy(buf, req->buf, *read_size);
    }
    result = req->result;
  }
  delete[] reqBuf;
  return result;
}

int srv_wait_instance_startuped(void)
{
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, 0);
  if (req_mem == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%d)", shm_inst, 0);
    return ERR_ALLOC_MEMORY;
  }
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_WAIT_CONNETOR_STARTUPED, req_mem, nullptr, SERVER_REGISTER_PROC_ID, 5);
  free_share_mem(shm_inst, req_mem);
  return ret;
}

int tse_create_table(void *table_def, ddl_ctrl_t *ddl_ctrl) {
  void *shm_inst = get_one_shm_inst(&ddl_ctrl->tch);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_batch_send_message(shm_inst, TSE_FUNC_TYPE_CREATE_TABLE, (uint8_t*)table_def, ddl_ctrl->msg_len,
                                      ddl_ctrl->msg_len);
  memcpy(ddl_ctrl, table_def, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  return result;
}

int tse_truncate_table(void *table_def, ddl_ctrl_t *ddl_ctrl) {
  assert(ddl_ctrl->msg_len + sizeof(ddl_ctrl_t) < REQUEST_SIZE);
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
  DBUG_EXECUTE_IF("truncate_table_shm_oom", { req_mem = NULL; });
  if (req_mem == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
    return ERR_ALLOC_MEMORY;
  }
  memcpy((char *)req_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  memcpy((char *)req_mem + sizeof(ddl_ctrl_t), table_def, ddl_ctrl->msg_len);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_TRUNCATE_TABLE, req_mem, nullptr);
  memcpy(ddl_ctrl, req_mem, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  free_share_mem(shm_inst, req_mem);
  return result;
}

int tse_truncate_partition(void *table_def, ddl_ctrl_t *ddl_ctrl) {
  void *shm_inst = get_one_shm_inst(&ddl_ctrl->tch);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_batch_send_message(shm_inst, TSE_FUNC_TYPE_TRUNCATE_PARTITION, (uint8_t*)table_def, ddl_ctrl->msg_len,
                                      ddl_ctrl->msg_len);
  memcpy(ddl_ctrl, table_def, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  return result;
}

int tse_alter_table(void *alter_def, ddl_ctrl_t *ddl_ctrl) {
  void *shm_inst = get_one_shm_inst(&ddl_ctrl->tch);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_batch_send_message(shm_inst, TSE_FUNC_TYPE_ALTER_TABLE, (uint8_t*)alter_def, ddl_ctrl->msg_len,
                                      ddl_ctrl->msg_len);
  memcpy(ddl_ctrl, alter_def, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  return result;
}

int tse_rename_table(void *alter_def, ddl_ctrl_t *ddl_ctrl) {
  assert(ddl_ctrl->msg_len + sizeof(ddl_ctrl_t) < REQUEST_SIZE);
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
  DBUG_EXECUTE_IF("rename_table_shm_oom", { req_mem = NULL; });
  if (req_mem == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
    return ERR_ALLOC_MEMORY;
  }
  memcpy((char *)req_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  memcpy((char *)req_mem + sizeof(ddl_ctrl_t), alter_def, ddl_ctrl->msg_len);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RENAME_TABLE, req_mem, nullptr);
  memcpy(ddl_ctrl, req_mem, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  free_share_mem(shm_inst, req_mem);
  return result;
}

int tse_execute_mysql_ddl_sql(tianchi_handler_t *tch, tse_ddl_broadcast_request *broadcast_req, bool allow_fail) {
  void *shm_inst = get_one_shm_inst(tch);
  execute_mysql_ddl_sql_request *req = (execute_mysql_ddl_sql_request*)alloc_share_mem(shm_inst, sizeof(execute_mysql_ddl_sql_request));
  DBUG_EXECUTE_IF("xcute_general_ddl_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(execute_mysql_ddl_sql_request));
      return ERR_ALLOC_MEMORY;
  }
  memcpy(&req->broadcast_req, broadcast_req, sizeof(tse_ddl_broadcast_request));
  req->tch = *tch;
  req->allow_fail = allow_fail;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_EXCUTE_MYSQL_DDL_SQL, req, tch->msg_buf);
  *tch = req->tch;
  broadcast_req->err_code = req->broadcast_req.err_code;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_broadcast_mysql_dd_invalidate(tianchi_handler_t *tch, tse_invalidate_broadcast_request *broadcast_req) {
  void *shm_inst = get_one_shm_inst(tch);
  invalidate_mysql_dd_request *req = (invalidate_mysql_dd_request *)alloc_share_mem(shm_inst, sizeof(invalidate_mysql_dd_request));
  DBUG_EXECUTE_IF("xcute_general_ddl_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(invalidate_mysql_dd_request));
      return ERR_ALLOC_MEMORY;
  }
  memcpy(&req->broadcast_req, broadcast_req, sizeof(tse_invalidate_broadcast_request));
  req->tch = *tch;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_INVALIDATE_OBJECT, req, tch->msg_buf);
  *tch = req->tch;
  broadcast_req->err_code = req->broadcast_req.err_code;
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_broadcast_rewrite_sql(tianchi_handler_t *tch, tse_ddl_broadcast_request *broadcast_req, bool allow_fail) {
  void *shm_inst = get_one_shm_inst(tch);
  execute_mysql_ddl_sql_request *req = (execute_mysql_ddl_sql_request*)alloc_share_mem(shm_inst, sizeof(execute_mysql_ddl_sql_request));
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(execute_mysql_ddl_sql_request));
      return ERR_ALLOC_MEMORY;
  }

  memcpy(&req->broadcast_req, broadcast_req, sizeof(tse_ddl_broadcast_request));
  req->tch = *tch;
  req->allow_fail = allow_fail;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_BROADCAST_REWRITE_SQL, req, tch->msg_buf);
  *tch = req->tch;
  broadcast_req->err_code = req->broadcast_req.err_code;
  memcpy(broadcast_req->err_msg, req->broadcast_req.err_msg, ERROR_MESSAGE_LEN);
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_get_serial_value(tianchi_handler_t *tch, uint64_t *value, dml_flag_t flag) {
  void *shm_inst = get_one_shm_inst(tch);
  get_serial_val_request *req = (get_serial_val_request*)alloc_share_mem(shm_inst, sizeof(get_serial_val_request));
  DBUG_EXECUTE_IF("get_serial_val_ddl_shm_oom", { req = NULL; });
  if (req == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(get_serial_val_request));
    return ERR_ALLOC_MEMORY;
  }
  req->tch = *tch;
  req->flag.auto_inc_step = flag.auto_inc_step;
  req->flag.auto_inc_offset = flag.auto_inc_offset;
  req->flag.auto_increase = flag.auto_increase;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_GET_SERIAL_VALUE, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
    *value = req->value;
    result = req->result;
  }

  free_share_mem(shm_inst, req);
  return result;
}

int tse_drop_table(void *drop_def, ddl_ctrl_t *ddl_ctrl) {
  assert(ddl_ctrl->msg_len + sizeof(ddl_ctrl_t) < REQUEST_SIZE);
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
  DBUG_EXECUTE_IF("drop_table_shm_oom", { req_mem = NULL; });
  if (req_mem == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
    return ERR_ALLOC_MEMORY;
  }
  memcpy((char *)req_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  memcpy((char *)req_mem + sizeof(ddl_ctrl_t), drop_def, ddl_ctrl->msg_len);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_DROP_TABLE, req_mem, nullptr);
  memcpy(ddl_ctrl, req_mem, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  free_share_mem(shm_inst, req_mem);
  return result;
}

int tse_create_tablespace(void *space_def, ddl_ctrl_t *ddl_ctrl) {
  assert(ddl_ctrl->msg_len + sizeof(ddl_ctrl_t) < REQUEST_SIZE);
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
  DBUG_EXECUTE_IF("create_tablespace_shm_oom", { req_mem = NULL; });
  if (req_mem == NULL) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
    return ERR_ALLOC_MEMORY;
  }
  memcpy((char *)req_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  memcpy((char *)req_mem + sizeof(ddl_ctrl_t), space_def, ddl_ctrl->msg_len);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_CREATE_TABLESPACE, req_mem, nullptr);
  memcpy(ddl_ctrl, req_mem, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  free_share_mem(shm_inst, req_mem);
  return result;
}

int tse_alter_tablespace(void *space_alter_def, ddl_ctrl_t *ddl_ctrl) {
  assert(ddl_ctrl->msg_len + sizeof(ddl_ctrl_t) < REQUEST_SIZE);
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
  DBUG_EXECUTE_IF("alter_tablespace_shm_oom", { req_mem = NULL; });
  if (req_mem == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
      return ERR_ALLOC_MEMORY;
  }
  memcpy((char *)req_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  memcpy((char *)req_mem + sizeof(ddl_ctrl_t), space_alter_def, ddl_ctrl->msg_len);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_ALTER_TABLESPACE, req_mem, nullptr);
  memcpy(ddl_ctrl, req_mem, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  free_share_mem(shm_inst, req_mem);
  return result;
}

int tse_drop_tablespace(void *space_drop_def, ddl_ctrl_t *ddl_ctrl) {
  assert(ddl_ctrl->msg_len + sizeof(ddl_ctrl_t) < REQUEST_SIZE);
  void *shm_inst = get_one_shm_inst(NULL);
  void *req_mem = alloc_share_mem(shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
  DBUG_EXECUTE_IF("drop_tablespace_shm_oom", { req_mem = NULL; });
  if (req_mem == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, ddl_ctrl->msg_len + sizeof(ddl_ctrl_t));
      return ERR_ALLOC_MEMORY;
  }
  memcpy((char *)req_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  memcpy((char *)req_mem + sizeof(ddl_ctrl_t), space_drop_def, ddl_ctrl->msg_len);
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_DROP_TABLESPACE, req_mem, nullptr);
  memcpy(ddl_ctrl, req_mem, sizeof(ddl_ctrl_t));
  if (ret == CT_SUCCESS) {
    result = ddl_ctrl->error_code;
  }
  free_share_mem(shm_inst, req_mem);
  return result;
}

int tse_lock_instance(bool *is_mysqld_starting, tse_lock_table_mode_t lock_type, tianchi_handler_t *tch) {
  void *shm_inst = get_one_shm_inst(tch);
  lock_instance_request *req = (lock_instance_request*)alloc_share_mem(shm_inst, sizeof(lock_instance_request));
  DBUG_EXECUTE_IF("lock_instance_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(lock_instance_request));
      return ERR_ALLOC_MEMORY;
  }

  req->tch = *tch;
  req->lock_type = lock_type;
  req->is_mysqld_starting = *is_mysqld_starting;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_LOCK_INSTANCE, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_unlock_instance(bool *is_mysqld_starting, tianchi_handler_t *tch) {
  void *shm_inst = get_one_shm_inst(tch);
  unlock_instance_request *req = (unlock_instance_request*)alloc_share_mem(shm_inst, sizeof(unlock_instance_request));
  DBUG_EXECUTE_IF("unlock_instance_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(unlock_instance_request));
      return ERR_ALLOC_MEMORY;
  }

  req->tch = *tch;
  req->is_mysqld_starting = *is_mysqld_starting;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_UNLOCK_INSTANCE, req, tch->msg_buf);
  *tch = req->tch;
  if (ret == CT_SUCCESS) {
      result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_search_metadata_status(bool *cantian_metadata_switch, bool *cantian_cluster_ready) {
  void *shm_inst = get_one_shm_inst(NULL);
  search_metadata_status_request *req = (search_metadata_status_request*)alloc_share_mem(shm_inst, sizeof(search_metadata_status_request));
  DBUG_EXECUTE_IF("check_init_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(search_metadata_status_request));
      return ERR_ALLOC_MEMORY;
  }
 
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_SEARCH_METADATA_SWITCH, req, nullptr);
  if (ret == CT_SUCCESS) {
      result = req->result;
      *cantian_metadata_switch = req->metadata_switch;
      *cantian_cluster_ready = req->cluster_ready;
  }
  free_share_mem(shm_inst, req);
 
  return result;
}
 
int tse_check_db_table_exists(const char *db, const char *name, bool *is_exists) {
  void *shm_inst = get_one_shm_inst(NULL);
  check_table_exists_request *req = (check_table_exists_request*)alloc_share_mem(shm_inst, sizeof(check_table_exists_request));
  DBUG_EXECUTE_IF("check_init_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(check_table_exists_request));
      return ERR_ALLOC_MEMORY;
  }
  int result = ERR_CONNECTION_FAILED;
  memcpy(req->db, db, strlen(db) + 1);
  memcpy(req->name, name, strlen(name) + 1);
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_CHECK_TABLE_EXIST, req, nullptr);
  if (ret == CT_SUCCESS) {
      result = req->result;
      *is_exists = (bool)req->is_exists;
  }
  free_share_mem(shm_inst, req);
 
  return result;
}

int ctc_record_sql_for_cantian(tianchi_handler_t *tch, tse_ddl_broadcast_request *broadcast_req, bool allow_fail) {
  void *shm_inst = get_one_shm_inst(tch);
  execute_mysql_ddl_sql_request *req = (execute_mysql_ddl_sql_request*)alloc_share_mem(shm_inst, sizeof(execute_mysql_ddl_sql_request));
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(execute_mysql_ddl_sql_request));
      return ERR_ALLOC_MEMORY;
  }

  memcpy(&req->broadcast_req, broadcast_req, sizeof(tse_ddl_broadcast_request));
  req->tch = *tch;
  req->allow_fail = allow_fail;
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_TYPE_RECORD_SQL, req, tch->msg_buf);
  *tch = req->tch;
  broadcast_req->err_code = req->broadcast_req.err_code;
  memcpy(broadcast_req->err_msg, req->broadcast_req.err_msg, ERROR_MESSAGE_LEN);
  if (ret == CT_SUCCESS) {
    result = req->result;
  }
  free_share_mem(shm_inst, req);
  return result;
}

int tse_query_cluster_role(bool *is_slave, bool *cantian_cluster_ready) {
  void *shm_inst = get_one_shm_inst(NULL);
  query_cluster_role_request *req = (query_cluster_role_request*) alloc_share_mem(shm_inst, sizeof(query_cluster_role_request));
  DBUG_EXECUTE_IF("check_init_shm_oom", { req = NULL; });
  if (req == NULL) {
      tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(query_cluster_role_request));
      return ERR_ALLOC_MEMORY;
  }

  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, TSE_FUNC_QUERY_CLUSTER_ROLE, req, nullptr);
  if (ret == CT_SUCCESS) {
    result = req->result;
    *is_slave = req->is_slave;
    *cantian_cluster_ready = req->cluster_ready;
  }
  free_share_mem(shm_inst, req);

  return result;
}

int ctc_update_sample_size(uint32_t sample_size)
{
  void *shm_inst = get_one_shm_inst(nullptr);

  uint32_t *req = (uint32_t*)alloc_share_mem(shm_inst, sizeof(uint32_t));
  if (req == nullptr) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(uint32_t));
    return ERR_ALLOC_MEMORY;
  }
  *req = sample_size;

  int res = tse_mq_deal_func(shm_inst, CTC_FUNC_TYPE_UPDATE_SAMPLE_SIZE, req, nullptr);
  if (res != CT_SUCCESS) {
    tse_log_error("tse_mq_deal_func CTC_FUNC_TYPE_UPDATE_SAMPLE_SIZE failed");
  }
  free_share_mem(shm_inst, req);

  return res;
}

int ctc_query_shm_file_num(uint32_t *shm_file_num)
{
  void *shm_inst = get_one_shm_inst(NULL);
  query_shm_file_num_request *req = (query_shm_file_num_request*)
                                     alloc_share_mem(shm_inst, sizeof(query_shm_file_num_request));
  if (req == nullptr) {
    tse_log_error("alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(query_shm_file_num_request));
    return ERR_ALLOC_MEMORY;
  }
  int result = ERR_CONNECTION_FAILED;
  int ret = tse_mq_deal_func(shm_inst, CTC_FUNC_QUERY_SHM_FILE_NUM, req, nullptr, SERVER_REGISTER_PROC_ID);
  if (ret == CT_SUCCESS) {
    result = req->result;
    *shm_file_num = req->shm_file_num;
  }
  free_share_mem(shm_inst, req);
  return result;
}