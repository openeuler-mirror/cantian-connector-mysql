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

// @file storage/tianchi/ha_tsepart.cc
// description: TIANCHI handler implementation for MySQL storage engine API.
// this module should use tse_part_srv rather than knl_intf
#if NEED_TSE_PART_HANDLER
#include "my_global.h"
#include "ha_tsepart.h"
#include "ha_tse_ddl.h"
#include <errno.h>
#include <limits.h>
//#include <sql/sql_thd_internal_api.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <vector>
//#include "field_types.h"
#include "mysql_com.h"

#include "my_base.h"
#include "my_dbug.h"
//#include "my_macros.h"
//#include "my_pointer_arithmetic.h"
//#include "my_psi_config.h"
#include "mysql/plugin.h"
//#include "sql/current_thd.h"
#include "sql/field.h"
#include "sql/sql_base.h"  // enum_tdc_remove_table_type
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_plugin.h"
#include "sql/partition_info.h"
#include "tse_error.h"
#include "tse_log.h"
#include "srv_mq_msg.h"
#include <atomic>
#include "typelib.h"
#include "tse_cbo.h"
#include "sql/sql_alter.h"

#define INVALID_PART_ID (uint32)0xFFFFFFFF;

extern handlerton *get_tse_hton();
extern uint32_t ctc_update_analyze_time;

constexpr uint64 INVALID_VALUE64 = 0xFFFFFFFFFFFFFFFFULL;
constexpr int max_prefetch_num = MAX_PREFETCH_REC_NUM;

static uint32_t get_ct_sub_no(uint num_subparts, uint part_id) {
  uint sub_no = part_id % num_subparts;
  return (uint32_t)sub_no;
}

static uint32_t get_ct_part_no(uint num_subparts, uint part_id) {
  uint part_no = part_id / num_subparts;
  return (uint32_t)part_no;
}

static bool get_used_partitions(partition_info *part_info,
                                uint32_t **part_ids,uint32_t **subpart_ids, uint32_t *used_parts) {
  *used_parts = part_info->num_partitions_used();
  if (*used_parts > 0) {
    *part_ids = (uint32_t *)my_malloc(PSI_NOT_INSTRUMENTED, (*used_parts) * sizeof(uint32_t), MYF(MY_WME));
     if (*part_ids == NULL) {
      tse_log_system("Failed to allocate memory for part_ids.");
      return false;
    }
    *subpart_ids = (uint32_t *)my_malloc(PSI_NOT_INSTRUMENTED, (*used_parts) * sizeof(uint32_t), MYF(MY_WME));
    if (*subpart_ids == NULL) {
      tse_log_system("Failed to allocate memory for subpart_ids. Freeing previously allocated memory...");
      my_free(*part_ids);
      return false;
    }
    uint32_t part_id = part_info->get_first_used_partition(); 
    if(part_id == MY_BIT_NONE){
      return true;
    }
    if(part_info->num_subparts > 0){
      // subpartition
      for (uint32_t i = 0; i < *used_parts; i++) {
        *(*part_ids + i) = part_id / part_info->num_subparts;
        *(*subpart_ids + i) = part_id % part_info->num_subparts;
        part_id = part_info->get_next_used_partition(part_id);
      }
    }else{
      // partition
      for (uint32_t i = 0; i < *used_parts; i++) {
        *(*part_ids + i) = part_id;
        *(*subpart_ids + i) = 0;
        part_id = part_info->get_next_used_partition(part_id);
      }
    }
  }else{
    tse_log_system("invalid used_parts :%u", *used_parts);
  }
  return true;
}

ha_tsepart::ha_tsepart(handlerton *hton, TABLE_SHARE *table_arg)
    : ha_tse(hton, table_arg), Partition_helper(this),
      m_bulk_insert_parts(nullptr), m_part_share(nullptr) {
  pruned_by_engine = false;
  ref_length = ROW_ID_LENGTH + PARTITION_BYTES_IN_POS;
}

/**
 Open an TSE table.
 @param[in]	name		   table name
 @param[in]	mode		   access mode
 @param[in]	test_if_locked test if the file to be opened is locked
 @param[in]	table_def	   dd::Table describing table to be opened
 @retval 1 if error
 @retval 0 if success
*/
int ha_tsepart::open(const char *name, int mode, uint test_if_locked,
                     const dd::Table *table_def) {
  if (!(m_part_share = get_share<Tsepart_share>())) return HA_ERR_OUT_OF_MEM;

  if (m_part_info == NULL) {
    m_part_info = table->part_info;
  }

  lock_shared_ha_data();
  if (m_part_share->populate_partition_name_hash(m_part_info)) {
    unlock_shared_ha_data();
    free_share<Tsepart_share>();
    return HA_ERR_INTERNAL_ERROR;
  }
  if (m_part_share->auto_inc_mutex == nullptr &&
      table->found_next_number_field != nullptr) {
    if (m_part_share->init_auto_inc_mutex(table_share)) {
      unlock_shared_ha_data();
      free_share<Tsepart_share>();
      return HA_ERR_INTERNAL_ERROR;
    }
  }
  unlock_shared_ha_data();

  int ret = ha_tse::open(name, mode, test_if_locked, table_def);
  m_max_batch_num = m_max_batch_num > MAX_BULK_INSERT_PART_ROWS ? MAX_BULK_INSERT_PART_ROWS : m_max_batch_num;
  
  if (ret != CT_SUCCESS) {
    free_share<Tsepart_share>();
    return ret;
  }
  if (open_partitioning(m_part_share)) {
    return close();
  }

  return ret;
}

