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

#include "ctc_srv.h"
#include "ctc_util.h"
#include "ctc_log.h"
#include "ctc_proxy_util.h"

#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/tztime.h"
#include "m_ctype.h"
#include "my_sys.h"
#include "sql/mysqld.h"
#include "sql/strfunc.h"
#include "ha_ctc.h"
#include "ctc_error.h"
#include "decimal_convert.h"
#include "sql_string.h"
#include "ha_ctc_ddl.h"

using namespace std;
extern bool ctc_enable_x_lock_instance;

string cnvrt_name_for_sql(string name) {
  string res = "";
  for (size_t i = 0; i < name.length(); i++) {
    switch (name[i]) {
      case '`':
        res += '`';
      default:
        res += name[i];
    }
}
  return res;
}

void ctc_print_cantian_err_msg(const ddl_ctrl_t *ddl_ctrl, ct_errno_t ret)
{
  switch (ret) {
    case ERR_DUPLICATE_ENTRY:
        my_printf_error(ER_DUP_ENTRY, "%s", MYF(0), ddl_ctrl->error_msg);
        break;
    case ERR_COL_TYPE_MISMATCH:
        my_printf_error(ER_FK_INCOMPATIBLE_COLUMNS, "%s", MYF(0), ddl_ctrl->error_msg);
        break;
    default:
        my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), ddl_ctrl->error_msg);
        break;
  }
}

static uint32_t ctc_convert_identifier_to_sysname(char *to, const char *from, size_t to_len) {
  uint32_t errors_ignored;
  CHARSET_INFO *cs_from = &my_charset_filename;
  CHARSET_INFO *cs_to = system_charset_info;

  return (static_cast<uint32_t>(
      strconvert(cs_from, from, cs_to, to, to_len, &errors_ignored)));
}

void ctc_split_normalized_name(const char *file_name, char db[], size_t db_buf_len,
                               char name[], size_t name_buf_len, bool *is_tmp_table) {
  size_t dir_length, prefix_length;
  string path(file_name);
  const char *buf = path.c_str();

  dir_length = dirname_length(buf);

  if (name != nullptr && is_tmp_table != nullptr && (*is_tmp_table)) {
    /* Get table */
    string table_name = path.substr(dir_length);
    if (table_name.find("#sql") == table_name.npos) {
      *is_tmp_table = false;
    } else {
      assert(table_name.length() <= name_buf_len);
      table_name.copy(name, table_name.length());
      name[table_name.length() + 1] = '\0';
      name[name_buf_len - 1] = '\0';
    }
  }

  assert(db != nullptr);
  if (is_tmp_table != nullptr && (*is_tmp_table)) {
    (void)strncpy(db, TMP_DIR, db_buf_len - 1);
    db[db_buf_len - 1] = '\0';
  } else if (dir_length > 1) {
    /* Get database */
    path.replace(path.begin() + dir_length - 1, path.begin() + dir_length, 1, 0); // Remove end '/'
    prefix_length = dirname_length(buf);
    (void)ctc_convert_identifier_to_sysname(db, buf + prefix_length, db_buf_len - 1);
    db[db_buf_len - 1] = '\0';
  }
}


void ctc_copy_name(char to_name[], const char from_name[], size_t to_buf_len) {
  if (to_name != from_name) {
    (void)strncpy(to_name, from_name, to_buf_len - 1);
    to_name[to_buf_len - 1] = '\0';
  }
}

bool ctc_check_ddl_sql_length(const string &query_str) {
  if (query_str.length() > MAX_DDL_SQL_LEN_CONTEXT) {
    string err_msg =
      "`" + query_str.substr(0, 100) + "...` Is Large Than " + to_string(MAX_DDL_SQL_LEN_CONTEXT);
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), err_msg.c_str());
    return true;
  }
  return false;
}

string sql_without_plaintext_password(ctc_ddl_broadcast_request* broadcast_req) {
  if (broadcast_req->options & CTC_CURRENT_SQL_CONTAIN_PLAINTEXT_PASSWORD) {
    return "(contains plaintext password), sql_command = " + to_string(broadcast_req->sql_command);
  }
  return (string)broadcast_req->sql_str;
}

