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

static int cond_fill_null(Item *item, ctc_conds *cond, Field **, ctc_handler_t ,
                          ctc_cond_list *, bool , en_ctc_func_type_t *) {
  cond->cond_type = CTC_CONST_EXPR;
  cond->field_info.null_value = item->null_value;
  cond->field_info.field_type = CTC_DDL_TYPE_NULL;
  cond->field_info.field_size = 0;
  return CT_SUCCESS;
}

int ctc_cond_fill_data_int(ctc_conds *cond, longlong val, ctc_handler_t m_tch, bool is_unsigned) {
  cond->field_info.field_size = sizeof(int64_t);
  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_leaf_data_int: alloc field_data error, size(%u).", cond->field_info.field_size);
    return CT_ERROR;
  }
  
  *(int64_t *)cond->field_info.field_value = val;
  cond->field_info.field_type = CTC_DDL_TYPE_LONGLONG;
  cond->field_info.is_unsigned = is_unsigned;
  return CT_SUCCESS;
}

int cond_fill_int(Item *item, ctc_conds *cond, Field **, ctc_handler_t m_tch, 
                  ctc_cond_list *, bool , en_ctc_func_type_t *) {
  cond->cond_type = CTC_CONST_EXPR;
  Item_int *item_int = dynamic_cast<Item_int *>(item);
  CTC_RET_ERR_IF_NULL(item_int);
  cond->field_info.null_value = item_int->null_value;
  if (cond->field_info.null_value) {
    return CT_SUCCESS;
  }
  return ctc_cond_fill_data_int(cond, item_int->val_int(), m_tch, item_int->unsigned_flag);
}

int cond_fill_real(Item *item, ctc_conds *cond, Field **, ctc_handler_t m_tch, 
                   ctc_cond_list *, bool , en_ctc_func_type_t *) {
  cond->cond_type = CTC_CONST_EXPR;
  cond->field_info.field_type = CTC_DDL_TYPE_DOUBLE;
  Item_float *item_float = dynamic_cast<Item_float *>(item);
  CTC_RET_ERR_IF_NULL(item_float);
  cond->field_info.null_value = item_float->null_value;
  if (cond->field_info.null_value) {
    return CT_SUCCESS;
  }
  cond->field_info.field_size = sizeof(double);
  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_leaf_data_int: alloc field_data error, size(%u).", cond->field_info.field_size);
    return CT_ERROR;
  }
  *(double *)cond->field_info.field_value = item_float->value;
  return CT_SUCCESS;
}

int cond_fill_decimal(Item *item, ctc_conds *cond, Field **, ctc_handler_t m_tch, 
                      ctc_cond_list *, bool , en_ctc_func_type_t *) {
  cond->cond_type = CTC_CONST_EXPR;
  cond->field_info.field_type = CTC_DDL_TYPE_DECIMAL;
  Item_decimal *item_decimal = dynamic_cast<Item_decimal *>(item);
  CTC_RET_ERR_IF_NULL(item_decimal);
  cond->field_info.null_value = item_decimal->null_value;
  if (cond->field_info.null_value) {
    return CT_SUCCESS;
  }
  const int scale = item_decimal->decimals;
  my_decimal *d = item_decimal->val_decimal(nullptr);
  const int prec = d->precision();
  int binary_size = my_decimal_get_binary_size(prec, scale);

  uchar *data = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, binary_size, MYF(MY_WME));
  if (data == nullptr) {
    ctc_log_error("[ctc_fill_cond_field_data_num]alloc mem failed, size(%d)", binary_size);
    my_error(ER_OUT_OF_RESOURCES, MYF(0), "COND FIELD DATA");
    return CT_ERROR;
  }
  my_decimal2binary(E_DEC_FATAL_ERROR, d, data, prec, scale);
  uchar cantian_ptr[DECIMAL_MAX_STR_LENGTH + 1];
  if (decimal_mysql_to_cantian(data, cantian_ptr, prec, scale, (uint32_t *)(&cond->field_info.field_size))) {
    my_free(data);
    return CT_ERROR;
  }

  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_size > 0 && cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_field: alloc field_data error, size(%u).", cond->field_info.field_size);
    my_free(data);
    return CT_ERROR;
  }

  memcpy(cond->field_info.field_value, cantian_ptr, cond->field_info.field_size);
  my_free(data);
  return CT_SUCCESS;
}

void refill_cond_type_date(MYSQL_TIME ltime, en_ctc_func_type_t *functype) {
  if (!ltime.hour || !ltime.minute || !ltime.second || !ltime.second_part) {
    switch (*functype) {
      case CTC_LT_FUNC:
        *functype = CTC_LE_FUNC;
        break;
      case CTC_NE_FUNC:
        *functype = CTC_ISNOTNULL_FUNC;
        break;
      default:
        break;
    }
  }
}