int ha_tsepart::close() {
  assert(m_part_share);
  close_partitioning();
  free_share<Tsepart_share>();
  if (m_bulk_insert_parts != nullptr) {
    my_free(m_bulk_insert_parts);
    m_bulk_insert_parts = nullptr;
  }
  return ha_tse::close();
}

void ha_tsepart::set_partition(uint part_id) {
  if (m_is_sub_partitioned) {
    m_tch.part_id = get_ct_part_no(m_part_info->num_subparts, part_id);
    m_tch.subpart_id = get_ct_sub_no(m_part_info->num_subparts, part_id);
  } else {
    m_tch.part_id = part_id;
    m_tch.subpart_id = INVALID_PART_ID;
  }
}

void ha_tsepart::reset_partition(uint part_id) {
  m_tch.part_id = INVALID_PART_ID;
  m_tch.subpart_id = INVALID_PART_ID;
  m_last_part = part_id;   
}

void ha_tsepart::get_auto_increment(ulonglong, ulonglong, ulonglong,
                                    ulonglong *first_value,
                                    ulonglong*) {
  uint64 inc_value;
  update_member_tch(m_tch, get_tse_hton(), ha_thd());
  THD* thd = ha_thd();
  dml_flag_t flag;
  flag.auto_inc_offset = thd->variables.auto_increment_increment;
  flag.auto_inc_step = thd->variables.auto_increment_offset;
  flag.auto_increase = true;
  int ret = tse_get_serial_value(&m_tch, &inc_value, flag);
  update_sess_ctx_by_tch(m_tch, get_tse_hton(), ha_thd());
  if (ret != 0) {
    *first_value = (~(ulonglong)0);
    return;
  }
  *first_value = inc_value;
}

void ha_tsepart::print_error(int error, myf errflag) {
  if (print_partition_error(error)) {
    handler::print_error(error, errflag);
  }
}

void ha_tsepart::part_autoinc_has_expl_non_null_value_update_row(uchar *new_data) {
    if (table->found_next_number_field && new_data == table->record[0] &&
        !table->s->next_number_keypart &&
        bitmap_is_set(table->write_set,
                      table->found_next_number_field->field_index)) {
        autoinc_has_expl_non_null_value_update_row = true;
    }
}

/**
 Write a row in specific partition.
 Stores a row in an TSE database, to the table specified in this
 handle.
 @param[in]	part_id	Partition to write to.
 @param[in]	record	A row in MySQL format.
 @return error code.
*/
int ha_tsepart::write_row_in_part(uint part_id, uchar *record) {
  bool saved_autoinc_has_expl_non_null = table->autoinc_field_has_explicit_non_null_value;
  Field *saved_next_number_field = table->next_number_field;
  THD *thd = ha_thd();
  if (!autoinc_has_expl_non_null_value_update_row &&
      table->next_number_field && !autoinc_has_expl_non_null_value) {
      table->autoinc_field_has_explicit_non_null_value = false;
      table->next_number_field->store(0, true);
  }
  if (table->found_next_number_field && autoinc_has_expl_non_null_value_update_row) {
      table->autoinc_field_has_explicit_non_null_value = true;
      table->next_number_field = table->found_next_number_field;
      if (table->next_number_field->val_int() == 0) {
        thd->force_one_auto_inc_interval(0);
      }
      autoinc_has_expl_non_null_value_update_row = false;
  }

  set_partition(part_id);
  int ret = ha_tse::write_row(record, write_through);// DZW

  if (ret == 0 && m_rec_buf_4_writing != nullptr) {
    assert(m_bulk_insert_parts != nullptr);
    assert(m_rec_buf_4_writing->records() > 0);
    // write_row has already put the record in buffer, so pos is size - 1
    ha_rows pos = m_rec_buf_4_writing->records() - 1;
    m_bulk_insert_parts[pos] = {m_tch.part_id, m_tch.subpart_id};
  }
  reset_partition(part_id);

  table->autoinc_field_has_explicit_non_null_value = saved_autoinc_has_expl_non_null;
  table->next_number_field = saved_next_number_field;
  return ret;
}

void ha_tsepart::start_bulk_insert(ha_rows rows) {
  assert(m_rec_buf_4_writing == nullptr);
  ha_tse::start_bulk_insert(rows);
  if (m_rec_buf_4_writing != nullptr) {
    if (m_bulk_insert_parts == nullptr) {
      // m_max_batch_num is fixed after open table, if table structrue is changed, it should be closed and reopened
      m_bulk_insert_parts = (ctc_part_t *)my_malloc(PSI_NOT_INSTRUMENTED,
                                                    m_max_batch_num * sizeof(ctc_part_t), MYF(MY_WME));
    }
  }
}

int ha_tsepart::bulk_insert_low(dml_flag_t flag, uint *dup_offset) {
  record_info_t record_info = {m_rec_buf_data, (uint16_t)m_cantian_rec_len, nullptr, nullptr};
  return (ct_errno_t)tse_bulk_write(&m_tch, &record_info, m_rec_buf_4_writing->records(),
                                    dup_offset, flag, m_bulk_insert_parts);
}

/**
 Update a row in partition.
 Updates a row given as a parameter to a new value.
 @param[in]	part_id	Partition to update row in.
 @param[in]	old_row	Old row in MySQL format.
 @param[in]	new_row	New row in MySQL format.
 @return error number or 0.
*/
int ha_tsepart::update_row_in_part(uint part_id, const uchar *old_row, uchar *new_row) {
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    m_tch.cursor_addr = cursors[part_id];
  }
  int ret;
  ret = ha_tse::update_row(old_row, new_row);
  reset_partition(part_id);
  return ret;
}

