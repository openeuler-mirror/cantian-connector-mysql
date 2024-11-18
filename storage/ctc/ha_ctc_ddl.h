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
#ifndef __HA_CTC_DDL_H__
#define __HA_CTC_DDL_H__
#include "ctc_srv.h"
#include <mutex>
#include <string>
#include <algorithm>
#include "storage/ctc/ha_ctcpart.h"
#define  UN_SUPPORT_DDL "ddl statement"
/** Max table name length as defined in CT_MAX_NAME_LEN */
#define CTC_MAX_TABLE_NAME_LEN 64
#define CTC_MAX_CONS_NAME_LEN CTC_MAX_TABLE_NAME_LEN
#define CTC_MAX_DATABASE_NAME_LEN CTC_MAX_TABLE_NAME_LEN
#define CTC_MAX_BIT_LEN 64
#define CTC_ENUM_DEFAULT_NULL -1
#define CTC_ENUM_DEFAULT_INVALID -2
#define CTC_MAX_FULL_NAME_LEN (CTC_MAX_TABLE_NAME_LEN + CTC_MAX_DATABASE_NAME_LEN + 14)
#define CTC_TYPE_TIME_SIZE 3
#define CTC_TYPE_DATATIME_SIZE 5
#define CTC_TYPE_TIMPSTAMP_SIZE 4

#define DATA_N_SYS_COLS 3 /* number of system columns defined above */

#define DATA_ITT_N_SYS_COLS 2
/* Maximum values for various fields (for non-blob tuples) */
#define REC_MAX_N_FIELDS (1024 - 1)
#define REC_MAX_HEAP_NO (2 * 8192 - 1)
#define REC_MAX_N_OWNED (16 - 1)

/* Maximum number of user defined fields/columns. The reserved columns
are the ones InnoDB adds internally: DB_ROW_ID, DB_TRX_ID, DB_ROLL_PTR.
We need "* 2" because mlog_parse_index() creates a dummy table object
possibly, with some of the system columns in it, and then adds the 3
system columns (again) using dict_table_add_system_columns(). The problem
is that mlog_parse_index() cannot recognize the system columns by
just having n_fields, n_uniq and the lengths of the columns. */
#define REC_MAX_N_USER_FIELDS (REC_MAX_N_FIELDS - DATA_N_SYS_COLS * 2)

#define CTC_DDL_PROTOBUF_MSG_STACK_SIZE (4 * 1024)  //  < 4kb用栈内存，大于4kb用堆内存
#define CTC_DDL_PROTOBUF_MSG_SIZE (1024 * 1024 * 10) // 10M

typedef enum {
    CTC_CREATE_IF_NOT_EXISTS = 0x00000001,
    CTC_CREATE_OR_REPLACE = 0x00000002,
    CTC_CREATE_TYPE_NO_CHECK_CONS = 0x00000008, // for create child table but ref parent table not create
} ctc_create_option_t;

typedef enum {
    CTC_DROP_IF_EXISTS = 0x00000001,
    CTC_DROP_KEEP_FILES = 0x00000002,    // for tablespace
    CTC_DROP_CASCADE_CONS = 0x00000004,  // for tablespace
    CTC_DROP_NO_CHECK_FK = 0x00000010, // for support drop without checking foreign key
    CTC_DROP_FOR_MYSQL_COPY = 0x00000020, // for support alter parent table using mysql copy
    CTC_DROP_NO_CHECK_FK_FOR_CANTIAN_AND_BROADCAST = 0x01000000, // just for broadcast to set NO_CHECK_FK, not for cantian
} ctc_drop_option_t;

typedef enum {
    CTC_ALTSPACE_ADD_DATAFILE = 0,
    CTC_ALTSPACE_DROP_DATAFILE = 1,
    CTC_ALTSPACE_RENAME_DATAFILE = 2,
    CTC_ALTSPACE_RENAME_SPACE = 3,
    CTC_ALTSPACE_SET_AUTOEXTEND = 4,
    CTC_ALTSPACE_SET_AUTOPURGE = 5,
    CTC_ALTSPACE_SET_RETENTION = 6,
    CTC_ALTSPACE_OFFLINE_DATAFILE = 7,
    CTC_ALTSPACE_SHRINK_SPACE = 8,
    CTC_ALTSPACE_SET_AUTOOFFLINE = 9
} ctc_altspace_action_t;