int16_t ctc_get_column_by_field(Field **field, const char *col_name) {
  int16_t col_id; 
  for (col_id = 0; *field != nullptr; field++, col_id++) {
    if (my_strcasecmp(system_charset_info, (*field)->field_name, col_name) == 0) {
     return col_id;
    }
  }
  return INVALID_MAX_COLUMN;
}

int ctc_set_cond_field_size(const field_cnvrt_aux_t *mysql_info, ctc_conds *cond) {
  switch (mysql_info->cantian_map_type) {
    case CANTIAN_COL_BITS_4:
      cond->field_info.field_size = 4;
      break;
    case CANTIAN_COL_BITS_8:
      cond->field_info.field_size = 8;
      break;
    case CANTIAN_COL_BITS_VAR:{
      if (mysql_info->sql_data_type == STRING_DATA || mysql_info->sql_data_type == NUMERIC_DATA) {
        return CT_SUCCESS;
      }
      break;
    }
    default:
      ctc_log_error("ctc_set_cond_field_size: unknow col bits: %d", mysql_info->cantian_map_type);
      return CT_ERROR;
  }
  return CT_SUCCESS;
}

int ctc_fill_cond_field_data_num(ctc_handler_t m_tch, Item *items, Field *mysql_field,
                                 const field_cnvrt_aux_t *mysql_info, ctc_conds *cond) {
  int ret = CT_SUCCESS;
  void *data = nullptr;
  bool is_alloc_data = CT_FALSE;
  Item_func_comparison *item_func_comparison = dynamic_cast<Item_func_comparison *>(items);
  CTC_RET_ERR_IF_NULL(item_func_comparison);
  switch (mysql_info->ddl_field_type) {
    case CTC_DDL_TYPE_LONG:
    case CTC_DDL_TYPE_LONGLONG: {
      Item_func *item_func = dynamic_cast<Item_func *>(items);
      CTC_RET_ERR_IF_NULL(item_func);
      longlong val;
      if ((item_func->arguments()[1])->type() == Item::CACHE_ITEM) {
        Item_cache_int *item_cache_int = dynamic_cast<Item_cache_int *>(item_func_comparison->arguments()[1]);
        CTC_RET_ERR_IF_NULL(item_cache_int);
        val = item_cache_int->val_int();
      } else {
        Item_int *item_int = dynamic_cast<Item_int *>(item_func_comparison->arguments()[1]);
        CTC_RET_ERR_IF_NULL(item_int);
        val = item_int->val_int();
      }
      data = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(longlong), MYF(MY_WME));
      if (data == nullptr) {
        ctc_log_error("[ctc_fill_cond_field_data_num]alloc mem failed, size(%ld)", sizeof(longlong));
        my_error(ER_OUT_OF_RESOURCES, MYF(0), "COND FIELD DATA");
        return CT_ERROR;
      }
      is_alloc_data = CT_TRUE;
      memcpy(data, &val, sizeof(longlong));
      break;
    }
    case CTC_DDL_TYPE_DOUBLE: {
      Item_float *item_float = dynamic_cast<Item_float *>(item_func_comparison->arguments()[1]);
      CTC_RET_ERR_IF_NULL(item_float);
      data = &item_float->value;
      break;
    }
    case CTC_DDL_TYPE_NEWDECIMAL: {
      const int scale = mysql_field->decimals();
      Field_new_decimal *field_new_decimal = dynamic_cast<Field_new_decimal *>(mysql_field);
      CTC_RET_ERR_IF_NULL(field_new_decimal);
      const int prec = field_new_decimal->precision;
      int binary_size = my_decimal_get_binary_size(prec, scale);
      uchar *buff = new uchar[binary_size];
      Item_decimal *item_decimal = dynamic_cast<Item_decimal *>(item_func_comparison->arguments()[1]);
      CTC_RET_ERR_IF_NULL(item_decimal);
      my_decimal *d = item_decimal->val_decimal(nullptr);
      my_decimal2binary(E_DEC_FATAL_ERROR, d, buff, prec, scale);
      data = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, binary_size, MYF(MY_WME));
      if (data == nullptr) {
        ctc_log_error("[ctc_fill_cond_field_data_num]alloc mem failed, size(%d)", binary_size);
        my_error(ER_OUT_OF_RESOURCES, MYF(0), "COND FIELD DATA");
        return CT_ERROR;
      }
      is_alloc_data = CT_TRUE;
      memcpy(data, buff, binary_size);
      delete[] buff;
      break;
    }
    default:
      ctc_log_error("[ctc_copy_cond_field_data]unsupport sql_data_type %d", mysql_info->sql_data_type);
      assert(0);
      return CT_ERROR;
  }
  uchar cantian_ptr[DECIMAL_MAX_STR_LENGTH + 1];
  ret = convert_numeric_to_cantian(mysql_info, (const uchar *)data, cantian_ptr, mysql_field,
                                   (uint32_t *)(&cond->field_info.field_size));
  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_size > 0 && cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_field: alloc field_data error, size(%u).", cond->field_info.field_size);
    return CT_ERROR;
  }

  memcpy(cond->field_info.field_value, cantian_ptr, cond->field_info.field_size);
  if (is_alloc_data) {
    my_free(data);
    is_alloc_data = CT_FALSE;
  }
  return ret;
}