/**
 Write row to new partition.
 @param[in]	new_part_id	New partition to write to.
 @return 0 for success else error code.
*/
int ha_tsepart::write_row_in_new_part(uint new_part_id) {
  UNUSED_PARAM(new_part_id);
  return 0;
}

/**
 Deletes a row in partition.
 @param[in]	part_id	Partition to delete from.
 @param[in]	record	Row to delete in MySQL format.
 @return error number or 0.
*/
int ha_tsepart::delete_row_in_part(uint part_id, const uchar *record) {
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    m_tch.cursor_addr = cursors[part_id];
  }
  int ret;
  ret = ha_tse::delete_row(record);
  reset_partition(part_id);
  return ret;
}

/**
 Initialize random read/scan of a specific partition.
 @param[in]	part_id	Partition to initialize.
 @param[in]	scan	True for scan else random access.
 @return error number or 0.
*/
int ha_tsepart::rnd_init_in_part(uint part_id, bool scan) {
  set_partition(part_id);
  int ret = ha_tse::rnd_init(scan);
  reset_partition(part_id);
  return ret;
}

/** 
 Get next row during scan of a specific partition.
 @param[in]	 part_id Partition to read from.
 @param[out] buf	 Next row.
 @return error number or 0.
*/
int ha_tsepart::rnd_next_in_part(uint part_id, uchar *buf) {
  set_partition(part_id);
  int ret = ha_tse::rnd_next(buf);
  reset_partition(part_id);
  return ret;
}

/**
 End random read/scan of a specific partition.
 @param[in]	part_id		Partition to end random read/scan.
 @param[in]	scan		True for scan else random access.
 @return error number or 0.
*/
int ha_tsepart::rnd_end_in_part(uint part_id, bool scan) {
  UNUSED_PARAM(scan);
  if (m_tch.cursor_addr == INVALID_VALUE64) {
    return 0;
  }
  set_partition(part_id);
  int ret = ha_tse::rnd_end();
  reset_partition(part_id);
  return ret;
}

/**
 Get a reference to the current cursor position in the last used partition.
 @param[out] ref_arg	Reference (PK if exists else row_id).
 @param[in]	 record	    Record to position.
*/
void ha_tsepart::position_in_last_part(uchar *ref_arg, const uchar *record) {
  UNUSED_PARAM(record);
  m_tch.thd_id = ha_thd()->thread_id();
  uint part_id = m_last_part;
  set_partition(part_id);
  int ret = 0;
  if (cur_pos_in_buf == INVALID_MAX_UINT32) {
    ret = tse_position(&m_tch, ref_arg, ref_length - PARTITION_BYTES_IN_POS);
  } else {
    assert(cur_pos_in_buf < max_prefetch_num);
    memcpy(ref_arg, &m_rowids[cur_pos_in_buf], ref_length - PARTITION_BYTES_IN_POS);
  }
  
  if (ret != 0) {
     tse_log_error("find position failed.");
     assert(0);
  }
  reset_partition(part_id);
}

/**
 Get a row from a position.
 Fetches a row from the table based on a row reference.
 @param[out] buf	Returns the row in this buffer, in MySQL format.
 @param[in]	 pos	Position, given as primary key value or DB_ROW_ID
 @return	0, HA_ERR_KEY_NOT_FOUND or error code.
*/
int ha_tsepart::rnd_pos(uchar *buf, uchar *pos) {
  
  // ha_statistic_increment(&SSV::ha_read_rnd_count);
  m_tch.thd_id = ha_thd()->thread_id();
  uint part_id = uint2korr(pos);
  set_partition(part_id);
  int ret = ha_tse::rnd_pos(buf, pos + PARTITION_BYTES_IN_POS);
  reset_partition(part_id);
  return ret;
}

/**
 Initializes a handle to use an index.
 @param[in]	index	Key (index) number.
 @param[in]	sorted	True if result MUST be sorted according to index.
 @return	0 or error number.
*/
int ha_tsepart::index_init(uint index, bool sorted) {
  int ret;
  ret = ph_index_init_setup(index, sorted);
  if (ret != 0) {
    return ret;
  }

  if (sorted) {
    ret = init_record_priority_queue();
    if (ret != 0) {
      /* Needs cleanup in case it returns error. */
      destroy_record_priority_queue();
      return ret;
    }
  }
  
  uint part_id = m_part_info->get_first_used_partition();

  set_partition(part_id);
  ret = ha_tse::index_init(index, sorted);
  reset_partition(part_id);

  pruned_by_engine = false;
  if (sorted && m_rec_buf && m_part_info->num_partitions_used() > 1) {
    delete m_rec_buf;
    m_rec_buf = nullptr;
  }

  return ret;
}

/**
 End index cursor.
 @return	0 or error code.
*/
int ha_tsepart::index_end() {
  pruned_by_engine = false;
  if (m_ordered) {
    destroy_record_priority_queue();
    for (uint i = m_part_info->get_first_used_partition(); i < MY_BIT_NONE; i = m_part_info->get_next_used_partition(i)) {
      if (cursor_is_set(cursor_set, i)){
        m_tch.cursor_addr = cursors[i];
        ha_tse::index_end();
        cursor_clear_bit(cursor_set, i);
      }
    }
    m_tch.part_id = INVALID_PART_ID;
    m_tch.subpart_id = INVALID_PART_ID;
    return 0;
  }
  
  ha_tse::index_end();
  m_tch.part_id = INVALID_PART_ID;
  m_tch.subpart_id = INVALID_PART_ID;
  m_tch.cursor_ref = 0;
  m_tch.cursor_valid = false;
  return 0;
}