typedef enum {
    CTC_DDL_FK_RULE_UNKNOW = -1,
    CTC_DDL_FK_RULE_RESTRICT,
    CTC_DDL_FK_RULE_CASCADE,
    CTC_DDL_FK_RULE_SET_NULL
} ctc_ddl_fk_rule;

typedef enum {
    /**
        Used for cases when key algorithm which is supported by SE can't be
        described by one of other classes from this enum (@sa Federated,
        PerfSchema SE, @sa dd::Index::IA_SE_SPECIFIC).

        @note Assigned as default value for key algorithm by parser, replaced by
            SEs default algorithm for keys in mysql_prepare_create_table().
    */
    CTC_HA_KEY_ALG_UNKNOW = -1,
    CTC_HA_KEY_ALG_SE_SPECIFIC = 0,
    CTC_HA_KEY_ALG_BTREE = 1,    /* B-tree. */
    CTC_HA_KEY_ALG_RTREE = 2,    /* R-tree, for spatial searches */
    CTC_HA_KEY_ALG_HASH = 3,     /* HASH keys (HEAP, NDB). */
    CTC_HA_KEY_ALG_FULLTEXT = 4  /* FULLTEXT. */
} ctc_ha_key_alg;

typedef enum {
    COLLATE_DEFAULT = -1,
    COLLATE_GBK_BIN = 3,
    COLLATE_GBK_CHINESE_CI,
    COLLATE_UTF8MB4_GENERAL_CI,
    COLLATE_UTF8MB4_BIN,
    COLLATE_BINARY,
    COLLATE_UTF8MB4_0900_AI_CI,
    COLLATE_UTF8MB4_0900_BIN,
    COLLATE_LATIN1_GENERAL_CI,
    COLLATE_LATIN1_GENERAL_CS,
    COLLATE_LATIN1_BIN,
    COLLATE_ASCII_GENERAL_CI,
    COLLATE_ASCII_BIN,
    COLLATE_UTF8MB3_GENERAL_CI,
    COLLATE_UTF8MB3_BIN,
    COLLATE_UTF8_TOLOWER_CI,
    COLLATE_CP850_GENERAL_CI = 28,
    COLLATE_LATIN1_DANISH_CI = 33,
    COLLATE_LATIN1_GERMAN1_CI = 45,
    COLLATE_HP_ENGLISH_CI = 46,
    COLLATE_UJIS_JAPANESE_CI = 47,
    COLLATE_SWE7_SWEDISH_CI = 48,
    COLLATE_SJIS_JAPANESE_CI = 49,
    COLLATE_KOI8R_GENERAL_CI = 63,
    COLLATE_CP1251_BULGARIAN_CI = 65,
    COLLATE_HEBREW_GENERAL_CI = 83,
    COLLATE_DEC8_SWEDISH_CI = 87,
    COLLATE_SWEDISH_CI = 255,
    COLLATE_LATIN2_GENERAL_CI = 309
} enum_ctc_ddl_collate_type;

// mysql字符序和cantian的参数对接
static map<const int, const int> mysql_collate_num_to_ctc_type = {
  {3, COLLATE_DEC8_SWEDISH_CI},
  {4, COLLATE_CP850_GENERAL_CI},
  {5, COLLATE_LATIN1_GERMAN1_CI},
  {6, COLLATE_HP_ENGLISH_CI},
  {7, COLLATE_KOI8R_GENERAL_CI},
  {8, COLLATE_SWEDISH_CI},
  {9, COLLATE_LATIN2_GENERAL_CI},
  {10, COLLATE_SWE7_SWEDISH_CI},
  {13, COLLATE_SJIS_JAPANESE_CI},
  {12, COLLATE_UJIS_JAPANESE_CI},
  {14, COLLATE_CP1251_BULGARIAN_CI},
  {15, COLLATE_LATIN1_DANISH_CI},
  {16, COLLATE_HEBREW_GENERAL_CI},
  {45, COLLATE_UTF8MB4_GENERAL_CI},
  {46, COLLATE_UTF8MB4_BIN},
  {63, COLLATE_BINARY},
  {255, COLLATE_UTF8MB4_0900_AI_CI},
  {309, COLLATE_UTF8MB4_0900_BIN},
  {48, COLLATE_LATIN1_GENERAL_CI},
  {49, COLLATE_LATIN1_GENERAL_CS},
  {47, COLLATE_LATIN1_BIN},
  {11, COLLATE_ASCII_GENERAL_CI},
  {65, COLLATE_ASCII_BIN},
  {28, COLLATE_GBK_CHINESE_CI},
  {87, COLLATE_GBK_BIN},
  {33, COLLATE_UTF8MB3_GENERAL_CI},
  {83, COLLATE_UTF8MB3_BIN},
  {76, COLLATE_UTF8_TOLOWER_CI},
};

