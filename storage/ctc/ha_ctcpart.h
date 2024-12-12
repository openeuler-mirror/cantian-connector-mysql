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

/* class for the the ctc part table handler */

#ifndef __HA_CTCPART_H__
#define __HA_CTCPART_H__

#include "ha_ctc.h"
#include "ctc_srv.h"
#include "ctc_util.h"
#include "sql/partitioning/partition_handler.h"
#include "sql/dd/string_type.h"

class Ctcpart_share : public Partition_share {
 public:
  //std::vector<u_int8_t *> table_records;
  ctc_cbo_stats_t *cbo_stats = nullptr;
  int used_count = 0;
  bool need_fetch_cbo = false;
  time_t get_cbo_time = 0;
};


/** @brief
  Class definition for the storage engine
*/
class ha_ctcpart : public ha_ctc,
                   public Partition_helper,
                   public Partition_handler {
 public:
  ha_ctcpart(handlerton *hton, TABLE_SHARE *table_arg);

  handler *get_handler() override { return (static_cast<handler *>(this)); }

  /** Set active partition.
  @param[in]	part_id	Partition to set as active. */
  void set_partition(uint part_id);

  /** Reset active partition and last partition.
  @param[in]	part_id	Partition to reset after use. */
  void reset_partition(uint part_id);

  /**
    Truncate partition.

    Low-level primitive for handler, implementing
    Partition_handler::truncate_partition().

    @sa Partition_handler::truncate_partition().
  */
  int truncate_partition_low(dd::Table *dd_table) override;

  bool check_unsupported_indexdir(dd::Table *table_def);

  /** Write a row in specific partition.
  Stores a row in an CTC database, to the table specified in this
  handle.
  @param[in]	part_id	Partition to write to.
  @param[in]	record	A row in MySQL format.
  @return error code. */
  int write_row_in_part(uint part_id, uchar *record) override;

  /* functions used for notifying SE to start/terminate batch insertion */
  void start_bulk_insert(ha_rows rows) override;

  /** write row to new partition.
  @param[in]	new_part	New partition to write to.
  @return 0 for success else error code. */
  int write_row_in_new_part(uint new_part) override;

  /** Update a row in partition.
  Updates a row given as a parameter to a new value.
  @param[in]	part_id	Partition to update row in.
  @param[in]	old_row	Old row in MySQL format.
  @param[in]	new_row	New row in MySQL format.
  @return error number or 0. */
  int update_row_in_part(uint part_id, const uchar *old_row,
                         uchar *new_row) override;

  /** Deletes a row in partition.
  @param[in]	part_id	Partition to delete from.
  @param[in]	record	Row to delete in MySQL format.
  @return error number or 0. */
  int delete_row_in_part(uint part_id, const uchar *record) override;

  /** Return first record in index from a partition.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	First record in index in the partition.
  @return error number or 0. */
  int index_first_in_part(uint part_id, uchar *record) override;

  /** Return last record in index from a partition.
  @param[in]	part_idart	Partition to read from.
  @param[out]	record	Last record in index in the partition.
  @return error number or 0. */
  int index_last_in_part(uint part_id, uchar *record) override;

  /** Return previous record in index from a partition.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	Last record in index in the partition.
  @return error number or 0. */
  int index_prev_in_part(uint part_id, uchar *record) override;

  /** Return next record in index from a partition.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	Last record in index in the partition.
  @return error number or 0. */
  int index_next_in_part(uint part_id, uchar *record) override;

  /** Initialize random read/scan of a specific partition.
  @param[in]	part_id		Partition to initialize.
  @param[in]	scan		True for scan else random access.
  @return error number or 0. */
  int rnd_init_in_part(uint part_id, bool scan) override;

  /** Get next row during scan of a specific partition.
  @param[in]	part_id	Partition to read from.
  @param[out]	buf	Next row.
  @return error number or 0. */
  int rnd_next_in_part(uint part_id, uchar *buf) override;

  /** End random read/scan of a specific partition.
  @param[in]	part_id		Partition to end random read/scan.
  @param[in]	scan		True for scan else random access.
  @return error number or 0. */
  int rnd_end_in_part(uint part_id, bool scan) override;

  /** Get a reference to the current cursor position in the last used
  partition.
  @param[out]	ref_arg	Reference (PK if exists else row_id).
  @param[in]	record	Record to position. */
  void position_in_last_part(uchar *ref_arg, const uchar *record) override;

  /** Return next same record in index from a partition.
  This routine is used to read the next record, but only if the key is
  the same as supplied in the call.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	Last record in index in the partition.
  @param[in]	key	Key to match.
  @param[in]	length	Length of key.
  @return error number or 0. */
  int index_next_same_in_part(uint part_id, uchar *record, const uchar *key,
                              uint length) override;

  /** Start index scan and return first record from a partition.
  This routine starts an index scan using a start key. The calling
  function will check the end key on its own.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	First matching record in index in the partition.
  @param[in]	key	Key to match.
  @param[in]	keypart_map	Which part of the key to use.
  @param[in]	find_flag	Key condition/direction to use.
  @return error number or 0. */
  int index_read_map_in_part(uint part_id, uchar *record, const uchar *key,
                             key_part_map keypart_map,
                             enum ha_rkey_function find_flag) override;

  /** Return last matching record in index from a partition.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	Last matching record in index in the partition.
  @param[in]	key	Key to match.
  @param[in]	keypart_map	Which part of the key to use.
  @return error number or 0. */
  int index_read_last_map_in_part(uint papart_id, uchar *record, const uchar *key,
                                  key_part_map keypart_map) override;

  /** Start index scan and return first record from a partition.
  This routine starts an index scan using a start and end key.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	First matching record in index in the partition.
  if NULL use table->record[0] as return buffer.
  @param[in]	start_key	Start key to match.
  @param[in]	end_key	End key to match.
  @param[in]	sorted	Return rows in sorted order.
  @return error number or 0. */
  int read_range_first_in_part(uint part_id, uchar *record,
                               const key_range *start_key,
                               const key_range *end_key, bool sorted) override;

  /** Return next record in index range scan from a partition.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	First matching record in index in the partition.
  if NULL use table->record[0] as return buffer.
  @return error number or 0. */
  int read_range_next_in_part(uint part_id, uchar *record) override;

  /** Start index scan and return first record from a partition.
  This routine starts an index scan using a start key. The calling
  function will check the end key on its own.
  @param[in]	part_id	Partition to read from.
  @param[out]	record	First matching record in index in the partition.
  @param[in]	index	Index to read from.
  @param[in]	key	Key to match.
  @param[in]	keypart_map	Which part of the key to use.
  @param[in]	find_flag	Key condition/direction to use.
  @return error number or 0. */
  int index_read_idx_map_in_part(uint part_id, uchar *record, uint index,
                                 const uchar *key, key_part_map keypart_map,
                                 enum ha_rkey_function find_flag) override;

  static int key_and_rowid_cmp(KEY **key_info, uchar *a, uchar *b);

  int extra(enum ha_extra_function operation) override;

  int cmp_ref(const uchar *ref1, const uchar *ref2) const override;

  /** Open an CTC table.
  @param[in]	name		table name
  @param[in]	mode		access mode
  @param[in]	test_if_locked	test if the file to be opened is locked
  @param[in]	table_def	dd::Table describing table to be opened
  @retval 1 if error
  @retval 0 if success */
  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;

  int close() override;

  int check(THD *thd, HA_CHECK_OPT *check_opt) override;

  int repair(THD *thd, HA_CHECK_OPT *check_opt) override;

  int rnd_init(bool scan) override {
    return (Partition_helper::ph_rnd_init(scan));
  }
  
  int rnd_next(uchar *record) override {
    return (Partition_helper::ph_rnd_next(record));
  }

  int rnd_end() override {
    return (Partition_helper::ph_rnd_end());
  }

  void position(const uchar *record) override {
    Partition_helper::ph_position(record);
  }

  int rnd_pos(uchar *record, uchar *pos) override;

  void part_autoinc_has_expl_non_null_value_update_row(uchar *new_data);
#ifdef METADATA_NORMALIZED
  int write_row(uchar *record, bool write_through MY_ATTRIBUTE((unused)) = false) override {
#else
  int write_row(uchar *record) override {
#endif
    if (table->next_number_field) {
      autoinc_has_expl_non_null_value = true;
    }
    return Partition_helper::ph_write_row(record);
  }

  int update_row(const uchar *old_record, uchar *new_record) override {
    part_autoinc_has_expl_non_null_value_update_row(new_record);
    return (Partition_helper::ph_update_row(old_record, new_record));
  }

  int delete_row(const uchar *record) override {
    return (Partition_helper::ph_delete_row(record));
  }

  int delete_all_rows() override { return (handler::delete_all_rows()); }

  int index_init(uint index, bool sorted) override;

  int index_end() override;

  int index_read(uchar *buf, const uchar *key, uint key_len,
                 ha_rkey_function find_flag) override;

  int index_next(uchar *record) override {
    return (Partition_helper::ph_index_next(record));
  }

  int index_next_same(uchar *record, const uchar *, uint keylen) override {
    return (Partition_helper::ph_index_next_same(record, keylen));
  }

  int index_prev(uchar *record) override {
    return (Partition_helper::ph_index_prev(record));
  }

  int index_first(uchar *record) override {
    return (Partition_helper::ph_index_first(record));
  }

  int index_last(uchar *record) override {
    return (Partition_helper::ph_index_last(record));
  }

  int index_read_last_map(uchar *record, const uchar *key,
                          key_part_map keypart_map) override {
    return (Partition_helper::ph_index_read_last_map(record, key, keypart_map));
  }

  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override {
    return (Partition_helper::ph_index_read_map(buf, key, keypart_map, find_flag));
  }

  int index_read_idx_map(uchar *buf, uint index, const uchar *key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override {
    return (Partition_helper::ph_index_read_idx_map(buf, index, key, keypart_map, find_flag));
  }

  /** @brief
    Read first row between two ranges.
    @retval
      0			Found row
    @retval
      HA_ERR_END_OF_FILE	No rows in range
  */
  int read_range_first(const key_range *start_key, const key_range *end_key,
                       bool eq_range_arg, bool sorted) override {
    int result = Partition_helper::ph_read_range_first(start_key, end_key, eq_range_arg, sorted);
    return (result == HA_ERR_KEY_NOT_FOUND) ? HA_ERR_END_OF_FILE : result;
  }

  int read_range_next() override {
    return (Partition_helper::ph_read_range_next());
  }

  ulonglong table_flags() const override {
    return (ha_ctc::table_flags() | HA_CAN_REPAIR);
  }

  THD *get_thd() const override { return ha_thd(); }

  TABLE *get_table() const override { return table; }

  bool get_eq_range() const override { return eq_range; }

  void set_eq_range(bool eq_range_arg) override { eq_range = eq_range_arg; }

  void set_range_key_part(KEY_PART_INFO *key_part) override {
    range_key_part = key_part;
  }

  int initialize_auto_increment(bool no_lock MY_ATTRIBUTE((unused))) override { return 0; }

  void get_dynamic_partition_info(ha_statistics *stat_info,
                                  ha_checksum *check_sum,
                                  uint part_id) override {
    Partition_helper::get_dynamic_partition_info_low(stat_info, check_sum,
                                                     part_id);
  }

  void set_part_info(partition_info *part_info, bool early) override {
    Partition_helper::set_part_info_low(part_info, early);
  }

  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values, ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  
  void print_error(int error, myf errflag) override;

  /* function used for notifying SE to collect cbo stats */
  int analyze(THD *thd, HA_CHECK_OPT *check_opt) override;

  int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;

  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;

  /* Get partition row type
  @param[in] partition_table partition table
  @param[in] part_id Id of partition for which row type to be retrieved
  @return Partition row type. */
  enum row_type get_partition_row_type(const dd::Table *partition_table,
                                       uint part_id) override;
  
  Partition_handler *get_partition_handler() override {
    return (static_cast<Partition_handler *>(this));
  }

  uint alter_flags(uint flags MY_ATTRIBUTE((unused))) const override {
    return (HA_PARTITION_FUNCTION_SUPPORTED | HA_INPLACE_CHANGE_PARTITION);
  }

  bool need_prune_partitions_by_engine(const key_range *start_key, const key_range *end_key);

  bool equal_range_on_part_field(const key_range *start_key, const key_range *end_key);

  void info_low() override;

  int info(uint) override;

  ha_rows records_in_range(uint inx, key_range *min_key, key_range *max_key) override;

  int records(ha_rows *num_rows) override;

  int initialize_cbo_stats() override;

  void free_cbo_stats() override;

  int get_cbo_stats_4share() override;

  uint32 calculate_key_hash_value(Field **field_array) override;

 protected:
  bool pruned_by_engine;

  part_id_range part_spec;

  uint64 cursors[PART_CURSOR_NUM];

  //BITMAP cursor_set;
  int cursor_set[PART_CURSOR_NUM / 8] = {0};

  // store partitions for bulk insert
  ctc_part_t *m_bulk_insert_parts;

  /** Pointer to Ctcpart_share on the TABLE_SHARE. */
  Ctcpart_share *m_part_share;
  bool autoinc_has_expl_non_null_value = false;
  bool autoinc_has_expl_non_null_value_update_row = false;

 private:
  int bulk_insert_low(dml_flag_t flag, uint *dup_offset) override;
};

static inline void cursor_set_bit(int *cursor_set, uint bit) {
  (cursor_set)[bit / 8] |= (1 << (bit & 7));
}

static inline bool cursor_is_set(int *cursor_set, uint bit) {
  return (cursor_set)[bit / 8] & (1 << (bit & 7));
}

static inline void cursor_clear_bit(int *cursor_set, uint bit) {
  (cursor_set)[bit / 8] &= ~(1 << (bit & 7));
}

bool get_used_partitions(partition_info *part_info,
                         uint32_t **part_ids, uint32_t **subpart_ids, uint32_t *used_parts);
#endif /* ha_ctcpart_h */