int ha_tsepart::index_read(uchar *buf, const uchar *key, uint key_len,
                           ha_rkey_function find_flag) {
  return ha_tse::index_read(buf, key, key_len, find_flag);
}

/**
 Return first record in index from a partition.
 @param[in]	 part_id Partition to read from.
 @param[out] record	 First record in index in the partition.
 @return error number or 0.
*/
int ha_tsepart::index_first_in_part(uint part_id, uchar *record) {
  if (part_id != m_last_part) {
    ha_tse::reset_rec_buf();
  }
  set_partition(part_id);
  int ret = ha_tse::index_first(record);
  reset_partition(part_id);
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    cursors[part_id] = m_tch.cursor_addr;
  }
  return ret;
}

/**
 Return last record in index from a partition.
 @param[in]	 part_id Partition to read from.
 @param[out] record	 Last record in index in the partition.
 @return error number or 0.
*/
int ha_tsepart::index_last_in_part(uint part_id, uchar *record) {
  if (part_id != m_last_part) {
    ha_tse::reset_rec_buf();
  }
  set_partition(part_id);
  int ret = ha_tse::index_last(record);
  reset_partition(part_id);
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    cursors[part_id] = m_tch.cursor_addr;
  }
  return ret;
}

/**
 Return previous record in index from a partition.
 @param[in]	 part_id Partition to read from.
 @param[out] record	 Last record in index in the partition.
 @return error number or 0.
*/
int ha_tsepart::index_prev_in_part(uint part_id, uchar *record) {
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    m_tch.cursor_addr = cursors[part_id];
  }
  set_partition(part_id);
  int ret = ha_tse::index_prev(record);
  reset_partition(part_id);
  return ret;
}

/**
 Return next record in index from a partition.
 @param[in]	 part_id Partition to read from.
 @param[out] record	 Last record in index in the partition.
 @return error number or 0.
*/
int ha_tsepart::index_next_in_part(uint part_id, uchar *record) {
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    m_tch.cursor_addr = cursors[part_id];
  }
  set_partition(part_id);
  int ret = ha_tse::index_next(record);
  reset_partition(part_id);
  return ret;
}

/** 
 Return next same record in index from a partition.
 This routine is used to read the next record, but only if the key is
 the same as supplied in the call.
 @param[in]	 part_id Partition to read from.
 @param[out] record	 Last record in index in the partition.
 @param[in]	 key	 Key to match.
 @param[in]	 length	 Length of key.
 @return error number or 0.
*/
int ha_tsepart::index_next_same_in_part(uint part_id, uchar *record, const uchar *key, uint length) {
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    m_tch.cursor_addr = cursors[part_id];
  }
  set_partition(part_id);
  int ret = ha_tse::index_next_same(record, key, length);
  reset_partition(part_id);
  return ret;
}

/**
 Start index scan and return first record from a partition.
 This routine starts an index scan using a start key. The calling
 function will check the end key on its own.
 @param[in]	 part_id	 Partition to read from.
 @param[out] record	     First matching record in index in the partition.
 @param[in]	 key	     Key to match.
 @param[in]	 keypart_map Which part of the key to use.
 @param[in]	 find_flag	 Key condition/direction to use.
 @return error number or 0.
*/
int ha_tsepart::index_read_map_in_part(uint part_id, uchar *record, const uchar *key,
                            key_part_map keypart_map, enum ha_rkey_function find_flag) {
  if (part_id != m_last_part) {
    ha_tse::reset_rec_buf();
  }
  set_partition(part_id);
  int ret = ha_tse::index_read_map(record, key, keypart_map, find_flag);
  reset_partition(part_id);
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    cursors[part_id] = m_tch.cursor_addr;
  }
  return ret;
}

/**
 Return last matching record in index from a partition.
 @param[in]	 part_id	 Partition to read from.
 @param[out] record	     Last matching record in index in the partition.
 @param[in]	 key	     Key to match.
 @param[in]	 keypart_map Which part of the key to use.
 @return error number or 0.
*/
int ha_tsepart::index_read_last_map_in_part(uint part_id, uchar *record, const uchar *key,
                                key_part_map keypart_map) {
  if (part_id != m_last_part) {
    ha_tse::reset_rec_buf();
  }
  set_partition(part_id);
  int ret = ha_tse::index_read_last_map(record, key, keypart_map);
  reset_partition(part_id);
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    cursors[part_id] = m_tch.cursor_addr;
  }
  return ret;
}

/** 
 Start index scan and return first record from a partition.
 This routine starts an index scan using a start key. The calling
 function will check the end key on its own.
 @param[in]	 part_id     Partition to read from.
 @param[out] record	     First matching record in index in the partition.
 @param[in]	 index	     Index to read from.
 @param[in]	 key	     Key to match.
 @param[in]	 keypart_map Which part of the key to use.
 @param[in]	 find_flag	 Key condition/direction to use.
 @return error number or 0.
*/
int ha_tsepart::index_read_idx_map_in_part(uint part_id, uchar *record, uint index,
                                const uchar *key, key_part_map keypart_map,
                                enum ha_rkey_function find_flag) {
  set_partition(part_id);
  int ret = ha_tse::index_read_idx_map(record, index, key, keypart_map, find_flag);
  reset_partition(part_id);
  return ret;
}