int ctc_fill_cond_field_data_date(ctc_handler_t m_tch, enum_field_types datatype, en_ctc_func_type_t *functype,
                                  MYSQL_TIME ltime, date_detail_t *date_detail, ctc_conds *cond) {
  cond->cond_type = CTC_CONST_EXPR;
  cond->field_info.field_size = sizeof(int64_t);
  int ret = CT_SUCCESS;

  cond->field_info.field_value = ctc_alloc_buf(&m_tch, cond->field_info.field_size);
  if (cond->field_info.field_value == nullptr) {
    ctc_log_error("ctc_fill_cond_field: alloc field_data error, size(%u).", cond->field_info.field_size);
    return CT_ERROR;
  }

  uchar my_ptr[8] = {0};
  longlong ll;
  switch (datatype) {
    case MYSQL_TYPE_TIME:
      cond->field_info.field_type = CTC_DDL_TYPE_TIME;
      ll = TIME_to_longlong_time_packed(ltime);
      my_time_packed_to_binary(ll, my_ptr, DATETIME_MAX_DECIMALS);
      memcpy(cond->field_info.field_value, my_ptr, cond->field_info.field_size);
      return ret;

    case MYSQL_TYPE_DATETIME:
      cond->field_info.field_type = CTC_DDL_TYPE_DATETIME;
      ll = TIME_to_longlong_datetime_packed(ltime);
      my_datetime_packed_to_binary(ll, my_ptr, DATETIME_MAX_DECIMALS);
      memcpy(cond->field_info.field_value, my_ptr, cond->field_info.field_size);
      return ret;

    case MYSQL_TYPE_DATE:
      cond->field_info.field_type = CTC_DDL_TYPE_DATE;
      my_date_to_binary(&ltime, my_ptr);
      memcpy(cond->field_info.field_value, my_ptr, cond->field_info.field_size);
      refill_cond_type_date(ltime, functype);
      return ret;

    case MYSQL_TYPE_TIMESTAMP: {
      cond->field_info.field_type = CTC_DDL_TYPE_TIMESTAMP;
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
      break;
    }
    case MYSQL_TYPE_YEAR:
    default:
      cond->field_info.field_type = CTC_DDL_TYPE_YEAR;
      break;
  }

  ret = assign_mysql_date_detail(datatype, ltime, date_detail);
  if (ret != CT_SUCCESS) {
    return ret;
  }
  cm_encode_date(date_detail, (date_t *)cond->field_info.field_value);
  return ret;
}

int16_t ctc_get_timezone_offset() {
  THD *thd = current_thd;
  bool not_used = false;
  MYSQL_TIME ltime = {1970, 1, 2, 0, 0, 0, 0, 0, MYSQL_TIMESTAMP_DATETIME, 0};
  my_time_t tm_sess = thd->time_zone()->TIME_to_gmt_sec(&ltime, &not_used);
  longlong seconds = SECONDS_IN_24H - tm_sess;
  int16_t timezone_offset_minutes = static_cast<int16_t>(seconds / SECS_PER_MIN);
  return timezone_offset_minutes;
}