void refill_cond_type_date(MYSQL_TIME ltime, ctc_conds *cond) {
  if (!ltime.hour || !ltime.minute || !ltime.second || !ltime.second_part) {
    switch (cond->func_type) {
      case CTC_LT_FUNC:
        cond->func_type = CTC_LE_FUNC;
        break;
      case CTC_NE_FUNC:
        cond->func_type = CTC_ISNOTNULL_FUNC;
        break;
      default:
        break;
    }
  }
}

int ctc_fill_cond_field_data_date(ctc_handler_t m_tch, const field_cnvrt_aux_t *mysql_info,
                                  MYSQL_TIME ltime, date_detail_t *date_detail, ctc_conds *cond) {
  int ret = CT_SUCCESS;

  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_size > 0 && cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_field: alloc field_data error, size(%u).", cond->field_info.field_size);
    return CT_ERROR;
  }

  uchar my_ptr[8] = {0};
  longlong ll;
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_TIME:
      ll = TIME_to_longlong_time_packed(ltime);
      my_time_packed_to_binary(ll, my_ptr, DATETIME_MAX_DECIMALS);
      memcpy(cond->field_info.field_value, my_ptr, cond->field_info.field_size);
      return ret;

    case MYSQL_TYPE_DATETIME:
      ll = TIME_to_longlong_datetime_packed(ltime);
      my_datetime_packed_to_binary(ll, my_ptr, DATETIME_MAX_DECIMALS);
      memcpy(cond->field_info.field_value, my_ptr, cond->field_info.field_size);
      return ret;

    case MYSQL_TYPE_DATE:
      my_date_to_binary(&ltime, my_ptr);
      memcpy(cond->field_info.field_value, my_ptr, cond->field_info.field_size);
      refill_cond_type_date(ltime, cond);
      return ret;

    case MYSQL_TYPE_TIMESTAMP: {
      if (!check_zero_time_ltime(ltime)) {
        THD *thd = current_thd;
        int warnings = 0;
#ifdef FEATURE_X_FOR_MYSQL_32
        struct my_timeval tm = {0, 0};
        datetime_with_no_zero_in_date_to_timeval(&ltime, *thd->time_zone(), &tm, &warnings);
#elif defined(FEATURE_X_FOR_MYSQL_26)
        struct timeval tm = {0, 0};
        ctc_datetime_with_no_zero_in_date_to_timeval(&ltime, *thd->time_zone(), &tm, &warnings);
#endif
        assert((warnings == EOK) || (warnings == MYSQL_TIME_WARN_TRUNCATED));
        my_tz_UTC->gmt_sec_to_TIME(&ltime, tm);
      }
      /* fall through */
    }

    default:
      ret = assign_mysql_date_detail(mysql_info->mysql_field_type, ltime, date_detail);
      if (ret != CT_SUCCESS) {
        return ret;
      }
      cm_encode_date(date_detail, (date_t *)cond->field_info.field_value);
      return ret;
  }

}