static map<enum ts_alter_tablespace_type, ctc_altspace_action_t> g_ctc_alter_tablespace_map = {
  {ALTER_TABLESPACE_ADD_FILE, CTC_ALTSPACE_ADD_DATAFILE},
  {ALTER_TABLESPACE_DROP_FILE, CTC_ALTSPACE_DROP_DATAFILE},
  {ALTER_TABLESPACE_RENAME, CTC_ALTSPACE_RENAME_SPACE},
  {ALTER_TABLESPACE_OPTIONS, CTC_ALTSPACE_SET_AUTOEXTEND}, // option 只有auto extend适配
};

static const std::unordered_map<string, set_opt_flag> user_var_flag_map = {
    {"ctc_ddl_local_enabled", CTC_DDL_LOCAL_ENABLED},
    {"ctc_ddl_enabled", CTC_DDL_ENABLED},
    {"ctc_replay_ddl", CTC_REPLAY_DDL}
};

class ctc_ddl_stack_mem {
 public:
  ctc_ddl_stack_mem(size_t mem_size):buf_obj(nullptr) {
    set_mem_size(mem_size);
  }
  void set_mem_size(size_t mem_size) {
    free_buf();
    assert(mem_size < CTC_DDL_PROTOBUF_MSG_SIZE);
    if (mem_size <= CTC_DDL_PROTOBUF_MSG_STACK_SIZE) {
      buf_obj = stack_obj;
    } else {
      buf_obj = my_malloc(PSI_NOT_INSTRUMENTED, mem_size, MYF(MY_WME));
      ctc_ddl_req_msg_mem_use_heap_cnt++;
    }
    ctc_ddl_req_msg_mem_max_size =
        std::max(ctc_ddl_req_msg_mem_max_size, mem_size);
  }
  ~ctc_ddl_stack_mem() { free_buf(); }
  void *get_buf() { return buf_obj; }
private:
  void free_buf() {
    if (buf_obj != nullptr && buf_obj != stack_obj) {
      my_free(buf_obj);
      buf_obj = nullptr;
    }
  }
public:
  static size_t ctc_ddl_req_msg_mem_max_size; // 统计ddl的req_msg_mem用的最多内存尺寸
  static size_t ctc_ddl_req_msg_mem_use_heap_cnt; // 统计ddl的req_msg_mem用的堆内存的次数
 private:
  char stack_obj[CTC_DDL_PROTOBUF_MSG_STACK_SIZE];
  void *buf_obj;
};

int fill_delete_table_req(const char *full_path_name, const dd::Table *table_def,
                          THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem);
int ctcbase_alter_tablespace(handlerton *hton, THD *thd,
                             st_alter_tablespace *alter_info,
                             const dd::Tablespace *old_ts_def,
                             dd::Tablespace *new_ts_def);
int ha_ctc_truncate_table(ctc_handler_t *tch, THD *thd, const char *db_name,
                          const char *table_name, bool is_tmp_table);
int check_ctc_identifier_name(const char *in_name);
int fill_create_table_req(HA_CREATE_INFO *create_info, dd::Table *table_def, char *db_name, char *table_name,
                          TABLE *form, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem);
int fill_alter_table_req(TABLE *altered_table, Alter_inplace_info *ha_alter_info, const dd::Table *old_table_def,
                         dd::Table *new_table_def, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem);
int fill_rename_table_req(const char *from, const char *to, const dd::Table *from_table_def,
  dd::Table *to_table_def, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem);
int fill_truncate_partition_req(const char *full_name, partition_info *part_info,
                                dd::Table *dd_table, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem);
int fill_rebuild_index_req(TABLE *table, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem);
bool get_ctc_key_type(const KEY *key_info, int32_t *ret_type);
bool get_ctc_key_algorithm(ha_key_alg algorithm, int32_t *ret_algorithm);
#endif