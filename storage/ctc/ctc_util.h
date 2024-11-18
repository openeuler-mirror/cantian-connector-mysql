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

#define CM_IS_EMPTY_STR(str)     (((str) == NULL) || ((str)[0] == 0))

#define CTC_GET_THD_DB_NAME(thd) (thd->db().str == NULL) ? nullptr : const_cast<char *>(thd->db().str)

#define CBO_STRING_MAX_LEN 16

#define OFFSET_VARCHAR_TYPE 2

void ctc_split_normalized_name(const char *file_name, char db[], size_t db_buf_len,
                               char name[], size_t name_buf_len, bool *is_tmp_table);
void ctc_copy_name(char to_name[], const char from_name[], size_t to_buf_len);
bool ctc_check_ddl_sql_length(const string &query_str);

string format_remote_errmsg(const char *err_msg);
// utils for cond pushdown
int dfs_fill_conds(ctc_handler_t m_tch, Item *items, Field **field, ctc_conds *conds, bool no_backslash);
int ctc_push_cond_list(ctc_handler_t m_tch, Item *items, Field **field, ctc_cond_list *list, bool no_backslash);
int ctc_push_cond_args(ctc_handler_t m_tch, Item *items, Field **field, ctc_cond_list *list, bool no_backslash);
int ctc_fill_cond_field(Item *items, Field **field, ctc_conds *cond, bool no_backslash);
int ctc_set_cond_field_size(const field_cnvrt_aux_t *mysql_info, ctc_conds *cond);
int ctc_fill_cond_field_data(ctc_handler_t m_tch, Item *items, Field *mysql_field,
                             const field_cnvrt_aux_t *mysql_info, ctc_conds *cond);
int ctc_fill_cond_field_data_num(ctc_handler_t m_tch, Item *items, Field *mysql_field,
                                 const field_cnvrt_aux_t *mysql_info, ctc_conds *cond);
int ctc_fill_cond_field_data_date(ctc_handler_t m_tch, const field_cnvrt_aux_t *mysql_info,
                                  MYSQL_TIME ltime, date_detail_t *date_detail, ctc_conds *cond);
int ctc_fill_cond_field_data_string(ctc_handler_t m_tch, Item_func *item_func, ctc_conds *cond, bool no_backslash);
void update_value_by_charset(char *data, uint16 *size, uint16 bytes);
ctc_func_type_t item_func_to_ctc_func(Item_func::Functype fc);
int16_t ctc_get_column_by_field(Field **field, const char *col_name);
int ctc_get_column_cs(const CHARSET_INFO *cs);

void cm_assert(bool condition);
string ctc_deserilize_username_with_single_quotation(string &src);
void ctc_print_cantian_err_msg(const ddl_ctrl_t *ddl_ctrl, ct_errno_t ret);
int ctc_check_lock_instance(MYSQL_THD thd, bool &need_forward);
int ctc_check_unlock_instance(MYSQL_THD thd);
int ctc_record_sql(MYSQL_THD thd, bool need_select_db);
int ctc_lock_table_pre(MYSQL_THD thd, vector<MDL_ticket*>& ticket_list, enum_mdl_type mdl_type);
void ctc_lock_table_post(MYSQL_THD thd, vector<MDL_ticket*>& ticket_list);

#pragma GCC visibility push(default)
 
/* exposing API for ctc_proxy */
string sql_without_plaintext_password(ctc_ddl_broadcast_request* broadcast_req);
string ctc_escape_single_quotation_str(string &src);
string cnvrt_name_for_sql(string name);
 
#pragma GCC visibility pop

#endif // __CTC_UTIL_H__