void update_value_by_charset(char *data, uint16 *size, uint16 bytes) {
  if (bytes == 0) {
    return;
  }
  uint16 cur = 0;
  for (int i = 0; i < *size; i++) {
    if (data[i] == '_' || data[i] == '%') {
      cur -= bytes;
    }
    data[cur++] = data[i];
  }
  *size = cur;
}

int ctc_get_column_cs(const CHARSET_INFO *cs) {
  auto it = mysql_collate_num_to_ctc_type.find(cs->number);
  if (it != mysql_collate_num_to_ctc_type.end()) {
    return (int32_t)it->second;
  }
  return cs->number;
}

int ctc_fill_cond_field_data_string(ctc_handler_t m_tch, Item_func *item_func,
                                    ctc_conds *cond, bool no_backslash) {
  if ((item_func->arguments()[1])->type() == Item::NULL_ITEM) {
    cond->field_info.null_value = true;
    return CT_SUCCESS;
  }
  Item_field *item_field = dynamic_cast<Item_field *>((item_func)->arguments()[0]);
  CTC_RET_ERR_IF_NULL(item_field);
  uint cslen = item_field->collation.collation->mbminlen;
  cond->field_info.collate_id = ctc_get_column_cs(item_field->collation.collation);
  if (no_backslash) {
    cond->field_info.no_backslash = true;
  }
  Item_string *item_string = dynamic_cast<Item_string *>(item_func->arguments()[1]);
  CTC_RET_ERR_IF_NULL(item_string);
  String *item_str = item_string->val_str(nullptr);
  cond->field_info.field_size = item_str->length();
  void *data = item_str->ptr();
  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_size > 0 && cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_field: alloc field_data error, size(%u).", cond->field_info.field_size);
    return CT_ERROR;
  }
  memset(cond->field_info.field_value, 0,  cond->field_info.field_size);
  memcpy(cond->field_info.field_value, data, cond->field_info.field_size);
  if(cond->func_type == CTC_LIKE_FUNC) {
    update_value_by_charset((char *)cond->field_info.field_value, &cond->field_info.field_size, cslen - 1);
  }
  return CT_SUCCESS;
}

int ctc_fill_cond_field_data(ctc_handler_t m_tch, Item *items, Field *mysql_field,
                             const field_cnvrt_aux_t *mysql_info, ctc_conds *cond) {
  int ret = CT_SUCCESS;
  Item_func *item_func = dynamic_cast<Item_func *>(items);
  CTC_RET_ERR_IF_NULL(item_func);
  Item_func_comparison *item_func_comparison = dynamic_cast<Item_func_comparison *>(items);
  CTC_RET_ERR_IF_NULL(item_func_comparison);
  if ((item_func->arguments()[1])->type() == Item::CACHE_ITEM) {
    Item_cache *item_cache = dynamic_cast<Item_cache *>(item_func_comparison->arguments()[1]);
    CTC_RET_ERR_IF_NULL(item_cache);
    cond->field_info.null_value = !item_cache->has_value();
  } else {
    cond->field_info.null_value = item_func_comparison->arguments()[1]->null_value;
  }
  if (cond->field_info.null_value) {
    return CT_SUCCESS;
  }

  switch (mysql_info->sql_data_type) {
    case NUMERIC_DATA:
      ret = ctc_fill_cond_field_data_num(m_tch, items, mysql_field, mysql_info, cond);
      break;
    case DATETIME_DATA:{
      MYSQL_TIME ltime;
      date_detail_t date_detail;
      memset(&date_detail, 0, sizeof(date_detail_t));
      Item_func_comparison *item_func_comparison = dynamic_cast<Item_func_comparison *>(items);
      CTC_RET_ERR_IF_NULL(item_func_comparison);
      if (mysql_info->mysql_field_type == MYSQL_TYPE_YEAR) {
        Item_int *item_int = dynamic_cast<Item_int *>(item_func_comparison->arguments()[1]);
        CTC_RET_ERR_IF_NULL(item_int);
        ltime.year = item_int->value;
        ltime.month = 1;
        ltime.day = 1;
        ltime.hour = 0;
        ltime.minute = 0;
        ltime.second = 0;
        ltime.second_part = 0;
        ltime.neg = false;
      } else {
        Item_func *item_date_func = dynamic_cast<Item_func *>(item_func->arguments()[1]);
        CTC_RET_ERR_IF_NULL(item_date_func);
        Item_date_literal *item_date_literal = (Item_date_literal *)(item_date_func);
        CTC_RET_ERR_IF_NULL(item_date_literal);
        if (item_date_literal->get_date(&ltime, TIME_FUZZY_DATE)) {
          return CT_ERROR;
        }
      }
      ret = ctc_fill_cond_field_data_date(m_tch, mysql_info, ltime, &date_detail, cond);
      break;
    }
    case STRING_DATA:{
      ret = ctc_fill_cond_field_data_string(m_tch, item_func, cond, false);
      break;
    }
    case LOB_DATA:
    case UNKNOW_DATA:
    default:
      ctc_log_error("[mysql2cantian]unsupport sql_data_type %d", mysql_info->sql_data_type);
      return CT_ERROR;
  }
  return ret;
}