bool ha_tsepart::need_prune_partitions_by_engine(const key_range *start_key, const key_range *end_key) {
  if (start_key == nullptr || end_key == nullptr) {
    return false;
  }

  if (m_part_info->num_partitions_used() != m_part_info->get_tot_partitions()) {
    // pruned by optimizer
    return false;
  }

  if (start_key->flag == HA_READ_KEY_EXACT) {
    return false;
  }

  for (uint i = 0; i < m_part_info->num_columns; i++) {
    if (!m_part_info->part_field_array[i]->part_of_key.is_set(active_index)) {
      // partition field is not used in current index
      return false;
    }
  }

  return true;
}

bool ha_tsepart::equal_range_on_part_field(const key_range *start_key, const key_range *end_key) {
  vector<uint> part_field_ids;
  for (uint i = 0; i <m_part_info->num_columns; i++) {
    part_field_ids.push_back(m_part_info->part_field_array[i]->field_index);
  }

  KEY cur_index = table->key_info[active_index];
  uint offset = 0;

  for (uint i = 0; i < cur_index.user_defined_key_parts; i++) {
    if (offset >= start_key->length) {
      return false;
    }
    KEY_PART_INFO cur_index_part = cur_index.key_part[i];
    auto iter = find(part_field_ids.begin(), part_field_ids.end(), cur_index_part.field->field_index);
    if (iter == part_field_ids.end()) {
      // current key part is not in parted columns
      continue;
    }
    if (memcmp(start_key->key + offset, end_key->key + offset, cur_index_part.length) != 0) {
      return false;
    }
    if (start_key->keypart_map & (1 << i)) {
      offset += cur_index_part.length;
    }
  }
  return true;
}

/** 
 Start index scan and return first record from a partition.
 This routine starts an index scan using a start and end key.
 @param[in]	 part_id   Partition to read from.
 @param[out] record    First matching record in index in the partition.
                       if NULL use table->record[0] as return buffer.
 @param[in]	 start_key Start key to match.
 @param[in]	 end_key   End key to match.
 @param[in]	 sorted	   Return rows in sorted order.
 @return error number or 0.
*/
int ha_tsepart::read_range_first_in_part(uint part_id, uchar *record,
                            const key_range *start_key,
                            const key_range *end_key, bool sorted) {
  UNUSED_PARAM(sorted);

  uchar *read_record = record;
  if (read_record == nullptr) {
    read_record = table->record[0];
  }

  if (pruned_by_engine && part_id != part_spec.start_part) {
    return HA_ERR_END_OF_FILE;
  }

  // prune partitions that doesn't match with condition and take an early return
  if (need_prune_partitions_by_engine(start_key, end_key) && equal_range_on_part_field(start_key, end_key)) {
    pruned_by_engine = true;
    key_range tmp_key;
    memcpy(&tmp_key, start_key, sizeof(key_range));
    tmp_key.flag = HA_READ_KEY_EXACT;
    get_partition_set(table, read_record, active_index, &tmp_key, &part_spec);
    assert(part_spec.start_part == part_spec.end_part);

    if (part_id != part_spec.start_part) {
      return HA_ERR_END_OF_FILE;
    }
  }

  int ret;
  if (part_id != m_last_part) {
    ha_tse::reset_rec_buf();
  }

  set_partition(part_id);
  if (m_start_key.key != nullptr) {
    ret = index_read(read_record, m_start_key.key,
                     m_start_key.length, m_start_key.flag);
  } else {
    ret = ha_tse::index_first(read_record);
  }
  if (ret == HA_ERR_KEY_NOT_FOUND) {
    ret = HA_ERR_END_OF_FILE;
  } else if (ret == 0 && !in_range_check_pushed_down) {
    /* compare_key uses table->record[0], so we
    need to copy the data if not already there. */
 
    if (record != nullptr) {
      memcpy(table->record[0], read_record, m_rec_length);
    }
    if (compare_key(end_range) > 0) {
      ret = HA_ERR_END_OF_FILE;
    }
  }
  reset_partition(part_id);

  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    cursors[part_id] = m_tch.cursor_addr;
  }
  return ret;
}

/**
 Return next record in index range scan from a partition.
 @param[in]	 part_id Partition to read from.
 @param[out] record	 First matching record in index in the partition.
                     if NULL use table->record[0] as return buffer.
 @return error number or 0.
*/
int ha_tsepart::read_range_next_in_part(uint part_id, uchar *record) {
  uchar *read_record = record;
  if (read_record == nullptr) {
    read_record = table->record[0];
  }
  if (m_index_sorted) {
    cursor_set_bit(cursor_set, part_id);
    m_tch.cursor_addr = cursors[part_id];
  }
  set_partition(part_id);
  int ret = ha_tse::index_next(read_record);
  reset_partition(part_id);
  return ret;
}

/**
 Get partition row type
 @param[in] partition_table Partition table
 @param[in] part_id         Id of partition for which row type to be retrieved
 @return Partition row type.
*/
enum row_type ha_tsepart::get_partition_row_type(const dd::Table *partition_table, uint part_id) {
  UNUSED_PARAM(partition_table);
  UNUSED_PARAM(part_id);
  return ROW_TYPE_NOT_USED;
}

void ha_tsepart::info_low() {
  stats.records = 0;
  if (m_part_share->cbo_stats != nullptr) {
    uint part_num = m_is_sub_partitioned ? table->part_info->num_parts * table->part_info->num_subparts :
                                            table->part_info->num_parts;
    for (uint part_id = m_part_info->get_first_used_partition(); part_id < part_num;
        part_id = m_part_info->get_next_used_partition(part_id)) {
          stats.records += m_part_share->cbo_stats->tse_cbo_stats_table[part_id].estimate_rows;
    }
  }
}

