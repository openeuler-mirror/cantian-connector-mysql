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

#ifndef SRV_MQ_MSG__
#define SRV_MQ_MSG__

#include "ctc_srv.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cpluscplus */

#define SHM_SEG_MAX_NUM 64
#define CTC_MAX_COLUMNS 4096  // CT_MAX_COLUMNS
#define INDEX_KEY_SIZE 4096  // 索引查询条件的大小mysql限制为3072，取4096
#define MAX_PREFETCH_REC_NUM 100
#define REQUEST_SIZE (MAX_RECORD_SIZE + (2 * MAX_PREFETCH_REC_NUM) + 24)  // 根据rnd_prefetch_request计算, 取8字节对齐
#define CTC_MQ_MESSAGE_SLICE_LEN 102400

#define MAX_LOB_LOCATOR_SIZE 4000  // 存储引擎存储blob对象结构体最大长度

#define REG_MISMATCH_CTC_VERSION   501
#define REG_ALLOC_INST_ID_FAILED   502

// duplicate req->worker_count and req->index_paral_range->workers, intent to do so to align with knl api.
struct get_index_paral_schedule_request {
    ctc_handler_t tch;
    int worker_count; // in and out param, expected DOP (degree of parallel), out as actually scheduled worker count.
    char index_name[CTC_MAX_KEY_NAME_LENGTH + 1]; // in param, indicating the slot id used to fetch index param
    ctc_scan_range_t *scan_range; // in param
    ctc_index_paral_range_t *index_paral_range; // out param
    uint64_t query_scn; // query_scn is of scn type, out param for first request, drop for the rest
    bool reverse;
    bool is_index_full;
    int result;
};

// duplicate req->worker_count and req->paral_range->workers, intent to do so to align with knl api.
struct get_paral_schedule_request {
    ctc_handler_t tch;
    int worker_count; // in and out pararm
    ctc_index_paral_range_t *paral_range; // out param, point to a range on shared memory allocated with ctc_malloc
    uint64_t query_scn; // query_scn is of scn type, out param for first request, drop for the rest
    uint64_t ssn;
    int result;
};

struct register_instance_request {
    uint32_t ctc_version; // ctc支持多版本的接口，格式为1.1.3=1001003，00作为点标记
    int group_num;
    int cpu_info[SHM_SEG_MAX_NUM][SMALL_RECORD_SIZE];
    int result;
};

struct close_session_request {
    ctc_handler_t tch;
    int result;
};

struct open_table_request {
    char table_name[SMALL_RECORD_SIZE];
    char user_name[SMALL_RECORD_SIZE];
    ctc_handler_t tch;
    int result;
};

struct close_table_request {
    ctc_handler_t tch;
    int result;
};

struct write_row_request {
    uint16_t record_len;
    uint8_t *record;
    int result;
    ctc_handler_t tch;
    uint16_t serial_column_offset;
    uint64_t last_insert_id;
    dml_flag_t flag;
};

struct update_job_request {
    int result;
    update_job_info info;
};

struct bulk_write_request {
    int result;
    ctc_handler_t tch;
    uint16_t record_len;
    uint64_t record_num;
    uint32_t err_pos;
    uint8_t record[MAX_RECORD_SIZE];
    dml_flag_t flag;
    ctc_part_t part_ids[MAX_BULK_INSERT_PART_ROWS];
};

struct update_row_request {
    ctc_handler_t tch;
    uint16_t new_record_len;
    uint8_t *new_record;
    uint16_t upd_cols[CTC_MAX_COLUMNS];
    uint16_t col_num;
    int result;
    dml_flag_t flag;
};

struct delete_row_request {
    ctc_handler_t tch;
    uint16_t record_len;
    int result;
    dml_flag_t flag;
};

struct rnd_init_request {
    ctc_handler_t tch;
    int result;
    expected_cursor_action_t action;
    ctc_select_mode_t mode;
    ctc_conds *cond;
};

struct rnd_end_request {
    ctc_handler_t tch;
    int result;
};

struct scan_records_request {
    ctc_handler_t tch;
    uint64_t num_rows;
    char index_name[CTC_MAX_KEY_NAME_LENGTH];  // 索引名
    int result;
};

struct rnd_next_request {
    ctc_handler_t tch;
    uint16_t record_len;
    uint8_t *record;
    int result;
};

struct rnd_prefetch_request {
    ctc_handler_t tch;
    int result;
    int max_row_size;
    uint8_t records[MAX_RECORD_SIZE];
    uint16_t record_lens[MAX_PREFETCH_REC_NUM];
    uint32_t recNum[1];
    uint64_t rowids[MAX_PREFETCH_REC_NUM];
};