int ctc_fill_cond_field(ctc_handler_t m_tch, Item *items, Field **field, ctc_conds *cond, bool no_backslash) {
  Item_func *item_func = dynamic_cast<Item_func *>(items);
  CTC_RET_ERR_IF_NULL(item_func);
  const char *field_name = item_func->arguments()[0]->item_name.ptr();
  cond->field_info.field_no = ctc_get_column_by_field(field, field_name);
  if (cond->field_info.field_no == INVALID_MAX_COLUMN) {
    return CT_ERROR;
  }
  Field *mysql_field = *(field + cond->field_info.field_no);
  enum_field_types type = mysql_field->type();
  type = (type == MYSQL_TYPE_FLOAT) ? MYSQL_TYPE_DOUBLE : type;
  const field_cnvrt_aux_t *mysql_info = get_auxiliary_for_field_convert(mysql_field, type);
  cond->field_info.field_type = mysql_info->ddl_field_type;
  // update field_no if there are gcol in tables 
  uint16_t gcol_cnt = 0;
  for (uint16_t col_id = 0; col_id < cond->field_info.field_no; col_id++) {
    Field *pre_field = *(field + col_id);
    if (pre_field->is_gcol()) {
      gcol_cnt++;
    }
  }
  cond->field_info.field_no -= gcol_cnt;
  if (cond->func_type == CTC_ISNULL_FUNC || cond->func_type == CTC_ISNOTNULL_FUNC) {
    return CT_SUCCESS;
  } else if(cond->func_type == CTC_LIKE_FUNC) {
    return ctc_fill_cond_field_data_string(m_tch, item_func, cond, no_backslash);
  }

  if (ctc_set_cond_field_size(mysql_info, cond) != CT_SUCCESS) {
    return CT_ERROR;
  }

  return ctc_fill_cond_field_data(m_tch, items, mysql_field, mysql_info, cond);
}

int ctc_push_cond_list(ctc_handler_t m_tch, Item *items, Field **field,
                       ctc_cond_list *list, bool no_backslash) {
  Item_cond *item_cond = dynamic_cast<Item_cond *>(items);
  CTC_RET_ERR_IF_NULL(item_cond);
  List<Item> *argument_list = item_cond->argument_list();
  uint16_t size = argument_list->size();
  list_node *node = argument_list->first_node();

  for (uint16_t i = 0; i < size; i++) {
    ctc_conds *cond = (ctc_conds *)ctc_alloc_buf(&m_tch, sizeof(ctc_conds));
    if (cond == nullptr) {
      ctc_log_error("ctc_push_cond_list: alloc ctc_conds error, size(%lu).", sizeof(ctc_conds));
      return CT_ERROR;
    }
    memset(cond, 0, sizeof(ctc_conds));
    if (dfs_fill_conds(m_tch, (Item *)(node->info), field, cond, no_backslash) != CT_SUCCESS) {
      return CT_ERROR;
    }
    if (list->elements == 0) {
      list->first = cond;
    } else {
      list->last->next = cond;
    }
    list->last = cond;
    (list->elements)++;
    node = node->next;
  }

  return CT_SUCCESS;
}