int ha_tsepart::info(uint flag) {
  return ha_tse::info(flag);
}

ha_rows ha_tsepart::records_in_range(uint inx, key_range *min_key, key_range *max_key) {

  double density;
  if (m_part_share && !m_part_share->cbo_stats->is_updated) {
    tse_log_debug("table %s has not been analyzed", table->alias);
    return 1;
  }


  tse_key tse_min_key;
  tse_key tse_max_key;
  set_tse_range_key(&tse_min_key, min_key, true);
  set_tse_range_key(&tse_max_key, max_key, false);
  tse_range_key key = {&tse_min_key, &tse_max_key};
  if (tse_max_key.len < tse_min_key.len) {
    tse_max_key.cmp_type = CMP_TYPE_NULL;
  } else if (tse_max_key.len > tse_min_key.len) {
    tse_min_key.cmp_type = CMP_TYPE_NULL;
  }

  uint64_t n_rows_num = 0;
  uint part_num = m_is_sub_partitioned ? table->part_info->num_parts * table->part_info->num_subparts :
                                                table->part_info->num_parts;

for (uint part_id = m_part_info->get_first_used_partition(); part_id < part_num;
        part_id = m_part_info->get_next_used_partition(part_id)) {

    set_tse_range_key(&tse_min_key, min_key, true);
    set_tse_range_key(&tse_max_key, max_key, false);
    if (tse_max_key.len < tse_min_key.len) {
      tse_max_key.cmp_type = CMP_TYPE_NULL;
    } else if (tse_max_key.len > tse_min_key.len) {
      tse_min_key.cmp_type = CMP_TYPE_NULL;
    }
    density = calc_density_one_table(inx, &key, &m_part_share->cbo_stats->tse_cbo_stats_table[part_id], *table);
    n_rows_num += m_part_share->cbo_stats->tse_cbo_stats_table[part_id].estimate_rows * density;
  }

  /*
   * The MySQL optimizer seems to believe an estimate of 0 rows is
   * always accurate and may return the result 'Empty set' based on that
   */
  if (n_rows_num == 0) {
    n_rows_num = 1;
  }
  return (ha_rows)n_rows_num;
}

int ha_tsepart::records(ha_rows *num_rows) /*!< out: number of rows */
{
  *num_rows = 0;
  int ret;
  for (uint part_id = m_part_info->get_first_used_partition(); part_id < m_tot_parts;
       part_id = m_part_info->get_next_used_partition(part_id)) {
    set_partition(part_id);
    ha_rows n_rows{};
    ret = ha_tse::records(&n_rows);
    reset_partition(part_id);
    if (ret != 0) {
      *num_rows = HA_POS_ERROR;
      return ret;
    }
    *num_rows += n_rows;
  }
  return 0;
}