struct trx_begin_request {
    ctc_handler_t tch;
    int result;
    ctc_trx_context_t trx_context;
    bool is_mysql_local;
    struct timeval begin_time;
    bool enable_stat;
};

struct trx_commit_request {
    ctc_handler_t tch;
    int result;
    bool is_ddl_commit;
    int32_t csize;
    uint64_t *cursors;
    char sql[MAX_WSR_DML_SQL_LEN];
    bool enable_stat;
};

struct trx_rollback_request {
    ctc_handler_t tch;
    int result;
    int32_t csize;
    uint64_t *cursors;
};

struct lock_table_request {
    char db_name[SMALL_RECORD_SIZE];
    ctc_lock_table_info lock_info;
    ctc_handler_t tch;
    int result;
    uint32_t mysql_inst_id;
    int error_code;
    char error_message[ERROR_MESSAGE_LEN];
};


struct pre_create_db_request {
    ctc_handler_t tch;
    char sql_str[MAX_DDL_SQL_LEN];
    char db_name[SMALL_RECORD_SIZE];
    uint32_t ctc_db_datafile_size;
    bool ctc_db_datafile_autoextend;
    uint32_t ctc_db_datafile_extend_size;
    int error_code;
    char error_message[ERROR_MESSAGE_LEN];
    int result;
};

struct drop_tablespace_and_user_request {
    ctc_handler_t tch;
    char db_name[SMALL_RECORD_SIZE];
    char sql_str[MAX_DDL_SQL_LEN];
    char user_name[SMALL_RECORD_SIZE];
    char user_ip[SMALL_RECORD_SIZE];
    int error_code;
    char error_message[ERROR_MESSAGE_LEN];
    int result;
};

struct drop_db_pre_check_request {
    ctc_handler_t tch;
    char db_name[SMALL_RECORD_SIZE];
    int result;
    int error_code;
    char error_message[ERROR_MESSAGE_LEN];
};

struct srv_set_savepoint_request {
    char name[SMALL_RECORD_SIZE];
    ctc_handler_t tch;
    int result;
};

struct srv_rollback_savepoint_request {
    char name[SMALL_RECORD_SIZE];
    ctc_handler_t tch;
    int result;
    int32_t csize;
    uint64_t *cursors;
};

struct srv_release_savepoint_request {
    char name[SMALL_RECORD_SIZE];
    ctc_handler_t tch;
    int result;
};

struct index_key_info {
    uint32_t key_lens[MAX_KEY_COLUMNS];
    uint32_t key_offsets[MAX_KEY_COLUMNS];
};

struct index_read_request {
    bool sorted;
    bool need_init;
    uint8_t *record;
    uint16_t record_len;
    uint16_t find_flag;
    char index_name[CTC_MAX_KEY_NAME_LENGTH + 1];
    uint16_t key_num;
    int action;
    int result;
    bool is_key_null[MAX_KEY_COLUMNS];
    uint8_t left_key_record[INDEX_KEY_SIZE];
    uint8_t right_key_record[INDEX_KEY_SIZE];
    struct index_key_info left_key_info;
    struct index_key_info right_key_info;
    ctc_handler_t tch;
    ctc_select_mode_t mode;
    ctc_conds *cond;
    bool is_replace;
    bool index_skip_scan;
};

struct pq_index_read_request {
    bool sorted;
    bool need_init;
    uint8_t *record;
    uint16_t record_len;
    uint16_t find_flag;
    char index_name[CTC_MAX_KEY_NAME_LENGTH + 1];
    uint16_t key_num;
    int action;
    int result;
    ctc_handler_t tch;
    ctc_select_mode_t mode;
    ctc_conds *cond;
    bool is_replace;
    bool index_skip_scan;
    ctc_scan_range_t scan_range;
    uint64_t query_scn;
};

struct set_cursor_range_requst {
    ctc_handler_t tch;
    ctc_page_id_t l_page;
    ctc_page_id_t r_page;
    uint64_t query_scn;
    uint64_t ssn;
    int result;
};

struct index_end_request {
    ctc_handler_t tch;
    int result;
};

struct general_fetch_request {
    ctc_handler_t tch;
    uint16_t record_len;
    uint8_t *record;
    int result;
};

struct general_prefetch_request {
    ctc_handler_t tch;
    int result;
    int max_row_size;
    uint8_t records[MAX_RECORD_SIZE];
    uint16_t record_lens[MAX_PREFETCH_REC_NUM];
    uint32_t recNum[1];
    uint64_t rowids[MAX_PREFETCH_REC_NUM];
};
 
struct free_session_cursors_request {
    ctc_handler_t tch;
    int result;
    int32_t csize;
    uint64_t *cursors;
};