int ctc_push_cond_args(ctc_handler_t m_tch, Item *items, Field **field,
                       ctc_cond_list *list, bool no_backslash) {
  Item_func *item_func = dynamic_cast<Item_func *>(items);
  CTC_RET_ERR_IF_NULL(item_func);
  Item **args = item_func->arguments();
  uint16_t size = item_func->argument_count();

  for (uint16_t i = 0; i < size; i++) {
    ctc_conds *cond = (ctc_conds *)ctc_alloc_buf(&m_tch, sizeof(ctc_conds));
    if (cond == nullptr) {
      ctc_log_error("ctc_push_cond_args: alloc ctc_conds error, size(%lu).", sizeof(ctc_conds));
      return CT_ERROR;
    }
    memset(cond, 0, sizeof(ctc_conds));
    dfs_fill_conds(m_tch, args[i], field, cond, no_backslash);
    if (list->elements == 0) {
      list->first = cond;
    } else {
      list->last->next = cond;
    }
    list->last = cond;
    (list->elements)++;
  }

  return CT_SUCCESS;
}

ctc_func_type_t item_func_to_ctc_func(Item_func::Functype fc) {
  switch (fc) {
    case (Item_func::Functype::EQUAL_FUNC):
      return CTC_EQUAL_FUNC;
    case (Item_func::Functype::EQ_FUNC):
      return CTC_EQ_FUNC;
    case (Item_func::Functype::NE_FUNC):
      return CTC_NE_FUNC;
    case (Item_func::Functype::LT_FUNC):
      return CTC_LT_FUNC;
    case (Item_func::Functype::LE_FUNC):
      return CTC_LE_FUNC;
    case (Item_func::Functype::GT_FUNC):
      return CTC_GT_FUNC;
    case (Item_func::Functype::GE_FUNC):
      return CTC_GE_FUNC;
    case (Item_func::Functype::ISNULL_FUNC):
      return CTC_ISNULL_FUNC;
    case (Item_func::Functype::ISNOTNULL_FUNC):
      return CTC_ISNOTNULL_FUNC;
    case (Item_func::Functype::LIKE_FUNC):
      return CTC_LIKE_FUNC;
    case (Item_func::Functype::NOT_FUNC):
      return CTC_NOT_FUNC;
    case (Item_func::Functype::COND_AND_FUNC):
      return CTC_COND_AND_FUNC;
    case (Item_func::Functype::COND_OR_FUNC):
      return CTC_COND_OR_FUNC;
    case (Item_func::Functype::XOR_FUNC):
      return CTC_XOR_FUNC;
    default:
      return CTC_UNKNOWN_FUNC;
  }
}