extern uint32_t ctc_instance_id;
/* alter table truncate partition */
int ha_tsepart::truncate_partition_low(dd::Table *dd_table) {
  THD *thd = ha_thd();
  if (engine_ddl_passthru(thd)) {
    return 0;  
  }

  tse_ddl_stack_mem stack_mem(0);
  update_member_tch(m_tch, ht, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, m_tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ct_errno_t ret = (ct_errno_t)fill_truncate_partition_req(
      table->s->normalized_path.str, m_part_info, dd_table, thd, &ddl_ctrl,
      &stack_mem);
  if (ret != 0) {
    return convert_tse_error_code_to_mysql(ret);
  }
  void *tse_ddl_req_msg_mem = stack_mem.get_buf();
  if (tse_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  ret = (ct_errno_t)tse_truncate_partition(tse_ddl_req_msg_mem, &ddl_ctrl);
  tse_ddl_hook_cantian_error("tse_truncate_partition_cantian_error", thd, &ddl_ctrl, &ret);
  m_tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(m_tch, ht, thd);
  return tse_ddl_handle_fault("tse_truncate_partition", thd, &ddl_ctrl, ret);
}

int ha_tsepart::analyze(THD *thd, HA_CHECK_OPT *opt) {
  if (engine_ddl_passthru(thd)) {
    m_part_share->need_fetch_cbo = true;
    return 0;
  }
  Alter_info *const alter_info = &(get_thd()->lex->alter_info);
  if ((alter_info->flags & Alter_info::ALTER_ADMIN_PARTITION) == 0 ||
      (alter_info->flags & Alter_info::ALTER_ALL_PARTITION)) {
    m_tch.part_id = INVALID_PART_ID;
    m_part_share->need_fetch_cbo = true;
    return ha_tse::analyze(thd, opt);
  }
  int ret;
  uint32_t used_parts;
  uint32_t *part_ids = nullptr;
  uint32_t *subpart_ids = nullptr;
  bool memory_allocated = false;
  if (!m_part_info->set_read_partitions(&alter_info->partition_names)) {
    memory_allocated = get_used_partitions(m_part_info, &part_ids, &subpart_ids, &used_parts);
    if(!memory_allocated){
      tse_log_error("Failed to allocate memory !");
      return HA_ERR_OUT_OF_MEM;
    }
  } else {
    tse_log_error("no partition alter !");
    return HA_ERR_GENERIC;
  }
  if (m_part_info->num_parts == used_parts) {
    m_tch.part_id = INVALID_PART_ID;
    my_free(part_ids);
    my_free(subpart_ids);
    m_part_share->need_fetch_cbo = true;
    return ha_tse::analyze(thd, opt);
  }
  for (uint i = 0; i < used_parts; i++) {
    uint32_t part_id = part_ids[i];
    uint32_t subpart_id = subpart_ids[i];
    m_tch.part_id = part_id;
    m_tch.subpart_id = subpart_id;
    m_part_share->need_fetch_cbo = true;
    ret = ha_tse::analyze(thd, opt);
    if (ret != 0) {
      tse_log_error("analyze partition error!");
      my_free(subpart_ids);
      my_free(part_ids);
      return ret;
    }
  }
  my_free(subpart_ids);
  my_free(part_ids);
  return ret;
}

int ha_tsepart::optimize(THD *thd, HA_CHECK_OPT *opt) {
    int ret;
    m_tch.part_id = INVALID_PART_ID;
    ret = ha_tse::analyze(thd, opt);
    if (ret != 0) {
      tse_log_error("analyze partition error!");
      return ret;
    }
    m_part_share->need_fetch_cbo = true;
    ret = ha_tse::optimize(thd, opt);
    if (ret != 0) {
      tse_log_error("optimize partition error!");
      return ret;
    }
    return (HA_ADMIN_TRY_ALTER);
}

int ha_tsepart::initialize_cbo_stats() {
  if (m_part_share->cbo_stats != nullptr) {
    return CT_SUCCESS;
  }
  uint32_t part_num = m_is_sub_partitioned ? table->part_info->num_parts * table->part_info->num_subparts : 
                      table->part_info->num_parts;
    
  m_part_share->cbo_stats = (tianchi_cbo_stats_t*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(tianchi_cbo_stats_t), MYF(MY_WME));
  if (m_part_share->cbo_stats == nullptr) {
    tse_log_error("alloc mem failed, m_part_share->cbo_stats size(%lu)", sizeof(tianchi_cbo_stats_t));
    return ERR_ALLOC_MEMORY;
  }
  *m_part_share->cbo_stats = {0, 0, 0, 0, 0, nullptr, 0, nullptr, nullptr};

  m_part_share->cbo_stats->part_cnt = part_num;

  m_part_share->cbo_stats->tse_cbo_stats_table = 
      (tse_cbo_stats_table_t*)my_malloc(PSI_NOT_INSTRUMENTED, part_num * sizeof(tse_cbo_stats_table_t), MYF(MY_WME));
  if (m_part_share->cbo_stats->tse_cbo_stats_table == nullptr) {
    tse_log_error("alloc mem failed, m_part_share->cbo_stats->tse_cbo_stats_table size(%lu)", part_num * sizeof(tse_cbo_stats_table_t));
    return ERR_ALLOC_MEMORY;
  }
  m_part_share->cbo_stats->ndv_keys =
      (uint32_t*)my_malloc(PSI_NOT_INSTRUMENTED, table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS, MYF(MY_WME));
  if (m_part_share->cbo_stats->ndv_keys == nullptr) {
    tse_log_error("alloc mem failed, m_part_share->cbo_stats->ndv_keys size(%lu)", table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS);
    return ERR_ALLOC_MEMORY;
  }
  for (uint i = 0; i < part_num; i++) {
    m_part_share->cbo_stats->tse_cbo_stats_table[i].estimate_rows = 0;
    m_part_share->cbo_stats->tse_cbo_stats_table[i].columns =
      (tse_cbo_stats_column_t*)my_malloc(PSI_NOT_INSTRUMENTED, table->s->fields * sizeof(tse_cbo_stats_column_t), MYF(MY_WME));
    if (m_part_share->cbo_stats->tse_cbo_stats_table[i].columns == nullptr) {
    tse_log_error("alloc mem failed, m_part_share->cbo_stats->tse_cbo_stats_table size(%lu)", table->s->fields * sizeof(tse_cbo_stats_column_t));
    return ERR_ALLOC_MEMORY;
    }
    for (uint col_id = 0; col_id < table->s->fields; col_id++) {
      m_part_share->cbo_stats->tse_cbo_stats_table[i].columns[col_id].hist_count = 0;
    }
  }
  
  ct_errno_t ret = (ct_errno_t)alloc_str_mysql_mem(m_part_share->cbo_stats, part_num, table);
  if (ret != CT_SUCCESS) {
    tse_log_error("m_part_share:tse alloc str mysql mem failed, ret:%d", ret);
  }
  
  m_part_share->cbo_stats->msg_len = table->s->fields * sizeof(tse_cbo_stats_column_t);
  m_part_share->cbo_stats->key_len = table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS;

  return CT_SUCCESS;
}

int ha_tsepart::get_cbo_stats_4share()
{
  THD *thd = ha_thd();
  int ret = CT_SUCCESS;
  time_t now = time(nullptr);
  if (m_part_share->need_fetch_cbo || now - m_part_share->get_cbo_time > ctc_update_analyze_time) {
    if (m_tch.ctx_addr == INVALID_VALUE64) {
      char user_name[SMALL_RECORD_SIZE] = { 0 };
      tse_split_normalized_name(table->s->normalized_path.str, user_name, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
      tse_copy_name(user_name, user_name, SMALL_RECORD_SIZE);
      update_member_tch(m_tch, get_tse_hton(), thd);
      ret = tse_open_table(&m_tch, table->s->table_name.str, user_name);
      update_sess_ctx_by_tch(m_tch, get_tse_hton(), thd);
      if (ret != CT_SUCCESS) {
        return ret;
      }
    }
    uint32_t str_data_size = m_part_share->cbo_stats->num_str_cols * (STATS_HISTGRAM_MAX_SIZE + 2) * CBO_STRING_MAX_LEN;
    uint32_t data_size = m_part_share->cbo_stats->msg_len > str_data_size ?
                         m_part_share->cbo_stats->msg_len : str_data_size;
    uint32_t part_cnt = m_part_share->cbo_stats->part_cnt;
    uint32_t num_part_fetch = MAX_MESSAGE_SIZE / data_size;
    uint32_t fetch_times = part_cnt / num_part_fetch;
    uint32_t first_partid = 0;

    for (uint32_t i = 0; i < fetch_times; i++) {
      update_member_tch(m_tch, get_tse_hton(), thd);
      ret = tse_get_cbo_stats(&m_tch, m_part_share->cbo_stats, &m_part_share->cbo_stats->tse_cbo_stats_table[first_partid], first_partid, num_part_fetch);
      update_sess_ctx_by_tch(m_tch, get_tse_hton(), thd);
      if (ret != CT_SUCCESS) {
        return ret;
      }
      first_partid += num_part_fetch;
    }

    num_part_fetch = part_cnt - first_partid;
    if (num_part_fetch > 0) {
      update_member_tch(m_tch, get_tse_hton(), thd);
      ret = tse_get_cbo_stats(&m_tch, m_part_share->cbo_stats, &m_part_share->cbo_stats->tse_cbo_stats_table[first_partid], first_partid, num_part_fetch);
      update_sess_ctx_by_tch(m_tch, get_tse_hton(), thd);
    }
    
    if (ret == CT_SUCCESS && m_part_share->cbo_stats->is_updated) {
      m_part_share->need_fetch_cbo = false;
      tse_index_stats_update(table, m_part_share->cbo_stats);
    }
    m_part_share->get_cbo_time = now;
  }

  return ret;
}
 
void ha_tsepart::free_cbo_stats() {
  if (m_part_share->cbo_stats == nullptr) {
      return;
  }
  uint32_t part_num = m_is_sub_partitioned ? table->part_info->num_parts * table->part_info->num_subparts : 
                      table->part_info->num_parts;

  bool is_str_first_addr = true;
  for (uint i = 0; i < part_num; i++) {
    free_columns_cbo_stats(m_part_share->cbo_stats->tse_cbo_stats_table[i].columns, &is_str_first_addr, table);
  }

  my_free((m_part_share->cbo_stats->ndv_keys));
  m_part_share->cbo_stats->ndv_keys = nullptr;
  my_free((m_part_share->cbo_stats->col_type));
  m_part_share->cbo_stats->col_type = nullptr;
  my_free(m_part_share->cbo_stats->tse_cbo_stats_table);
  m_part_share->cbo_stats->tse_cbo_stats_table = nullptr;
  my_free(m_part_share->cbo_stats);
  m_part_share->cbo_stats = nullptr;
}

int ha_tsepart::check(THD *, HA_CHECK_OPT *)
{
  return HA_ADMIN_OK;
}

int ha_tsepart::repair(THD *thd, HA_CHECK_OPT *)
{
  if (engine_ddl_passthru(thd)) {
    return HA_ADMIN_OK;
  }
  tianchi_handler_t tch;
  TSE_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(get_tse_hton(), thd, tch));
  tse_ddl_broadcast_request broadcast_req {{0}, {0}, {0}, {0}, 0, 0, 0, 0, {0}};
  string sql = string(thd->query().str).substr(0, thd->query().length);
  FILL_BROADCAST_BASE_REQ(broadcast_req, sql.c_str(), thd->main_security_ctx.priv_user,
    thd->m_main_security_ctx.priv_host.str, ctc_instance_id, thd->lex->sql_command);
  if (thd->db.str != NULL && thd->db.length > 0) {
    strncpy(broadcast_req.db_name, thd->db.str, SMALL_RECORD_SIZE - 1);
  }
  broadcast_req.options |= TSE_NOT_NEED_CANTIAN_EXECUTE;

  ct_errno_t ret = CT_SUCCESS;
  if (IS_METADATA_NORMALIZATION()) {
    ret = (ct_errno_t)ctc_record_sql_for_cantian(&tch, &broadcast_req, false);
    assert (ret == CT_SUCCESS);
  } else {
    ret = (ct_errno_t)tse_execute_mysql_ddl_sql(&tch, &broadcast_req, false);
    assert (ret == CT_SUCCESS);
  }

  return (int)ret;
}

uint32 ha_tsepart::calculate_key_hash_value(Field **field_array)
{
  return (Partition_helper::ph_calculate_key_hash_value(field_array));
}

bool ha_tsepart::check_unsupported_indexdir(dd::Table *table_def) {
  for (const auto dd_part : *table_def->leaf_partitions()) {
    dd::String_type index_file_name;
    if (dd_part == nullptr){
      continue;
    }
    const dd::Properties &options = dd_part->options();
    if (options.exists(index_file_name_val_key)) {
      options.get(index_file_name_val_key, &index_file_name);
    }
    if (!index_file_name.empty()) {
      my_error(ER_ILLEGAL_HA, MYF(0), table_share != nullptr ? table_share->table_name.str : " ");
      return true;
    }  
  }
  return false;
}

EXTER_ATTACK int ha_tsepart::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
                   dd::Table *table_def) {
  THD *thd = ha_thd();                  
  if (table_def != nullptr && check_unsupported_indexdir(table_def)) {
    tse_log_system("Unsupported operation. sql = %s", thd->query().str);
    return HA_ERR_WRONG_COMMAND;
  }                 
  return ha_tse::create(name, form, create_info, table_def);                  
}


#endif // NEED_TSE_PART_HANDLER