struct get_index_slot_request {
    ctc_handler_t tch;
    int result;
    char index_name[CTC_MAX_KEY_NAME_LENGTH + 1];
};

struct rnd_pos_request {
    ctc_handler_t tch;
    uint16_t record_len;
    uint8_t *record;
    uint16_t pos_length;
    uint8_t position[SMALL_RECORD_SIZE];
    int result;
};

struct position_request {
    ctc_handler_t tch;
    uint16_t pos_length;
    uint8_t position[SMALL_RECORD_SIZE];
    int result;
};

struct delete_all_rows_request {
    ctc_handler_t tch;
    int result;
    dml_flag_t flag;
};

struct knl_write_lob_request {
    ctc_handler_t tch;
    char locator[MAX_LOB_LOCATOR_SIZE];
    int column_id;
    uint32_t data_len;
    bool force_outline;
    int result;
    char data[0];
};

struct knl_read_lob_request {
    ctc_handler_t tch;
    char locator[MAX_LOB_LOCATOR_SIZE];
    uint32_t offset;
    uint32_t size;
    uint32_t read_size;
    int result;
    char buf[0];
};

struct get_max_session_request {
    uint32_t max_sessions;
};

struct analyze_table_request {
    ctc_handler_t tch;
    char table_name[SMALL_RECORD_SIZE];
    char user_name[SMALL_RECORD_SIZE];
    double ratio;
    int result;
};

struct get_cbo_stats_request {
    int result;
    ctc_handler_t tch;
    ctc_cbo_stats_t *stats;
    ctc_cbo_stats_table_t *ctc_cbo_stats_table;
    uint16_t first_partid;
    uint16_t num_part_fetch;
};

struct get_serial_val_request {
    ctc_handler_t tch;
    uint64_t value;
    int result;
    dml_flag_t flag;
};

struct close_mysql_connection_request {
    uint32_t thd_id;
    uint32_t inst_id;
    int result;
};

struct ctc_lock_tables_request {
    ctc_handler_t tch;
    char db_name[SMALL_RECORD_SIZE];
    ctc_lock_table_info lock_info;
    int err_code;
    int result;
};

struct ctc_unlock_tables_request {
    ctc_handler_t tch;
    int result;
    uint32_t mysql_inst_id;
    ctc_lock_table_info lock_info;
};

struct check_table_exists_request {
    char db[SMALL_RECORD_SIZE];
    char name[SMALL_RECORD_SIZE];
    bool is_exists;
    int result;
};
 
struct search_metadata_status_request {
    bool metadata_switch;
    bool cluster_ready;
    int result;
};

struct query_cluster_role_request {
    bool is_slave;
    bool cluster_ready;
    int result;
};

struct update_sample_size_request {
    uint32_t sample_size;
    bool need_persist;
};

struct query_shm_file_num_request {
    uint32_t shm_file_num;
    bool cluster_ready;
    int result;
};

struct query_shm_usage_request {
    uint32_t *shm_usage;
    int result;
};
 
struct set_cluster_role_by_cantian_request {
    bool is_slave;
    int result;
};

struct update_sql_statistic_stat {
    bool enable_stat;
    int result;
};

struct execute_ddl_mysql_sql_request {
    ctc_ddl_broadcast_request broadcast_req;
    uint32_t thd_id;
    int result;
    bool allow_fail;
};

struct execute_mysql_ddl_sql_request {
    ctc_ddl_broadcast_request broadcast_req;
    ctc_handler_t tch;
    int result;
    bool allow_fail;
};

struct execute_mysql_set_opt_request {
    ctc_set_opt_request broadcast_req;
    uint32_t thd_id;
    int result;
    bool allow_fail;
};

struct execute_set_opt_request {
    ctc_set_opt_request broadcast_req;
    ctc_handler_t tch;
    int result;
    bool allow_fail;
};

struct lock_instance_request {
    bool is_mysqld_starting;
    ctc_lock_table_mode_t lock_type;
    ctc_handler_t tch;
    int result;
};

struct unlock_instance_request {
    bool is_mysqld_starting;
    ctc_handler_t tch;
    int result;
};

struct invalidate_mysql_dd_request {
    ctc_invalidate_broadcast_request broadcast_req;
    ctc_handler_t tch;
    int err_code;
    int result;
};

struct query_sql_statistic_stat {
    bool* enable_stat;
    int result;
};

void* alloc_share_mem(void* shm_inst, uint32_t mem_size);

void free_share_mem(void* shm_inst, void* shm_mem);

#ifdef __cplusplus
}
#endif /* __cpluscplus */

#endif