int dfs_fill_conds(ctc_handler_t m_tch, Item *items, Field **field, ctc_conds *conds, bool no_backslash) {
  Item_func *item_func = dynamic_cast<Item_func *>(items);
  CTC_RET_ERR_IF_NULL(item_func);
  Item_func::Functype fc = item_func->functype();
  conds->func_type = item_func_to_ctc_func(fc);
  int ret = CT_SUCCESS;
  ctc_cond_list *list;

  switch (conds->func_type) {
    case CTC_COND_AND_FUNC:
    case CTC_COND_OR_FUNC:
      list = (ctc_cond_list *)ctc_alloc_buf(&m_tch, sizeof(ctc_cond_list));
      if (list == nullptr) {
        ctc_log_error("ctc_fill_conds: alloc ctc_cond_list error, size(%lu).", sizeof(ctc_cond_list));
        return CT_ERROR;
      }
      memset(list, 0, sizeof(ctc_cond_list));
      ret = ctc_push_cond_list(m_tch, items, field, list, no_backslash);
      conds->cond_list = list;
      break;
    case CTC_NOT_FUNC:
    case CTC_XOR_FUNC:
      list = (ctc_cond_list *)ctc_alloc_buf(&m_tch, sizeof(ctc_cond_list));
      if (list == nullptr) {
        ctc_log_error("ctc_fill_conds: alloc ctc_cond_list error, size(%lu).", sizeof(ctc_cond_list));
        return CT_ERROR;
      }
      memset(list, 0, sizeof(ctc_cond_list));
      ret = ctc_push_cond_args(m_tch, items, field, list, no_backslash);
      conds->cond_list = list;
      break;
    case CTC_EQ_FUNC:
    case CTC_EQUAL_FUNC:
    case CTC_NE_FUNC:
    case CTC_LT_FUNC:
    case CTC_LE_FUNC:
    case CTC_GE_FUNC:
    case CTC_GT_FUNC:
    case CTC_ISNULL_FUNC:
    case CTC_ISNOTNULL_FUNC:
    case CTC_LIKE_FUNC:
      ret = ctc_fill_cond_field(m_tch, item_func, field, conds, no_backslash);
      break;
    case CTC_UNKNOWN_FUNC:
    default:
      return CT_ERROR;
  }
  return ret;
}

void cm_assert(bool condition)
{
    if (!condition) {
        *((uint32 *)NULL) = 1;
    }
}

/*
    reference mysql function 'get_text' to implement deserilize get_text
@note:
    1.遇到反斜杠
        后一个字符是_,%，该字符与'\\'一起保持原样 => '\\' + '_' | '\\' + '%'
        后一个字符是其他字符，转义反斜杠本身 => '\\' + '\\'
    2.遇到Mysql认为的特殊字符
        拆成两个字符， '\\' + x
    3.普通字符
        追加即可
*/
string ctc_deserilize_get_text(string &name) {
  THD *thd = current_thd;
  string res("");
  int len = name.size();
  if (!(thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)) {
    res = name;
    return res;
  }
  for (int i = 0; i < len; i++) {
    if (name[i] == '\\' && i < len - 1) {
      switch (name[++i]) {
        case '_':
        case '%':
          res += '\\';
          res += name[i];
          break;
        default:
          res += '\\';
          res += '\\';
          --i;
          break;
      }
    } else {
      // 识别单个特殊字符(转移字符) => 两个字符：\\ + 本身
      switch (name[i]) {
        case '\n':
          res += '\\';
          res += 'n';
          break;
        case '\t':
          res += '\\';
          res += 't';
          break;
        case '\r':
          res += '\\';
          res += 'r';
          break;
        case '\b':
          res += '\\';
          res += 'b';
          break;
        case '\032':
          res += '\\';
          res += 'Z';
          break;
        default:
          res += name[i];
          break;
      }
    }
  }
  return res;
}

string ctc_escape_single_quotation_str(string &src) {
  string res = "";
  for (size_t i = 0; i < src.length(); i++) {
    switch (src[i]) {
      case '\'':
        res += '\\';
      default:
        res += src[i];
    }
  }
  return res;
}

string ctc_deserilize_username_with_single_quotation(string &src) {
  string deserilize = ctc_deserilize_get_text(src);
  return ctc_escape_single_quotation_str(deserilize);
}

/**
  Check for global name lock counts to determine if ddl is processing.
  @return
    - true: there is ddl in progress
    - false: no ddl is in progress
*/
static bool ctc_is_ddl_processing() {
  uint32_t name_locks = get_g_name_locks();
  if (name_locks > 0) {
    ctc_log_system("[CTC_LOCK_INSTANCE]: contains %u global name locks, there is DDL in progress.", name_locks);
    return true;
  }
  return false;
}