int cond_fill_date(Item *item, ctc_conds *cond, Field **, ctc_handler_t m_tch, 
                   ctc_cond_list *, bool , en_ctc_func_type_t *functype) {
  MYSQL_TIME ltime = {0, 0, 0, 0, 0, 0, 0, 0, MYSQL_TIMESTAMP_ERROR, 0};
  date_detail_t date_detail = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  enum_field_types datatype = item->data_type();
  if (datatype == MYSQL_TYPE_YEAR) {
    Item_int *item_int = dynamic_cast<Item_int *>(item);
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
    Item_func *item_date_func = dynamic_cast<Item_func *>(item);
    CTC_RET_ERR_IF_NULL(item_date_func);
    Item_date_literal *item_date_literal = (Item_date_literal *)(item_date_func);
    CTC_RET_ERR_IF_NULL(item_date_literal);
    if (item_date_literal->get_date(&ltime, TIME_FUZZY_DATE)) {
      return CT_ERROR;
    }
  }
  if (current_thd->time_zone()->get_timezone_type() == Time_zone::TZ_OFFSET) {
    cond->field_info.timezone = current_thd->time_zone()->get_timezone_offset() / SECS_PER_MIN;
  } else {
    cond->field_info.timezone = ctc_get_timezone_offset();
  }
  return ctc_fill_cond_field_data_date(m_tch, datatype, functype, ltime, &date_detail, cond);
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

int cond_fill_string(Item *item, ctc_conds *cond, Field **, ctc_handler_t m_tch, 
                     ctc_cond_list *, bool no_backslash, en_ctc_func_type_t *functype) {
  cond->cond_type = CTC_CONST_EXPR;
  cond->field_info.field_type = CTC_DDL_TYPE_VARCHAR;
  cond->field_info.collate_id = ctc_get_column_cs(item->collation.collation);
  if (no_backslash) {
    cond->field_info.no_backslash = true;
  }
  Item_string *item_string = dynamic_cast<Item_string *>(item);
  CTC_RET_ERR_IF_NULL(item_string);
  cond->field_info.null_value = item_string->null_value;
  if (cond->field_info.null_value) {
    return CT_SUCCESS;
  }
  uint cslen = item_string->collation.collation->mbminlen;
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
  if(*functype == CTC_LIKE_FUNC) {
    update_value_by_charset((char *)cond->field_info.field_value, &cond->field_info.field_size, cslen - 1);
  }
  return CT_SUCCESS;
}

int cond_fill_cache(Item *item, ctc_conds *cond, Field **, ctc_handler_t m_tch, 
                    ctc_cond_list *, bool , en_ctc_func_type_t *) {
  cond->cond_type = CTC_CONST_EXPR;
  Item_cache *item_cache = dynamic_cast<Item_cache *>(item);
  CTC_RET_ERR_IF_NULL(item_cache);
  
  cond->field_info.null_value = !item_cache->has_value();
  if (cond->field_info.null_value) {
    return CT_SUCCESS;
  }
  
  int ret;
  switch (item_cache->result_type()) {
    case INT_RESULT: {
      Item_cache_int *cache_int = dynamic_cast<Item_cache_int *>(item);
      CTC_RET_ERR_IF_NULL(cache_int);
      ret = ctc_cond_fill_data_int(cond, cache_int->val_int(), m_tch, cache_int->unsigned_flag);
      break;
    }
    default:
      ctc_log_error("cond_fill_cache: unsupport type of cache item.");
      return CT_ERROR;
  }
  return ret;
}

int cond_fill_field(Item *item, ctc_conds *cond, Field **field, ctc_handler_t , 
                    ctc_cond_list *, bool no_backslash, en_ctc_func_type_t *) {
  Item_field *field_item = dynamic_cast<Item_field *>(item);
  CTC_RET_ERR_IF_NULL(field_item);
  cond->cond_type = CTC_FIELD_EXPR;
  // fill field no
  const char *field_name = field_item->item_name.ptr();
  cond->field_info.field_no = ctc_get_column_by_field(field, field_name);
  if (cond->field_info.field_no == INVALID_MAX_COLUMN) {
    return CT_ERROR;
  }
  if (no_backslash) {
    cond->field_info.no_backslash = true;
  }
  cond->field_info.collate_id = ctc_get_column_cs(field_item->collation.collation);
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
  
  // fill field size
  if (ctc_set_cond_field_size(mysql_info, cond) != CT_SUCCESS) {
    return CT_ERROR;
  }

  return CT_SUCCESS;
}

static const cond_func_type_map g_cond_func_type[] = {
  {Item_func::EQ_FUNC,          CTC_EQ_FUNC,          CTC_CMP_EXPR},
  {Item_func::EQUAL_FUNC,       CTC_EQUAL_FUNC,       CTC_CMP_EXPR},
  {Item_func::NE_FUNC,          CTC_NE_FUNC,          CTC_CMP_EXPR},
  {Item_func::LT_FUNC,          CTC_LT_FUNC,          CTC_CMP_EXPR},
  {Item_func::LE_FUNC,          CTC_LE_FUNC,          CTC_CMP_EXPR},
  {Item_func::GT_FUNC,          CTC_GT_FUNC,          CTC_CMP_EXPR},
  {Item_func::GE_FUNC,          CTC_GE_FUNC,          CTC_CMP_EXPR},
  {Item_func::LIKE_FUNC,        CTC_LIKE_FUNC,        CTC_LIKE_EXPR},
  {Item_func::ISNULL_FUNC,      CTC_ISNULL_FUNC,      CTC_NULL_EXPR},
  {Item_func::ISNOTNULL_FUNC,   CTC_ISNOTNULL_FUNC,   CTC_NULL_EXPR},
  {Item_func::COND_AND_FUNC,    CTC_COND_AND_FUNC,    CTC_LOGIC_EXPR},
  {Item_func::COND_OR_FUNC,     CTC_COND_OR_FUNC,     CTC_LOGIC_EXPR},
  {Item_func::XOR_FUNC,         CTC_XOR_FUNC,         CTC_LOGIC_EXPR},
  {Item_func::MOD_FUNC,         CTC_MOD_FUNC,         CTC_ARITHMATIC_EXPR},
  {Item_func::PLUS_FUNC,        CTC_PLUS_FUNC,        CTC_ARITHMATIC_EXPR},
  {Item_func::MINUS_FUNC,       CTC_MINUS_FUNC,       CTC_ARITHMATIC_EXPR},
  {Item_func::MUL_FUNC,         CTC_MUL_FUNC,         CTC_ARITHMATIC_EXPR},
  {Item_func::DIV_FUNC,         CTC_DIV_FUNC,         CTC_ARITHMATIC_EXPR},
  {Item_func::DATE_FUNC,        CTC_DATE_FUNC,        CTC_DATE_EXPR},
  {Item_func::DATETIME_LITERAL, CTC_DATE_FUNC,        CTC_DATE_EXPR}
};

// Helper function to get condition type mapping
inline static const cond_func_type_map* get_cond_func_type_map(Item_func::Functype fc) {
  for (const auto& mapping : g_cond_func_type) {
    if (mapping.item_func_type == fc) {
        return &mapping;
    }
  }
  return nullptr;
}

inline void set_cond_types(ctc_conds* conds, Item_func::Functype fc) {
  const cond_func_type_map* mapping = get_cond_func_type_map(fc);
  if (mapping) {
    conds->cond_type = mapping->cond_type;
    conds->func_type = mapping->func_type;
  } else {
    // Default values if mapping not found
    conds->cond_type = CTC_UNKNOWN_EXPR;
    conds->func_type = CTC_UNKNOWN_FUNC;
  }
}

int cond_fill_func(Item *item, ctc_conds *cond, Field **field, ctc_handler_t m_tch, 
                   ctc_cond_list *list, bool no_backslash, en_ctc_func_type_t *functype) {
  Item_func *item_func = dynamic_cast<Item_func *>(item);
  CTC_RET_ERR_IF_NULL(item_func);
  
  Item_func::Functype fc = item_func->functype();
  set_cond_types(cond, fc);
  if (cond->cond_type == CTC_DATE_EXPR) {
    // fill in the parent functype in case of adjustment
    ctc_free_buf(&m_tch, (uint8_t *)(list));
    cond->cond_list = nullptr;
    return cond_fill_date(item, cond, field, m_tch, list, no_backslash, functype);
  }

  Item **args = item_func->arguments();
  uint16_t size = item_func->argument_count();

  for (uint16_t i = 0; i < size; i++) {
    ctc_conds *sub_cond = (ctc_conds *)ctc_alloc_buf(&m_tch, sizeof(ctc_conds));
    if (sub_cond == nullptr) {
      ctc_log_error("cond_fill_func: alloc ctc_conds error, size(%lu).", sizeof(ctc_conds));
      return CT_ERROR;
    }
    memset(sub_cond, 0, sizeof(ctc_conds));
    if (list->elements == 0) {
      list->first = sub_cond;
    } else {
      list->last->next = sub_cond;
    }
    list->last = sub_cond;
    (list->elements)++;
    if (dfs_fill_conds(m_tch, args[i], field, sub_cond, no_backslash, &cond->func_type) != CT_SUCCESS) {
      return CT_ERROR;
    }
  }
  return CT_SUCCESS;
}

int cond_fill_cond(Item *item, ctc_conds *cond, Field **field, ctc_handler_t m_tch, 
                   ctc_cond_list *list, bool no_backslash, en_ctc_func_type_t *) {
  Item_cond *item_cond = dynamic_cast<Item_cond *>(item);
  CTC_RET_ERR_IF_NULL(item_cond);

  Item_func::Functype fc = item_cond->functype();
  set_cond_types(cond, fc);
  List<Item> *argument_list = item_cond->argument_list();
  uint16_t size = argument_list->size();
  list_node *node = argument_list->first_node();

  for (uint16_t i = 0; i < size; i++) {
    ctc_conds *sub_cond = (ctc_conds *)ctc_alloc_buf(&m_tch, sizeof(ctc_conds));
    if (sub_cond == nullptr) {
      ctc_log_error("cond_fill_cond: alloc ctc_conds error, size(%lu).", sizeof(ctc_conds));
      return CT_ERROR;
    }
    memset(sub_cond, 0, sizeof(ctc_conds));
    if (list->elements == 0) {
      list->first = sub_cond;
    } else {
      list->last->next = sub_cond;
    }
    list->last = sub_cond;
    (list->elements)++;
    if (dfs_fill_conds(m_tch, (Item *)(node->info), field, sub_cond, no_backslash, NULL) != CT_SUCCESS) {
      return CT_ERROR;
    }
    node = node->next;
  }

  return CT_SUCCESS;
}

static bool is_supported_conds(Item *term, Item_func::Functype parent_type) {
  if (parent_type != Item_func::COND_AND_FUNC && parent_type != Item_func::COND_OR_FUNC) {
    /*
      Unsupported push down condition,
        e.g. select * from tbl where ((col1 < 1) and (col1 > 0)) = 1;
      in which case the parent condition for 
        ((col1 < 1) and (col1 > 0)) is not AND/OR operator
    */
    return false;
  }

  Item_func *item_func = dynamic_cast<Item_func *>(term);
  if (item_func == nullptr) {
    return false;
  }
  
  Item_func::Functype functype = item_func->functype();

  if (functype == Item_func::COND_AND_FUNC ||
      functype == Item_func::COND_OR_FUNC ) {
    return true;
  }
  return false;
}

static bool is_supported_funcs_like(Item_func *item_func, Item_func::Functype parent_type) {
  if (parent_type != Item_func::COND_AND_FUNC && parent_type != Item_func::COND_OR_FUNC) {
    return false;
  }
    /*
      push down LIKE function only for simple condition, eg.
      'select * from tbl where col like "abc%"'
    */
  Item_result cmp_context_next = item_func->arguments()[1]->cmp_context;
  if (item_func->arguments()[1]->type() != Item::STRING_ITEM || cmp_context_next != STRING_RESULT) {
    return false;
  }

  if (item_func->arguments()[0]->type() != Item::FIELD_ITEM) {
    return false;
  }

  Item_func_like *like_func = dynamic_cast<Item_func_like *>(item_func);
  if (like_func == nullptr || like_func->escape_was_used_in_parsing() ) {
    return false;
  }

  Item_field *item_field = dynamic_cast<Item_field *>(item_func->arguments()[0]);
  if (item_field == nullptr) {
    return false;
  }

  if (!is_string_type(item_field->data_type())) {
    return false;
  }

  return true;
}

static bool is_arithmetic_function(Item_func::Functype functype) {
  switch (functype) {
    case Item_func::PLUS_FUNC:
    case Item_func::MINUS_FUNC:
    case Item_func::MUL_FUNC:
    case Item_func::DIV_FUNC:
    case Item_func::MOD_FUNC:
      return true;
    default:
      return false;
  }
}

static bool is_supported_funcs_arithmetic(Item_func *item_func, Item_func::Functype parent_type) {
  if (parent_type == Item_func::COND_AND_FUNC || parent_type == Item_func::COND_OR_FUNC) {
    /*
      Unsupported push down condition,
        eg. select * from tbl where (col1 + 1) and (col1 > 0);
      In which case, condition (col1 + 1) is not comparision operation.
    */
    return false;
  }

  if (item_func->argument_count() != 2) {
    return false;
  }

  /*
    push down arithmetic operation only for integer type, eg.
    'select * from tbl where col1 % 2 = 0;'
  */
  Item **args = item_func->arguments();
  uint16_t size = item_func->argument_count();
  for (uint16_t i = 0; i < size; i++) {
    if (args[i]->type() == Item::FIELD_ITEM) {
      Item_field *item_field = dynamic_cast<Item_field *>(args[i]);
      if (item_field == nullptr) {
        return false;
      }
      if (!is_integer_type(item_field->data_type())) {
        return false;
      }
    } else if (args[i]->type() == Item::FUNC_ITEM) {
      if (!is_arithmetic_function(dynamic_cast<Item_func *>(args[i])->functype())) {
        return false;
      }
    } else if (!(args[i]->type() == Item::INT_ITEM ||
                args[i]->type() == Item::CACHE_ITEM)) {
      return false;
    }
  }
  return true;
}

static bool is_supported_funcs_logic(Item_func *item_func, Item_func::Functype parent_type) {
  if (parent_type != Item_func::COND_AND_FUNC && parent_type != Item_func::COND_OR_FUNC) {
    /*
      Unsupported push down condition,
        eg. select * from tbl where ((col1 < 1) and (col1 > 0))= 1;
    */
    return false;
  }

  /*
    push down logic operation only for comparison condition, eg.
    'select * from tbl where (col1 < 1) and (col1 > 0)'
    'select * from tbl where (col1 < 1) or (col1 > 0)'
    'select * from tbl where (col1 < 1) xor (col1 > 0)'
  */
  if (item_func->argument_count() != 2) {
    return false;
  }
  return true;

  Item **args = item_func->arguments();
  uint16_t size = item_func->argument_count();
  for (uint16_t i = 0; i < size; i++) {
    if (args[i]->type() != Item::FUNC_ITEM) {
      return false;
    }
    Item_func *sub_item = dynamic_cast<Item_func *>(args[i]);
    const cond_func_type_map* mapping = get_cond_func_type_map(sub_item->functype());
    if (mapping == nullptr) {
      return false;
    }
    if (mapping->cond_type == CTC_ARITHMATIC_EXPR || mapping->cond_type == CTC_DATE_EXPR) {
      return false;
    }
  }
  return true;
}

static bool is_supported_funcs_null(Item_func *item_func, Item_func::Functype parent_type) {
  if (parent_type != Item_func::COND_AND_FUNC && parent_type != Item_func::COND_OR_FUNC) {
    return false;
  }
  /*
    push down IS_NULL / IS_NOT_NULL function only for simple condition, eg.
      'select * from tbl where col is null'
  */
  if (item_func->arguments()[0]->type() != Item::FIELD_ITEM) {
    return false;
  } 
  return true;
}

static bool check_cmp_result(Item_field *item_field, Item *right) {
  if (item_field == nullptr) {
    return false;
  }
  Item_result cmp_context_next = right->cmp_context;
  Item::Type type = right->type();

  if (type == Item::NULL_ITEM) {
    return true;
  }

  if (item_field->data_type() == MYSQL_TYPE_YEAR) {
    return type == Item::INT_ITEM && cmp_context_next == INT_RESULT;
  }

  if (is_temporal_type(item_field->data_type())) {
    if (!((right)->basic_const_item() && cmp_context_next == INT_RESULT)) {
      return false;
    }
    if (item_field->data_type() == MYSQL_TYPE_TIMESTAMP) {
      MYSQL_TIME ltime = {0, 0, 0, 0, 0, 0, 0, 0, MYSQL_TIMESTAMP_ERROR, 0};
      Item_func *item_date_func = dynamic_cast<Item_func *>(right);
      if (item_date_func == nullptr) {
        return false;
      }
      Item_date_literal *item_date_literal = (Item_date_literal *)(item_date_func);
      if (item_date_literal == nullptr) {
        return false;
      }
      if (item_date_literal->get_date(&ltime, TIME_FUZZY_DATE)) {
        return false;
      }
      if (non_zero_date(ltime)) {
        return ltime.year && ltime.month && ltime.day;
      }
    }
    return true;
  }

  if (is_string_type(item_field->data_type())) {
    return type == Item::STRING_ITEM && cmp_context_next == STRING_RESULT;
  }

  if (is_integer_type(item_field->data_type())) {
    return (type == Item::INT_ITEM && cmp_context_next == INT_RESULT) || type == Item::CACHE_ITEM;
  }

  if (is_numeric_type(item_field->data_type())) {
    return (type == Item::REAL_ITEM || type == Item::DECIMAL_ITEM) && cmp_context_next != STRING_RESULT;
  }

  return false;
}

static bool is_supported_const(Item *term, Item_func::Functype parent_type) {
  if (parent_type == Item_func::COND_AND_FUNC || parent_type == Item_func::COND_OR_FUNC) {
    return false;
  }

  switch (term->type()) {
    case Item::INT_ITEM:
    case Item::STRING_ITEM:
    case Item::REAL_ITEM:
    case Item::DECIMAL_ITEM:
    case Item::NULL_ITEM:
      return true;
    default:
      return false;
  }
}

static bool is_valid_cmp_operand(Item *operand1, Item *operand2, Item_func::Functype parent_type) {
  if (operand1->type() == Item::FUNC_ITEM) {
    Item_func *item_func = dynamic_cast<Item_func *>(operand1);
    if (item_func == nullptr) {
      return false;
    }
    
    Item_func::Functype functype = item_func->functype();  
    const cond_func_type_map* mapping = get_cond_func_type_map(functype);
    // Check if the function type exists in the g_cond_type array
    if (mapping == nullptr) {
      return false;
    }
    if (mapping->cond_type == CTC_ARITHMATIC_EXPR) {
      if (operand2->type() == Item::INT_ITEM) {
        return true;
      }
      if (operand2->type() == Item::CACHE_ITEM) {
        return dynamic_cast<Item_cache *>(operand2)->result_type() == Item_result::INT_RESULT;
      }
      if (operand2->type() == Item::FIELD_ITEM) {
        return is_integer_type(operand2->data_type());
      }
      if (operand2->type() == Item::FUNC_ITEM) {
        return is_arithmetic_function(dynamic_cast<Item_func *>(operand2)->functype());
      }
      return false;
    }
    return false;
  }

  if (operand1->type() == Item::FIELD_ITEM) {
    return check_cmp_result(dynamic_cast<Item_field *>(operand1), operand2);
  }

  return is_supported_const(operand2, parent_type);
}

static bool is_supported_funcs_cmp(Item_func *item_func, Item_func::Functype parent_type) {
  if (parent_type != Item_func::COND_AND_FUNC && parent_type != Item_func::COND_OR_FUNC) {
    return false;
  }

  if (item_func->argument_count() != 2) {
    return false;
  }
  Item *left = item_func->arguments()[0];
  Item *right = item_func->arguments()[1];
  Item_func::Functype functype = item_func->functype();
  if (is_valid_cmp_operand(left, right, functype) || is_valid_cmp_operand(right, left, functype)) {
    return true;
  }

  return false;
}

static bool is_supported_funcs_date(Item_func *, Item_func::Functype parent_type) {
  if (parent_type == Item_func::COND_AND_FUNC || parent_type == Item_func::COND_OR_FUNC) {
    return false;
  }
  return true;
}

static bool is_supported_funcs(Item *term, Item_func::Functype parent_type) {
  Item_func *item_func = dynamic_cast<Item_func *>(term);
  if (item_func == nullptr) {
    return false;
  }
  
  Item_func::Functype functype = item_func->functype();  
  const cond_func_type_map* mapping = get_cond_func_type_map(functype);
  // Check if the function type exists in the g_cond_type array
  if (mapping == nullptr) {
    return false;
  }
  switch (mapping->cond_type) {
    case CTC_CMP_EXPR:
      return is_supported_funcs_cmp(item_func, parent_type);
    case CTC_LOGIC_EXPR:
      return is_supported_funcs_logic(item_func, parent_type);
    case CTC_NULL_EXPR:
      return is_supported_funcs_null(item_func, parent_type);
    case CTC_LIKE_EXPR:
      return is_supported_funcs_like(item_func, parent_type);
    case CTC_ARITHMATIC_EXPR:
      return is_supported_funcs_arithmetic(item_func, parent_type);
    case CTC_DATE_EXPR:
      return is_supported_funcs_date(item_func, parent_type);
    default:
      break;
  }
  return false;
}

static bool is_supported_cache(Item *term, Item_func::Functype parent_type) {
  if (parent_type == Item_func::COND_AND_FUNC || parent_type == Item_func::COND_OR_FUNC) {
    return false;
  }

  if (term->result_type() == Item_result::INT_RESULT) {
    return true;
  }
  return false;
}

static bool is_supported_datatype_field(enum_field_types datatype) {
  switch (datatype) {
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB: {
      return false;
    }
    default:
      return true;
  }
}

static bool is_supported_field(Item *term, Item_func::Functype parent_type) {
  if (parent_type == Item_func::COND_AND_FUNC || parent_type == Item_func::COND_OR_FUNC) {
    return false;
  }

  Item::Type type = term->type();
    // filter expressions
  if (type != Item::FIELD_ITEM) {
    return false;
  }

  Item_field *item_field = dynamic_cast<Item_field *>(term);
  if (item_field == nullptr) {
    return false;
  }

  // filter generated column
  if (item_field->field->is_gcol() || !item_field->field->part_of_prefixkey.is_clear_all()) {
    return false;
  }

  // filter unsupport datatype
  if (!is_supported_datatype_field(item_field->field->real_type())) {
    return false;
  }

  if (item_field->data_type() == MYSQL_TYPE_STRING) {
    if (item_field->collation.collation->pad_char == '\0') {
      return false;
    }
  }

  if (parent_type == Item_func::ISNULL_FUNC || parent_type == Item_func::ISNOTNULL_FUNC) {
    return true;
  }

  const cond_func_type_map* mapping = get_cond_func_type_map(parent_type);
  if (mapping == nullptr) {
    return false;
  }
  
  if (mapping->cond_type == CTC_CMP_EXPR && item_field->cmp_context == INVALID_RESULT) {
    return false;
  }
  return true;
}

static inline void push_all_cond_item(Item *term, Item *&pushed_cond, Item *&remainder_cond) {
  pushed_cond = term;
  remainder_cond = nullptr;
}

static inline void remain_all_cond_item(Item *term, Item *&pushed_cond, Item *&remainder_cond) {
  pushed_cond = nullptr;
  remainder_cond = term;
}

void push_func_item(Item *term, Item *&pushed_cond, Item *&remainder_cond) {
  Item_func *cond = dynamic_cast<Item_func *>(term);
  if (cond == nullptr) {
    return;
  }
  Item *operand;
  // Item_result result_type;
  for (uint i = 0; i < cond->argument_count(); i++) {
    operand = cond->arguments()[i];
    Item *pushed = nullptr, *remainder = nullptr;
    cond_push_term(operand, pushed, remainder, cond->functype());
    if (pushed == nullptr) {
      // failed to push one of the terms, fails entire cond
      remain_all_cond_item(cond, pushed_cond, remainder_cond);
      return;
    }
  }
  push_all_cond_item(cond, pushed_cond, remainder_cond);
}

int create_and_conditions(Item_cond *cond, List<Item> pushed_list,
                                 List<Item> remainder_list, Item *&pushed_cond,
                                 Item *&remainder_cond) {
  if (remainder_list.is_empty()) {
    // Entire cond pushed, no remainder
    pushed_cond = cond;
    remainder_cond = nullptr;
    return 0;
  }
  if (pushed_list.is_empty()) {
    // Nothing pushed, entire 'cond' is remainder
    pushed_cond = nullptr;
    remainder_cond = cond;
    return 0;
  }

  // Condition was partly pushed, with some remainder
  if (pushed_list.elements == 1) {
    // Single boolean term pushed, return it
    pushed_cond = pushed_list.head();
  } else {
    // Construct an AND'ed condition of pushed boolean terms
    pushed_cond = new Item_cond_and(pushed_list);
    if (pushed_cond == nullptr) {
      return 1;
    }
  }

  if (remainder_list.elements == 1) {
    // A single boolean term as remainder, return it
    remainder_cond = remainder_list.head();
  } else {
    // Construct a remainder as an AND'ed condition of the boolean terms
    remainder_cond = new Item_cond_and(remainder_list);
    if (remainder_cond == nullptr) {
      return 1;
    }
  }
  return 0;
}

int create_or_conditions(Item_cond *cond, List<Item> pushed_list,
                         List<Item> remainder_list, Item *&pushed_cond,
                         Item *&remainder_cond) {
  assert(pushed_list.elements == cond->argument_list()->elements);

  if (remainder_list.is_empty()) {
    // Entire cond pushed, no remainder
    pushed_cond = cond;
    remainder_cond = nullptr;
  } else {
    // When condition was partially pushed, we need to reevaluate
    // original OR-cond on the server side:
    remainder_cond = cond;

    // Construct an OR condition of pushed terms
    pushed_cond = new Item_cond_or(pushed_list);
    if (pushed_cond == nullptr){
      return 1;
    }
  }
  return 0;
}

void cond_push_boolean_term(Item *term, Item *&pushed_cond, Item *&remainder_cond) {
  if (term->type() != Item::COND_ITEM) {
    remain_all_cond_item(term, pushed_cond, remainder_cond);
    return;
  }

  List<Item> pushed_list;
  List<Item> remainder_list;
  Item_cond *cond = dynamic_cast<Item_cond *>(term);
  if (cond == nullptr) {
    return;
  }
  List_iterator<Item> li(*cond->argument_list());
  Item *operand;
  bool is_and_condition = (cond->functype() == Item_func::COND_AND_FUNC);
  while ((operand = li++)) {
    Item *pushed = nullptr, *remainder = nullptr;
    cond_push_term(operand, pushed, remainder, cond->functype());
    if (pushed == nullptr && !is_and_condition) {
      remain_all_cond_item(cond, pushed_cond, remainder_cond);
      return;
    }
    if (pushed != nullptr) pushed_list.push_back(pushed);
    if (remainder != nullptr) remainder_list.push_back(remainder);
  }

  if (is_and_condition) {
    if (create_and_conditions(cond, pushed_list, remainder_list, pushed_cond,
                              remainder_cond)) {
      remain_all_cond_item(cond, pushed_cond, remainder_cond);
      return;
    }
  } else {
    if (create_or_conditions(cond, pushed_list, remainder_list, pushed_cond,
                             remainder_cond)) {
      remain_all_cond_item(cond, pushed_cond, remainder_cond);
      return;
    }
  }
}

const ctc_cond_push_map g_cond_push_map[] = {
    {Item::FUNC_ITEM,    is_supported_funcs, push_func_item,         cond_fill_func},
    {Item::COND_ITEM,    is_supported_conds, cond_push_boolean_term, cond_fill_cond},
    {Item::FIELD_ITEM,   is_supported_field, push_all_cond_item,     cond_fill_field},
    {Item::INT_ITEM,     is_supported_const, push_all_cond_item,     cond_fill_int},
    {Item::REAL_ITEM,    is_supported_const, push_all_cond_item,     cond_fill_real},
    {Item::DECIMAL_ITEM, is_supported_const, push_all_cond_item,     cond_fill_decimal},
    {Item::STRING_ITEM,  is_supported_const, push_all_cond_item,     cond_fill_string},
    {Item::NULL_ITEM,    is_supported_const, push_all_cond_item,     cond_fill_null},
    {Item::CACHE_ITEM,   is_supported_cache, push_all_cond_item,     cond_fill_cache},
};

// Helper function to get condition pushdown mapping
static const ctc_cond_push_map* get_cond_push_map(Item::Type type) {
  for (const auto& mapping : g_cond_push_map) {
    if (mapping.type == type) {
        return &mapping;
    }
  }
  return nullptr;
}

void cond_push_term(Item *term, Item *&pushed_cond, Item *&remainder_cond, Item_func::Functype parent_type) {
  Item::Type type = term->type();
  const ctc_cond_push_map* cond_map = get_cond_push_map(type);
  if (cond_map != nullptr && cond_map->is_support(term, parent_type)) {
    cond_map->push(term, pushed_cond, remainder_cond);
    return;
  } 
  remain_all_cond_item(term, pushed_cond, remainder_cond);
}

int dfs_fill_conds(ctc_handler_t m_tch, Item *items, Field **field, ctc_conds *conds,
                   bool no_backslash, en_ctc_func_type_t *functype) {
  int ret = CT_SUCCESS;
  Item::Type type = items->type();
  const ctc_cond_push_map* cond_map = get_cond_push_map(type);
  if (cond_map == nullptr) {
    return CT_ERROR;
  }
  ctc_cond_list *list = nullptr;
  if (type == Item::FUNC_ITEM || type == Item::COND_ITEM) {
    list = (ctc_cond_list *)ctc_alloc_buf(&m_tch, sizeof(ctc_cond_list));
    if (list == nullptr) {
      ctc_log_error("dfs_fill_conds: alloc ctc_cond_list error, size(%lu).", sizeof(ctc_cond_list));
      return CT_ERROR;
    }
    memset(list, 0, sizeof(ctc_cond_list));
  }
  conds->cond_list = list;
  ret = cond_map->fill(items, conds, field, m_tch, list, no_backslash, functype);
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

/**
    Get int value from session variable if it's in a given range.
    If the variable does not exist or is outside of the range, then a default value is returned.

    @param thd thread handler
    @param variable_name the name of the session variable, without @
    @param min the min of the range
    @param max the max of the range
    @param default_value the default value returned if:
                            1. The variable has not been defined
                            2. The value in the variable is outside of the given range
    @return the value of the session variable or default value, as discussed above
*/
longlong get_session_variable_int_value_with_range(THD *thd,
                                                   const string &variable_name,
                                                   longlong min,
                                                   longlong max,
                                                   longlong default_value)
{
  user_var_entry *var_entry = find_or_nullptr(thd->user_vars, variable_name);
  if (var_entry == nullptr || var_entry->ptr() == nullptr) {
      return default_value;
  }
  bool is_var_null;
  longlong var_value = var_entry->val_int(&is_var_null);
  if ((is_var_null) || (var_value < min || var_value > max)) {
      return default_value;
  }
  return var_value;
}

int32_t ctc_cmp_cantian_rowid(const rowid_t *rowid1, const rowid_t *rowid2) {
  int32_t result = rowid1->file > rowid2->file ? 1 : (rowid1->file < rowid2->file ? (-1) : 0);
  if (result != 0) {
    return result;
  }

  result = rowid1->page > rowid2->page ? 1 : (rowid1->page < rowid2->page ? (-1) : 0);
  if (result != 0) {
    return result;
  }

  result = rowid1->slot > rowid2->slot ? 1 : (rowid1->slot < rowid2->slot ? (-1) : 0);
  return result;
}