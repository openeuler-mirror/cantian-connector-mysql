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

#ifndef __CTC_UTIL_H__
#define __CTC_UTIL_H__

#include <stddef.h>
#include <string>
#include <regex>
#include <unordered_set>
#include "sql/table.h"
#include "datatype_cnvrtr.h"
#include "sql/item_timefunc.h"
#include "sql/my_decimal.h"
#include "sql/sql_backup_lock.h"

using namespace std;

static unordered_set<string> mysql_system_db{"information_schema", "mysql", "performance_schema", "sys"};

typedef struct en_ctc_cond_func_type_map {
  Item_func::Functype item_func_type;
  ctc_func_type_t func_type;
  ctc_cond_type_t cond_type;
} cond_func_type_map;

#define CM_IS_EMPTY_STR(str)     (((str) == NULL) || ((str)[0] == 0))

#define CTC_GET_THD_DB_NAME(thd) (thd->db().str == NULL) ? nullptr : const_cast<char *>(thd->db().str)

#ifndef WITH_CANTIAN
    #define CBO_STRING_MAX_LEN 16
#else
    #define CBO_STRING_MAX_LEN 64
#endif

#define OFFSET_VARCHAR_TYPE 2

#define SESSION_VARIABLE_NAME_CREATE_INDEX_PARALLELISM "create_index_parallelism"
#define SESSION_VARIABLE_VALUE_MIN_CREATE_INDEX_PARALLELISM 0
#define SESSION_VARIABLE_VALUE_MAX_CREATE_INDEX_PARALLELISM 10
#define SESSION_VARIABLE_VALUE_DEFAULT_CREATE_INDEX_PARALLELISM -1

#define ROWID_FILE_BITS   10
#define ROWID_PAGE_BITS   30
#define ROWID_SLOT_BITS   12
#define ROWID_UNUSED_BITS 12
#define ROWID_VALUE_BITS  52
#pragma pack(4)
// cantian Row ID type identify a physical position of a row
typedef union st_rowid {
  struct {
    uint64_t value : ROWID_VALUE_BITS;
    uint64_t unused1 : ROWID_UNUSED_BITS;
  };

  struct {
    uint64_t file : ROWID_FILE_BITS;  // file
    uint64_t page : ROWID_PAGE_BITS;  // page
    uint64_t slot : ROWID_SLOT_BITS;  // slot number
    uint64_t unused2 : ROWID_UNUSED_BITS;
  };

  struct {
    uint64_t vmid : 32;     // virtual memory page id, dynamic view item, ...
    uint64_t vm_slot : 16;  // slot of virtual memory page, sub item
    uint64_t vm_tag : 16;
  };

  struct {
    uint32_t tenant_id : 16;
    uint32_t curr_ts_num : 16;
    uint32_t ts_id;
  };

  struct {
    uint32_t group_id;
    uint32_t attr_id;
  };

  struct {
    uint32_t pos;
    uint32_t bucket_id : 16;
    uint32_t sub_id : 16;
  };
} rowid_t;
#pragma pack()

int32_t ctc_cmp_cantian_rowid(const rowid_t *rowid1, const rowid_t *rowid2);
void ctc_split_normalized_name(const char *file_name, char db[], size_t db_buf_len,
                               char name[], size_t name_buf_len, bool *is_tmp_table);
void ctc_copy_name(char to_name[], const char from_name[], size_t to_buf_len);
bool ctc_check_ddl_sql_length(const string &query_str);

string format_remote_errmsg(const char *err_msg);
// utils for cond pushdown
int dfs_fill_conds(ctc_handler_t m_tch, Item *items, Field **field, ctc_conds *conds, bool no_backslash,
                   en_ctc_func_type_t *functype);
int ctc_get_column_cs(const CHARSET_INFO *cs);

void cm_assert(bool condition);
string ctc_deserilize_username_with_single_quotation(string &src);
void ctc_print_cantian_err_msg(const ddl_ctrl_t *ddl_ctrl, ct_errno_t ret);
int ctc_check_lock_instance(MYSQL_THD thd, bool &need_forward);
int ctc_check_unlock_instance(MYSQL_THD thd);
int ctc_record_sql(MYSQL_THD thd, bool need_select_db);
int ctc_lock_table_pre(MYSQL_THD thd, vector<MDL_ticket*>& ticket_list, enum_mdl_type mdl_type);
void ctc_lock_table_post(MYSQL_THD thd, vector<MDL_ticket*>& ticket_list);
longlong get_session_variable_int_value_with_range(THD *thd,
                                                   const string &variable_name,
                                                   longlong min,
                                                   longlong max,
                                                   longlong default_value);

#pragma GCC visibility push(default)
 
/* exposing API for ctc_proxy */
string sql_without_plaintext_password(ctc_ddl_broadcast_request* broadcast_req);
string ctc_escape_single_quotation_str(string &src);
string cnvrt_name_for_sql(string name);

typedef bool (*ctc_cond_is_support_t) (Item *term, Item_func::Functype parent_type);
typedef void (*ctc_cond_push_t) (Item *term, Item *&pushed_cond, Item *&remainder_cond);
typedef int (*ctc_cond_fill_t) (Item *item, ctc_conds *cond, Field **field, ctc_handler_t m_tch,
                                ctc_cond_list *list, bool no_backslash, en_ctc_func_type_t *functype);

typedef struct en_ctc_cond_push_map {
    Item::Type type;
    ctc_cond_is_support_t is_support;
    ctc_cond_push_t push;
    ctc_cond_fill_t fill;
} ctc_cond_push_map;

void cond_push_term(Item *term, Item *&pushed_cond, Item *&remainder_cond, Item_func::Functype parent_type);

#pragma GCC visibility pop

#endif // __CTC_UTIL_H__