int ctc_check_lock_instance(MYSQL_THD thd, bool &need_forward) {
  if (thd->mdl_context.has_locks(MDL_key::BACKUP_LOCK)) {
    need_forward = false;
    return 0;
  }

  if (ctc_is_ddl_processing()) {
    my_printf_error(ER_DISALLOWED_OPERATION, "Please try lock instance for backup later, DDL is in processing.", MYF(0));
    return -1;
  }

  if (acquire_exclusive_backup_lock(thd, 0, false)) {
    my_printf_error(ER_DISALLOWED_OPERATION, "Please try lock instance for backup later, DDL is in processing.", MYF(0));
    ctc_log_error("[CTC_LOCK_INSTANCE]: Not allowed to lock instance, DDL is in processing");
    return -1;
  }
  
  ctc_lock_table_mode_t lock_mode;
  bool is_mysqld_starting = is_starting();
  if (ctc_enable_x_lock_instance || is_mysqld_starting) {
    lock_mode = CTC_LOCK_MODE_EXCLUSIVE;
  } else {
    lock_mode = CTC_LOCK_MODE_SHARE;
  }
  
  ctc_handler_t tch;
  handlerton *ctc_hton = get_ctc_hton();
  if (get_tch_in_handler_data(ctc_hton, thd, tch)) {
    ctc_log_error("[CTC_LOCK_INSTANCE]: failed to get tch");
    release_backup_lock(thd);
    return -1;
  }

  int ret = ctc_lock_instance(&is_mysqld_starting, lock_mode, &tch);
  update_sess_ctx_by_tch(tch, ctc_hton, thd);
  assert(ret == 0);

  ctc_log_system("[CTC_LOCK_INSTANCE]: SUCCESS. ctc_inst:%u, conn_id:%u, lock_mode:%s",
                  tch.inst_id, tch.thd_id, lock_mode == CTC_LOCK_MODE_EXCLUSIVE ? "X_LATCH" : "S_LATCH");
  return ret;
}

int ctc_check_unlock_instance(MYSQL_THD thd) {
  if (!thd->mdl_context.has_locks(MDL_key::BACKUP_LOCK)) {
    return 0;
  }

  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(get_ctc_hton(), thd, tch));

  bool is_mysqld_starting = is_starting();
  ctc_unlock_instance(&is_mysqld_starting, &tch);
  ctc_log_system("[CTC_UNLOCK_INSTANCE]: SUCCESS. ctc_inst:%u, conn_id:%u", tch.inst_id, tch.thd_id);
  return 0;
}

#ifdef FEATURE_X_FOR_MYSQL_32
static inline bool is_temporary_table_being_opened(const Table_ref *table)
#elif defined(FEATURE_X_FOR_MYSQL_26)
static inline bool is_temporary_table_being_opened(const TABLE_LIST *table)
#endif
{
  return table->open_type == OT_TEMPORARY_ONLY ||
         (table->open_type == OT_TEMPORARY_OR_BASE &&
          is_temporary_table(table));
}

int ctc_lock_table_pre(MYSQL_THD thd, vector<MDL_ticket*>& ticket_list, enum_mdl_type mdl_type) {
#ifdef FEATURE_X_FOR_MYSQL_32
  Table_ref *tables_start = thd->lex->query_tables;
  Table_ref *tables_end = thd->lex->first_not_own_table();
  Table_ref *table;
#elif defined(FEATURE_X_FOR_MYSQL_26)
  TABLE_LIST *tables_start = thd->lex->query_tables;
  TABLE_LIST *tables_end = thd->lex->first_not_own_table();
  TABLE_LIST *table;
#endif
  for (table = tables_start; table && table != tables_end;
       table = table->next_global) {
    if (is_temporary_table_being_opened(table)) {
      continue;
    }
    MDL_request req;
    MDL_REQUEST_INIT(&req, MDL_key::TABLE, table->db, table->table_name,
                     mdl_type, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&req, 1)) {
      return 1;
    }
    ticket_list.push_back(req.ticket);
  }
  return 0;
}

void ctc_lock_table_post(MYSQL_THD thd, vector<MDL_ticket*>& ticket_list) {
  for (auto it = ticket_list.begin(); it != ticket_list.end(); ++it) {
    thd->mdl_context.release_lock(*it);
  }
  ticket_list.clear();
}