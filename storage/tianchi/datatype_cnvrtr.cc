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
#include <map>
#include "field_types.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "my_bitmap.h"
#include "datatype_cnvrtr.h"
#include "sql/tztime.h"  // my_tz_find, my_tz_OFFSET0
#include "m_string.h"
#include "myisampack.h"      // mi_int2store
#include "tse_error.h"
#include "tse_log.h"
#include "decimal_convert.h"
#include "tse_srv.h"
#include "sql/json_dom.h"

#define LOWER_LENGTH_BYTES  (uint8)0x01 // Bytes that are used to represent the length of variable length data

#define UNITS_PER_DAY 86400000000LL
#define SECONDS_PER_HOUR        3600U
#define SECONDS_PER_MIN         60U
#define CM_BASELINE_DAY ((int32)730120) /* == days_before_year(CM_BASELINE_YEAY) + 1 */
#define DAYS_1   365
#define DAYS_4   (DAYS_1 * 4 + 1)
#define DAYS_100 (DAYS_4 * 25 - 1)
#define DAYS_400 (DAYS_100 * 4 + 1)
#define IS_LEAP_YEAR(year) (((year) % 4 == 0) && (((year) % 100 != 0) || ((year) % 400 == 0)) ? 1 : 0)
#define SECONDS_PER_DAY         86400U
#define CM_MIN_YEAR      1
#define CM_MAX_YEAR      9999
/** Check whether the year is valid */
#define CM_IS_VALID_YEAR(year) ((year) >= CM_MIN_YEAR && (year) <= CM_MAX_YEAR)
#define CT_SPRS_COLUMNS  (uint32)1024
#define CM_ALIGN16(size) ((((size)&0x0F) == 0) ? (size) : ((size) + 0x10 - ((size)&0x0F)))
#define OFFSET_OF offsetof
#define IS_SPRS_ROW(row) ((row)->column_count == 0)

uint16 g_cantian_month_days[2][12] = {
  { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
  { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

#define CM_MONTH_DAYS(year, mon) (g_cantian_month_days[IS_LEAP_YEAR(year)][(mon) - 1])

/**
    Obtains the mapping between MySQL data types and Cantian data types, including the storage format
    and DDL transmission on the Cantian side.according to the field type (not real_type()).
    MYSQL_TYPE_ENUM -> MYSQL_TYPE_TINY(0-255),MYSQL_TYPE_SHORT(>255)
    MYSQL_TYPE_SET -> MYSQL_TYPE_TINY(0-8),MYSQL_TYPE_SHORT(8-16),MYSQL_TYPE_INT24(16-32),MYSQL_TYPE_LONGLONG(32-64)
    Tips:if add new val in enum_field_types, should update this array!
*/
static field_cnvrt_aux_t g_field_cnvrt_aux_array[] = {
  {MYSQL_TYPE_DECIMAL,     CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_DECIMAL    },
  {MYSQL_TYPE_TINY,        CANTIAN_COL_BITS_4,    NUMERIC_DATA,  TSE_DDL_TYPE_LONG       },
  {MYSQL_TYPE_SHORT,       CANTIAN_COL_BITS_4,    NUMERIC_DATA,  TSE_DDL_TYPE_LONG       },
  {MYSQL_TYPE_LONG,        CANTIAN_COL_BITS_4,    NUMERIC_DATA,  TSE_DDL_TYPE_LONG       },
  {MYSQL_TYPE_FLOAT,       CANTIAN_COL_BITS_8,    NUMERIC_DATA,  TSE_DDL_TYPE_DOUBLE     },
  {MYSQL_TYPE_DOUBLE,      CANTIAN_COL_BITS_8,    NUMERIC_DATA,  TSE_DDL_TYPE_DOUBLE     },
  {MYSQL_TYPE_NULL,        CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_NULL       },
  {MYSQL_TYPE_TIMESTAMP,   CANTIAN_COL_BITS_8,    DATETIME_DATA, TSE_DDL_TYPE_TIMESTAMP  },
  {MYSQL_TYPE_LONGLONG,    CANTIAN_COL_BITS_8,    NUMERIC_DATA,  TSE_DDL_TYPE_LONGLONG   },
  {MYSQL_TYPE_INT24,       CANTIAN_COL_BITS_4,    NUMERIC_DATA,  TSE_DDL_TYPE_LONG       },
  {MYSQL_TYPE_DATE,        CANTIAN_COL_BITS_8,    DATETIME_DATA, TSE_DDL_TYPE_DATE       },
  {MYSQL_TYPE_TIME,        CANTIAN_COL_BITS_8,    DATETIME_DATA, TSE_DDL_TYPE_TIME       },
  {MYSQL_TYPE_DATETIME,    CANTIAN_COL_BITS_8,    DATETIME_DATA, TSE_DDL_TYPE_DATETIME   },
  {MYSQL_TYPE_YEAR,        CANTIAN_COL_BITS_8,    DATETIME_DATA, TSE_DDL_TYPE_YEAR       },
  {MYSQL_TYPE_NEWDATE,     CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_NEWDATE    },
  {MYSQL_TYPE_VARCHAR,     CANTIAN_COL_BITS_VAR,  STRING_DATA,   TSE_DDL_TYPE_VARCHAR    },
  {MYSQL_TYPE_BIT,         CANTIAN_COL_BITS_8,  NUMERIC_DATA,   TSE_DDL_TYPE_LONGLONG   },
  {MYSQL_TYPE_TIMESTAMP2,  CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_TIMESTAMP2 },
  {MYSQL_TYPE_DATETIME2,   CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_DATETIME2  },
  {MYSQL_TYPE_TIME2,       CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_TIME2      },
  {MYSQL_TYPE_TYPED_ARRAY, CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_TYPED_ARRAY},
  // > MYSQL_TYPE_INVALID
  {MYSQL_TYPE_JSON,        CANTIAN_COL_BITS_VAR,  LOB_DATA,      TSE_DDL_TYPE_JSON       },
  {MYSQL_TYPE_NEWDECIMAL,  CANTIAN_COL_BITS_VAR,  NUMERIC_DATA,  TSE_DDL_TYPE_NEWDECIMAL },
  {MYSQL_TYPE_TINY_BLOB,   CANTIAN_COL_BITS_VAR,  LOB_DATA,      TSE_DDL_TYPE_TINY_BLOB  },
  {MYSQL_TYPE_MEDIUM_BLOB, CANTIAN_COL_BITS_VAR,  LOB_DATA,      TSE_DDL_TYPE_MEDIUM_BLOB},
  {MYSQL_TYPE_LONG_BLOB,   CANTIAN_COL_BITS_VAR,  LOB_DATA,      TSE_DDL_TYPE_LONG_BLOB  },
  {MYSQL_TYPE_BLOB,        CANTIAN_COL_BITS_VAR,  LOB_DATA,      TSE_DDL_TYPE_BLOB       },
  // The following two types not used for datatype convert, Unconfirmed scenarios in which this feature will be used.
  {MYSQL_TYPE_VAR_STRING,  CANTIAN_COL_BITS_NULL, UNKNOW_DATA,   TSE_DDL_TYPE_VAR_STRING },
  {MYSQL_TYPE_STRING,      CANTIAN_COL_BITS_VAR,  STRING_DATA,   TSE_DDL_TYPE_STRING     }
};
/**
  get idx in g_field_cnvrt_aux_array.
  Tips: when add new val in g_field_cnvrt_aux_array, should also update this func!
*/
int32_t get_idx_for_field_convert(enum_field_types mysql_field_type)
{
  if (mysql_field_type < MYSQL_TYPE_INVALID) {
    return mysql_field_type;
  }
  if (mysql_field_type >= MYSQL_TYPE_JSON && mysql_field_type <= MYSQL_TYPE_NEWDECIMAL) {
    return mysql_field_type - MYSQL_TYPE_JSON + (MYSQL_TYPE_TYPED_ARRAY - MYSQL_TYPE_DECIMAL + 1);
  }
  if (mysql_field_type >= MYSQL_TYPE_TINY_BLOB && mysql_field_type <= MYSQL_TYPE_STRING) {
    return mysql_field_type - MYSQL_TYPE_TINY_BLOB + (MYSQL_TYPE_TYPED_ARRAY - MYSQL_TYPE_DECIMAL + 1) +
      (MYSQL_TYPE_NEWDECIMAL - MYSQL_TYPE_JSON + 1) ;
  }
  return -1;
}

const field_cnvrt_aux_t* get_auxiliary_for_field_convert(Field *field, enum_field_types mysql_field_type)
{
  if (mysql_field_type < MYSQL_TYPE_INVALID) {
    return &g_field_cnvrt_aux_array[mysql_field_type];
  }
  enum_field_types mysql_real_type = mysql_field_type;
  if (field->real_type() == MYSQL_TYPE_ENUM ||
      field->real_type() == MYSQL_TYPE_SET) {
    uint field_len = field->pack_length();
    switch (field_len) {
      case 1:
        mysql_real_type = MYSQL_TYPE_TINY;
        break;
      case 2:
        mysql_real_type = MYSQL_TYPE_SHORT;
        break;
      case 3:
        mysql_real_type = MYSQL_TYPE_INT24;
        break;
      case 4:
        mysql_real_type = MYSQL_TYPE_LONG;
        break;
      case 8:
      default:
        mysql_real_type = MYSQL_TYPE_LONGLONG;
        break;
    }
  }
  int32_t idx = get_idx_for_field_convert(mysql_real_type);
  if (idx == -1) {
    return nullptr;
  }
  field_cnvrt_aux_t* ret = &g_field_cnvrt_aux_array[idx];

  if (field->is_flag_set(BLOB_FLAG)) {
    if (field->charset() == &my_charset_bin &&
        field->is_flag_set(BINARY_FLAG)) {
      ret->ddl_field_type = TSE_DDL_TYPE_BLOB;
    } else {
      ret->ddl_field_type = TSE_DDL_TYPE_CLOB;
    }
  }

  return ret;
}

static inline void cm_decode_leap(date_detail_t *detail, int32 *d)
{
  uint32 hundred_count;
  int32 days = *d;

  while (days >= DAYS_400) {
    detail->year += 400;
    days -= DAYS_400;
  }

  for (hundred_count = 1; days >= DAYS_100 && hundred_count < 4; hundred_count++) {
    detail->year += 100;
    days -= DAYS_100;
  }

  while (days >= DAYS_4) {
    detail->year += 4;
    days -= DAYS_4;
  }

  while (days > DAYS_1) {
    if (IS_LEAP_YEAR(detail->year)) {
      days--;
    }

    detail->year++;
    days -= DAYS_1;
  }

  *d = days;
}

static void cm_set_all_zero_detail(date_detail_t *detail)
{
  detail->year = 0;
  detail->mon = 0;
  detail->day = 0;
  detail->hour = 0;
  detail->min = 0;
  detail->sec = 0;
  detail->millisec = 0;
  detail->microsec = 0;
  detail->nanosec = 0;
  return;
}

/**
  @brief
  decode cantian date
*/
void cm_decode_date(date_t date, date_detail_t *detail)
{
  int32 i;
  int32 days;
  uint16 *day_tab = NULL;
  int64 time;

  if (detail == nullptr) {
    assert(0);
  }

  if (date == CM_ALL_ZERO_DATETIME) {
    cm_set_all_zero_detail(detail);
    return;
  }

  // decode time
  time = date;
  date /= UNITS_PER_DAY;
  time -= date * UNITS_PER_DAY;

  if (time < 0) {
    time += UNITS_PER_DAY;
    date -= 1;
  }

  detail->microsec = (uint16)(time % MICROSECS_PER_MILLISEC);
  time /= MICROSECS_PER_MILLISEC;

  detail->millisec = (uint16)(time % MILLISECS_PER_SECOND);
  time /= MILLISECS_PER_SECOND;

  detail->hour = (uint8)(time / SECONDS_PER_HOUR);
  time -= (uint32)detail->hour * SECONDS_PER_HOUR;

  detail->min = (uint8)(time / SECONDS_PER_MIN);
  time -= (uint32)detail->min * SECONDS_PER_MIN;

  detail->sec = (uint8)time;

  // "days -> (year, month, day), considering 01-Jan-0001 as day 1."
  days = (int32)(date + CM_BASELINE_DAY);  // number of days since 1.1.1 to the date
  detail->year = 1;

  cm_decode_leap(detail, &days);

  if (days == 0) {
    detail->year--;
    detail->mon = 12;
    detail->day = 31;
  } else {
    day_tab = g_cantian_month_days[IS_LEAP_YEAR(detail->year)];
    detail->mon = 1;

    i = 0;
    while (days > (int32)day_tab[i]) {
      days -= (int32)day_tab[i];
        i++;
    }

    detail->mon = (uint8)(detail->mon + i);
    detail->day = (uint8)(days);
  }
}

/* "year -> number of days before January 1st of year" */
static inline int32 days_before_year(int32 year)
{
  --year;
  return year * 365 + year / 4 - year / 100 + year / 400;
}

static inline int32 total_days_before_date(const date_detail_t *detail)
{
  int32 i;
  int32 total_days;

  if (detail == nullptr) {
    assert(0);
  }

  // compute total days
  total_days = days_before_year((int32)detail->year) - CM_BASELINE_DAY;
  uint16 *day_tab = (uint16 *)g_cantian_month_days[IS_LEAP_YEAR(detail->year)];
  for (i = 0; i < (int32)(detail->mon - 1); i++) {
    total_days += (int32)day_tab[i];
  }
  total_days += detail->day;

  return total_days;
}
/**
  @brief
  encode cantian date
*/
void cm_encode_date(const date_detail_t *detail, date_t *date)
{
  int32 total_days;

  if (detail == nullptr) {
    assert(0);
  }

  if (check_zero_date(*detail)) {
    *date = CM_ALL_ZERO_DATETIME;
    return;
  }

  assert(CM_IS_VALID_YEAR(detail->year));
  assert(detail->mon >= 1 && detail->mon <= 12);
  assert(detail->day >= 1 && detail->day <= 31);
  assert(detail->hour <= 23);
  assert(detail->min <= 59);
  assert(detail->sec <= 59);
  assert(detail->microsec <= 999);
  assert(detail->millisec <= 999);

  // compute total days
  total_days = total_days_before_date(detail);

  // encode the date into an integer with 1 nanosecond as the the minimum unit
  *date = (int64)total_days * SECONDS_PER_DAY;
  *date += (uint32)detail->hour * SECONDS_PER_HOUR;
  *date += (uint32)detail->min * SECONDS_PER_MIN;
  *date += detail->sec;
  *date = *date * MILLISECS_PER_SECOND + detail->millisec;
  *date = *date * MICROSECS_PER_MILLISEC + detail->microsec;
  return;
}

/**
  @brief
  Reads a uint32_t stored in the little-endian format.
  @param[in]  buf         from where to read.
  @return                 the value in uint32_t format.
*/
static inline uint32_t read_from_2_little_endian(const uint8_t *buf)
{
  return ((uint32_t)(buf[0]) | ((uint32_t)(buf[1]) << 8));
}

/**
  @brief
  decimal is stored by my_decimal struct in mysql , and stored by dec4_t in cantian;
  mysql->cantian: mysql_ptr -> my_decimal -> str -> dec8_t -> dec4_t
  decimal(prec,scale):prec indicates the maximum number of significant digits,
  scale indicates the maximum number of decimal places that can be stored.
*/
int decimal_mysql_to_cantian(const uint8_t *mysql_ptr, uchar *cantian_ptr, Field *mysql_field, uint32 *length)
{
  int ret = 0;
  const int scale = mysql_field->decimals();
  Field_new_decimal *f = dynamic_cast<Field_new_decimal *>(mysql_field);
  TSE_RET_ERR_IF_NULL(f);
  const int prec = f->precision;
  my_decimal d;
  ret = binary2my_decimal(E_DEC_FATAL_ERROR, mysql_ptr, &d, prec, scale);
  if (ret != E_DEC_OK) {
    tse_log_error("[mysql2cantians]Decimal data type convert binary to my_decimal failed!");
    return ret;
  }
  char buff[DECIMAL_MAX_STR_LENGTH + 1];
  int len = sizeof(buff);
  ret = decimal2string(&d, buff, &len);
  if (ret != E_DEC_OK) {
    tse_log_error("[mysql2cantian]Decimal data type convert my_decimal to string failed!");
    return ret;
  }
  dec8_t d8;
  if (ct_cm_str_to_dec8(buff, &d8) != CT_SUCCESS_STATUS) {
    tse_log_error("[mysql2cantian]Decimal data type convert str to dec8 failed!");
    assert(0);
  }
  cm_dec_8_to_4((dec4_t *)cantian_ptr, &d8);
  *length = (uint32)cm_dec4_stor_sz((dec4_t *)cantian_ptr);
  return ret;
}

void decimal_cantian_to_mysql(uint8_t *mysql_ptr, uchar *cantian_ptr, Field *mysql_field)
{
  dec8_t dec;
  dec4_t *d4 = (dec4_t *)cantian_ptr;
  ct_cm_dec_4_to_8(&dec, d4, (uint32)cm_dec4_stor_sz(d4));
  char str[DEC_MAX_NUM_SAVING_PREC];
  if (ct_cm_dec8_to_str(&dec, DEC_MAX_NUM_SAVING_PREC, str) != CT_SUCCESS_STATUS) {
    tse_log_error("[cantian2mysql]Decimal data type convert dec8 to str failed!");
    assert(0);
  }

  my_decimal decimal_value;
  const char *decimal_data = str;
  const char *end = strend(decimal_data);
  if (string2decimal(decimal_data, &decimal_value, &end) != E_DEC_OK) {
    tse_log_error("[cantian2mysql]Decimal data type convert str to my_decimal failed!");
    assert(0);
  }

  Field_new_decimal *f = dynamic_cast<Field_new_decimal *>(mysql_field);
  if (f == nullptr) {
    tse_log_error("[cantian2mysql]Decimal data type convert my_decimal to binary failed!");
    assert(0);
  }

  const int prec = f->precision;
  const int scale = mysql_field->decimals();
  if (my_decimal2binary(E_DEC_FATAL_ERROR, &decimal_value, mysql_ptr, prec, scale) != E_DEC_OK) {
    tse_log_error("[cantian2mysql]Decimal data type convert my_decimal to binary failed!");
    assert(0);
  }
}

int decimal_mysql_to_double(const uint8_t *mysql_ptr, uchar *double_ptr, Field *mysql_field)
{
  int ret = 0;
  const int scale = mysql_field->decimals();
  Field_new_decimal *f = dynamic_cast<Field_new_decimal *>(mysql_field);
  TSE_RET_ERR_IF_NULL(f);
  const int prec = f->precision;
  my_decimal d;
  ret = binary2my_decimal(E_DEC_FATAL_ERROR, mysql_ptr, &d, prec, scale);
  if (ret != E_DEC_OK) {
    tse_log_error("[decimal2double]: Decimal data type convert binary to my_decimal failed!");
    return ret;
  }
  ret = decimal2double(&d, (double *)double_ptr);
  if (ret != E_DEC_OK) {
    tse_log_error("[decimal2double]: Decimal data type convert my_decimal to double failed!");
  }
  return ret;
}

static inline uint32 get_lob_locator_size(void* locator)
{
  lob_locator_t *lob = (lob_locator_t *)locator;
  if (lob->head.is_outline) {
    return sizeof(lob_locator_t);
  }
  return (uint32)(lob->head.size + OFFSET_OF(lob_locator_t, data));
}

static inline uint32_t get_blob_buffer_size(uint32_t remain_size)
{
  if (remain_size < LOB_DATA_SIZE_100K) {
    return g_blob_field_buffer_size_map.at(LOB_DATA_SIZE_100K);
  }

  if (remain_size < LOB_DATA_SIZE_1M) {
    return g_blob_field_buffer_size_map.at(LOB_DATA_SIZE_1M);
  }

  if (remain_size < LOB_DATA_SIZE_8M) {
    return g_blob_field_buffer_size_map.at(LOB_DATA_SIZE_8M);
  }

  return LOB_DATA_SIZE_8M;
}

/**
  @brief
  blob data is stored in mysql Field_blob struct, which is in one piece of continuous memory
  we can get the ptr and length from Field_blob get_blob_data and get_length interface,
  then invoke the Cantian tse_knl_write_lob interface, write blob data to tse storage engine
  piece by piece. It's important to note that one piece of blob data preferably greater than 4000, 
  because the tse storage engine is store blob data inline when blob data length less than 4000, 
  otherwise the blob data store outline.
*/
int convert_blob_to_cantian(uchar *cantian_ptr, uint32 &cantian_offset,
                            uchar *blob_str, uint32 remain_size,
                            tianchi_handler_t &tch, uint column_id, uint32_t &field_len) {
  uint32_t buffer_size = get_blob_buffer_size(remain_size);
  assert(buffer_size >= TSE_LOB_LOCATOR_BUF_SIZE);

  lob_text_t piece_lob;
  uint32_t offset = 0;
  lob_locator_t *lob_locator = nullptr;
  uint32 locator_size = OFFSET_OF(lob_locator_t, data) + remain_size;
  locator_size = locator_size > TSE_LOB_LOCATOR_BUF_SIZE ? TSE_LOB_LOCATOR_BUF_SIZE : locator_size;
  lob_locator = (lob_locator_t *)my_malloc(PSI_NOT_INSTRUMENTED, locator_size, MYF(MY_WME));
  if (lob_locator == nullptr) {
    tse_log_error("[mysql2cantian]Apply for lob locator:%u Failed", locator_size);
    my_error(ER_OUT_OF_RESOURCES, MYF(0), "LOB LOCATOR");
    field_len = 0;
    return HA_ERR_SE_OUT_OF_MEMORY;
  }
  memset(lob_locator, 0xFF, locator_size);
  lob_locator->head.size = 0;
  // for inline
  if (remain_size <= TSE_LOB_MAX_INLINE_SIZE) {
    lob_locator->head.type = 0;
    lob_locator->head.is_outline = false;
    lob_locator->head.size = remain_size;
    memcpy(lob_locator->data, blob_str, remain_size);
    remain_size = 0;
  }
  // for outline
  bool force_outline = false;
  while (remain_size > 0) {
    piece_lob.str = (char *) blob_str + offset;
    piece_lob.len = remain_size > buffer_size ? buffer_size : remain_size;
    int ret = tse_knl_write_lob(&tch, (char *)lob_locator, locator_size, column_id,
                                (void *)piece_lob.str, (uint32_t)piece_lob.len, force_outline);
    if (ret != CT_SUCCESS) {
      tse_log_error("[mysql2cantian]tse_knl_write_lob Failed:%d", ret);
      my_free(lob_locator);
      lob_locator = nullptr;
      field_len = 0;
      return convert_tse_error_code_to_mysql((ct_errno_t)ret);
    }
    if (!force_outline) {
      force_outline = true;
    }
    remain_size -= piece_lob.len;
    offset += piece_lob.len;
  }
  // get cantian Field_len and store the length
  field_len = get_lob_locator_size(lob_locator);
  // store the length data
  *(uint16_t *)&cantian_ptr[cantian_offset] = field_len;
  cantian_offset += sizeof(uint16_t);
  // copy data
  memcpy(&cantian_ptr[cantian_offset], lob_locator, field_len);
    
  my_free(lob_locator);
  lob_locator = nullptr;
  return CT_SUCCESS;
}

void convert_json_to_mysql(Field *mysql_field)
{
  // json binary serialize
  if (mysql_field->type() == MYSQL_TYPE_JSON) {
    Field_json *json = dynamic_cast<Field_json *>(mysql_field);
    Json_wrapper wr;
    if (json == nullptr || (bitmap_is_set(json->table->read_set, mysql_field->field_index()) && json->val_json(&wr))) {
      tse_log_error("[convert_json_to_mysql] get json value failed");
      assert(0);
    }
  }
  return;
}
/**
  @brief
  The blob data stored in the TSE storage engine can be in-line or out-line scenarios.
  However, you can query the blob data by calling the tse_knl_read_lob interface in
  sharding mode. Then you need to assemble the fragmented data into a whole memory block
  and insert it into the MySQL by delivery the address of lob buf.
*/
void convert_blob_to_mysql(uchar *cantian_ptr, Field *mysql_field, tianchi_handler_t &tch, uint8_t *mysql_ptr)
{
  bitmap_set_bit(mysql_field->table->read_set, mysql_field->field_index());
  lob_locator_t *locator = (lob_locator_t *)(uint8*)cantian_ptr;
  uint32_t blob_len = locator->head.size;
  char *blob_buf = (char *)my_malloc(PSI_NOT_INSTRUMENTED, blob_len * sizeof(char), MYF(MY_WME));
  if (blob_len == 0) {
    tse_log_note("[cantian2mysql]data type %d for 0 length", mysql_field->type());
    // Insert in Mysql when the data is empty
    memcpy(mysql_ptr, &blob_buf, sizeof(char *));
    convert_json_to_mysql(mysql_field);
    return;
  }

  if (blob_buf == nullptr) {
    tse_log_error("[cantian2mysql]Apply for blob buf:%u Failed", blob_len);
    my_error(ER_OUT_OF_RESOURCES, MYF(0), "BLOB DATA");
    return;
  }

  if (!locator->head.is_outline) {
    memcpy(blob_buf, locator->data, blob_len);
    memcpy(mysql_ptr, &blob_buf, sizeof(char *));
    convert_json_to_mysql(mysql_field);
    return;
  }

  uint32_t buffer_size = get_blob_buffer_size(locator->head.size);
  uint32_t piece_len = buffer_size;
  uint32_t offset = 0;
  uint32_t read_size = 0;

  uint32_t remain_size = blob_len;
  bool read_flag = true;
  // read blob data piece by piece
  while (remain_size > 0) {
    piece_len = remain_size > buffer_size ? buffer_size : remain_size;
    int ret = tse_knl_read_lob(&tch, (char *)locator, offset, blob_buf + offset, piece_len, &read_size);
    if (ret != CT_SUCCESS) {
      tse_log_error("[cantian2mysql]tse_knl_read_lob Failed : %d", ret);
      read_flag = false;
      break;
    }
    if (read_size <= 0) {
      tse_log_note("[cantian2mysql]tse_knl_read_lob read lob length is %d,stop read!", read_size);
      read_flag = false;
      break;
    }
    remain_size -= read_size;
    offset += read_size;
  }
  
  if (!read_flag) {
    my_free(blob_buf);
    blob_buf = nullptr;
  }
  // Insert in Mysql,notify:do not free blob_buf
  memcpy(mysql_ptr, &blob_buf, sizeof(char *));
  convert_json_to_mysql(mysql_field);
  return;
}

static inline void row_set_column_bits2(row_head_t *row, uint8_t bits,
                                        uint32_t col_id)
{
  uint32_t map_id;
  uint8_t *bitmap = nullptr;

  map_id = col_id >> 2;
  bitmap = row->bitmap;

  // erase bits
  bitmap[map_id] &= ~(0x03 << ((col_id & 0x03) << 1));

  // set bits
  bitmap[map_id] |= bits << (((uint8_t)(col_id & 0x03)) << 1);
}

static inline uint8_t row_get_column_bits2(row_head_t *row, uint32_t id)
{
  uint32_t map_id;
  uint8_t *bitmap = nullptr;

  map_id = id >> 2;
  bitmap = row->bitmap;

  return (uint8_t)(bitmap[map_id] >> ((id & 0x03) << 1)) & (uint8_t)0x03;
}

static inline uint16 col_bitmap_ex_size(uint16 cols)
{
  if (cols >= CT_SPRS_COLUMNS) {
    return (uint16)(CM_ALIGN16(cols - 4) / 4);
  } else {
    return (uint16)((cols <= 12) ? 0 : (CM_ALIGN16(cols - 12) / 4));
  }
}

void cal_gcol_cnts_for_update(Field **field, uint column_id, uint32_t *virtual_gcol_cnt)
{
  *virtual_gcol_cnt = 0;
  for (uint i = 0; i < column_id; i++) {
    Field *mysql_field = *(field + i);
    if (mysql_field->is_gcol()) {
      *virtual_gcol_cnt += 1;
    }
  }
}

longlong bit_cnvt_mysql_cantian(const uchar *ptr, Field *mysql_field)
{
  ulonglong bits = 0;
  Field_bit *field = dynamic_cast<Field_bit *>(mysql_field);
  if (field == nullptr) {
    tse_log_error("bit_cnvt_mysql_cantian failed!");
    assert(0);
  }
  uint bytes_in_rec = field->bytes_in_rec;
  if (field->bit_len) {
    bits = get_rec_bits(field->bit_ptr, field->bit_ofs, field->bit_len);
    bits <<= (bytes_in_rec * 8);
  }
  switch (bytes_in_rec) {
    case 0:
      return bits;
    case 1:
      return bits | (ulonglong)ptr[0];
    case 2:
      return bits | mi_uint2korr(ptr);
    case 3:
      return bits | mi_uint3korr(ptr);
    case 4:
      return bits | mi_uint4korr(ptr);
    case 5:
      return bits | mi_uint5korr(ptr);
    case 6:
      return bits | mi_uint6korr(ptr);
    case 7:
      return bits | mi_uint7korr(ptr);
    default:
      return mi_uint8korr(ptr + bytes_in_rec - sizeof(longlong));
  }
}

void bit_cnvt_cantian_mysql(const uchar *cantian_ptr, uchar *mysql_ptr, Field *mysql_field)
{
  ulonglong bits = 0;
  Field_bit *field = dynamic_cast<Field_bit *>(mysql_field);
  if (field == nullptr) {
    tse_log_error("bit_cnvt_cantian_mysql failed!");
    assert(0);
    return;
  }
  uint bytes_in_rec = field->bytes_in_rec;
  if (field->bit_len) {
    bits = get_rec_bits(field->bit_ptr, field->bit_ofs, field->bit_len);
    bits <<= (bytes_in_rec * 8);
  }
  switch (bytes_in_rec) {
    case 0:
      return;
    case 1:
      *(uint8 *)mysql_ptr = bits | (ulonglong)cantian_ptr[0];
      return;
    case 2:
      *(uint16 *)mysql_ptr = bits | mi_uint2korr(cantian_ptr);
      return;
    case 3:
      *(uint32 *)mysql_ptr = bits | mi_uint3korr(cantian_ptr);
      return;
    case 4:
      *(uint32 *)mysql_ptr = bits | mi_uint4korr(cantian_ptr);
      return;
    case 5:
      *(ulonglong *)mysql_ptr = bits | mi_uint5korr(cantian_ptr);
      return;
    case 6:
      *(ulonglong *)mysql_ptr = bits | mi_uint6korr(cantian_ptr);
      return;
    case 7:
      *(ulonglong *)mysql_ptr = bits | mi_uint7korr(cantian_ptr);
      return;
    default:
      *(ulonglong *)mysql_ptr = mi_uint8korr(cantian_ptr + bytes_in_rec - sizeof(longlong));
      return;
  }
}

/**
  @brief
  convert numeric data from mysql to cantian
*/
int convert_numeric_to_cantian(const field_cnvrt_aux_t *mysql_info, const uchar *mysql_ptr, uchar *cantian_ptr,
                             Field *mysql_field, uint32_t *length)
{
  int res = 0;
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_TINY:
      *(int32_t *)cantian_ptr = *(const int8_t *)mysql_ptr;
      break;
    case MYSQL_TYPE_SHORT:
      *(int32_t *)cantian_ptr = *(const int16_t *)mysql_ptr;
      break;
    case MYSQL_TYPE_INT24:
      *(int32_t *)cantian_ptr = sint3korr(mysql_ptr);
      break;
    case MYSQL_TYPE_LONG:
      *(int32_t *)cantian_ptr = *(const int32_t *)mysql_ptr;
      break;
    case MYSQL_TYPE_FLOAT:
      *(double *)cantian_ptr = *(const float *)mysql_ptr;
      break; 
    case MYSQL_TYPE_DOUBLE:
      *(double *)cantian_ptr = *(const double *)mysql_ptr;
      break;
    case MYSQL_TYPE_BIT:
      *(int64_t *)cantian_ptr = bit_cnvt_mysql_cantian(mysql_ptr, mysql_field);
      break;
    case MYSQL_TYPE_LONGLONG:
      *(int64_t *)cantian_ptr = *(const int64_t *)mysql_ptr;
      break;
    case MYSQL_TYPE_NEWDECIMAL:
      res = decimal_mysql_to_cantian(mysql_ptr, cantian_ptr, mysql_field, length);
      break;
    default:
      tse_log_error("[mysql2cantian]unsupport numeric datatype %d", mysql_info->mysql_field_type);
      assert(0);
      break;
  }
  return res;
}

/**
  @brief
  Decode year value from YEAR type which in Mysql format, and fill in other fields.
  @note
  In Mysql Format,The value of the YEAR type ranges from 1901 to 2155, and 0000.
  For the value of year in the CantianDB must range from 1 to 9999,
  the year field of MYSQL_TIME class uses 1900 instead of 0000.
  @param[out]  ltime          The variable to convert to.
  @param       mysql_field    mysql field class
*/
static void decode_mysql_year_type(MYSQL_TIME& ltime, const uchar *mysql_ptr)
{
  // 1900 is officially defined by MySQL
  ltime.year = (unsigned int)*mysql_ptr == 0 ? 0 : (unsigned int)*mysql_ptr + 1900;
  ltime.month = ltime.year == 0 ? 0 : 1;
  ltime.day = ltime.year == 0 ? 0 : 1;
  ltime.hour = 0;
  ltime.minute = 0;
  ltime.second = 0;
  ltime.second_part = 0;
  ltime.neg = false;
}

/**
  @brief
  Decode time value from TIME type which in Mysql format, and fill in other fields.
  @note In Mysql Format,The value of the TIME type ranges from -838:59:59.000000 to 838:59:59.000000.
  For the value of hour in the CantianDB must range from 0 to 23, the tse just support
  TIME type range from 00:00:00.000000 to 23:59:59.000000.
  @param[out]  ltime          The variable to convert to.
  @param       mysql_ptr      The value to convert from, mysql time data ptr.
  @param       mysql_field    mysql field class
*/
static void decode_mysql_time_type(MYSQL_TIME& ltime, const uchar *mysql_ptr, Field *mysql_field)
{
  // decode mysql data
  uint dec = mysql_field->decimals();
  longlong tmp = my_time_packed_from_binary(mysql_ptr, dec);
  TIME_from_longlong_time_packed(&ltime, tmp);
 
  // fill in other fields
  ltime.year = 1900;
  ltime.month = 1;
  ltime.day = 1;
}

/**
  @brief
  Decode date value from DATE type which in Mysql format, and fill in other fields.
  @note reverse implementation of my_date_to_binary in my_time.cc
  @param[out]  ltime          The variable to convert to.
  @param       mysql_ptr      The value to convert from, mysql date data ptr.
*/
static void decode_mysql_date_type(MYSQL_TIME& ltime, const uchar *mysql_ptr)
{
  int tmp = *mysql_ptr + (((int)*(mysql_ptr + 1)) << 8) + (((int)*(mysql_ptr + 2)) << 16);
  ltime.year = tmp / 16 / 32;
  ltime.month = tmp / 32 % 16;
  ltime.day = tmp % 32;
}

/**
  @brief
  Decode datetime value from DATETIME type which in Mysql format, and fill in other fields.
  @param[out]  ltime          The variable to convert to.
  @param       mysql_ptr      The value to convert from, mysql datetime data ptr.
  @param       mysql_field    mysql field class
*/
static void decode_mysql_datetime_type(MYSQL_TIME& ltime, const uchar *mysql_ptr, Field *mysql_field)
{
  uint dec = mysql_field->decimals();
  longlong packed = my_datetime_packed_from_binary(mysql_ptr, dec);
  TIME_from_longlong_datetime_packed(&ltime, packed);
}

/**
  @brief
  Decode timestamp value from TIMESTAMP type which in Mysql format, and fill in other fields.
  @param[out]  ltime          The variable to convert to.
  @param       mysql_ptr      The value to convert from, mysql timestamp data ptr.
  @param       mysql_field    mysql field class
*/
static void decode_mysql_timestamp_type(MYSQL_TIME& ltime, const uchar *mysql_ptr, Field *mysql_field)
{
  uint dec = mysql_field->decimals();
  struct timeval tm;
  my_timestamp_from_binary(&tm, mysql_ptr, dec);
  if (tm.tv_sec == 0) {
    return;
  }
  my_tz_UTC->gmt_sec_to_TIME(&ltime, tm);
}

/**
  @brief
  Decode all date and time Type which in Mysql format, and output to MYSQL_TIME class.
  @note In Mysql Format,The value of the TIME type ranges from -838:59:59.000000 to 838:59:59.000000.
  For the value of hour in the CantianDB must range from 0 to 23, the tse just support
  TIME type range from 00:00:00.000000 to 23:59:59.000000.
  @param[out]  ltime          The variable to convert to.
  @param       mysql_info     mysql field type info. 
  @param       mysql_ptr      The value to convert from, mysql data ptr.
  @param       mysql_field    mysql field class.
*/
void decode_mysql_datetime(MYSQL_TIME& ltime, const field_cnvrt_aux_t* mysql_info,
                                  const uchar *mysql_ptr, Field *mysql_field)
{
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_YEAR:
      decode_mysql_year_type(ltime, mysql_ptr);
      break;
    case MYSQL_TYPE_TIME:
      decode_mysql_time_type(ltime, mysql_ptr, mysql_field);
      break;
    case MYSQL_TYPE_DATE:
      decode_mysql_date_type(ltime, mysql_ptr);
      break;
    case MYSQL_TYPE_DATETIME:
      decode_mysql_datetime_type(ltime, mysql_ptr, mysql_field);
      break;
    case MYSQL_TYPE_TIMESTAMP:
      decode_mysql_timestamp_type(ltime, mysql_ptr, mysql_field);
      break;
    default:
      tse_log_error("[mysql2cantian datetime]unsupport datatype %d", mysql_info->mysql_field_type);
      assert(false);
  }
}

bool check_zero_date(const date_detail_t& date_detail)
{
  if (!(date_detail.year || date_detail.mon || date_detail.day || date_detail.hour || date_detail.min ||
        date_detail.sec || date_detail.millisec || date_detail.microsec || date_detail.nanosec)) {
    return true;
  }
  return false;
}

bool check_zero_time_ltime(const MYSQL_TIME ltime)
{
  if (!(ltime.year || ltime.month || ltime.day || ltime.hour || ltime.minute || ltime.second || ltime.second_part)) {
    return true;
  }
  return false;
}

/**
  @brief
  Before use cm_encode_date interface, check whether the date_detail_t value is correct.
*/
bool check_datetime_vaild(const date_detail_t& date_detail)
{
  if (check_zero_date(date_detail)) {
    return true;
  }

  if (!CM_IS_VALID_YEAR(date_detail.year)) {
    return false;
  }

  if (date_detail.mon < 1 || date_detail.mon > 12) {
    return false;
  }

  if ((date_detail.day < 1) || (date_detail.day > CM_MONTH_DAYS(date_detail.year, date_detail.mon))) {
    return false;
  }

  if ((date_detail.hour > 23) || (date_detail.min > 59) || (date_detail.sec > 59)) {
    return false;
  }

  if ((date_detail.millisec > 999) || (date_detail.microsec > 999)) {
    return false;
  }
  return true;
}

int assign_mysql_date_detail(enum_field_types mysql_field_type, MYSQL_TIME ltime, date_detail_t *date_detail)
{
  // Assigning Values for date_detail_t 
  if (mysql_field_type == MYSQL_TYPE_TIME) {
    ltime.year = ltime.year == 0 ? 1900 : ltime.year;
    ltime.month = ltime.month == 0 ? 1 : ltime.month;
    ltime.day = ltime.day == 0 ? 1 : ltime.day;
  }
  date_detail->day = ltime.year == 0 ? 0 : ltime.day;
  date_detail->mon = ltime.year == 0 ? 0 : ltime.month;
  date_detail->year = ltime.year;
  date_detail->sec = ltime.second;
  date_detail->min = ltime.minute;
  date_detail->hour = ltime.hour;
  date_detail->millisec = (uint16_t)(ltime.second_part / MICROSECS_PER_MILLISEC);
  date_detail->microsec = (uint16_t)(ltime.second_part % MICROSECS_PER_MILLISEC);
  date_detail->nanosec = 0;
  date_detail->tz_offset = 0;
  return 0;
}

void cnvrt_time_decimal(const uchar *src_ptr, int src_dec, uchar *des_ptr, int des_dec, uint32 length)
{
    // convert decimal for MYSQL_TYPE_TIME
    MYSQL_TIME ltime;
    uchar tmp_ptr[TSE_BYTE_8] = {0};
    longlong packed = my_time_packed_from_binary(src_ptr, src_dec);
    TIME_from_longlong_time_packed(&ltime, packed);
    // fill in other fields
    ltime.year = 1900;
    ltime.month = 1;
    ltime.day = 1;
    longlong ll = TIME_to_longlong_time_packed(ltime);
    my_time_packed_to_binary(ll, tmp_ptr, des_dec);
    memcpy(des_ptr, tmp_ptr, length);
    return;
}

void cnvrt_datetime_decimal(const uchar *src_ptr, int src_dec, uchar *des_ptr, int des_dec, uint32 length)
{
    // convert decimal for MYSQL_TYPE_DATETIME
    MYSQL_TIME ltime;
    uchar tmp_ptr[TSE_BYTE_8] = {0};
    longlong packed = my_datetime_packed_from_binary(src_ptr, src_dec);
    TIME_from_longlong_datetime_packed(&ltime, packed);
    longlong ll = TIME_to_longlong_datetime_packed(ltime);
    my_datetime_packed_to_binary(ll, tmp_ptr, des_dec);
    memcpy(des_ptr, tmp_ptr, length);
    return;
}

/**
  @brief
  convert datetime data from mysql to cantian
*/
int convert_datetime_to_cantian(const field_cnvrt_aux_t* mysql_info, uchar *cantian_ptr,
                                const uchar *mysql_ptr, Field *mysql_field)
{
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_TIME:
      cnvrt_time_decimal(mysql_ptr, mysql_field->decimals(), cantian_ptr, DATETIME_MAX_DECIMALS, TSE_BYTE_8);
      break;

    case MYSQL_TYPE_DATETIME:
      cnvrt_datetime_decimal(mysql_ptr, mysql_field->decimals(), cantian_ptr, DATETIME_MAX_DECIMALS, TSE_BYTE_8);
      break;

    case MYSQL_TYPE_DATE:
      memset(cantian_ptr, 0, TSE_BYTE_8);
      memcpy(cantian_ptr, mysql_ptr, mysql_field->pack_length());
      break;

    default:
      // decode mysql MYSQL_TYPE_YEAR and MYSQL_TYPE_TIMESTAMP from binary
      MYSQL_TIME ltime;
      memset(&ltime, 0, sizeof(MYSQL_TIME));
      decode_mysql_datetime(ltime, mysql_info, mysql_ptr, mysql_field);

      date_detail_t date_detail;
      int ret = assign_mysql_date_detail(mysql_info->mysql_field_type, ltime, &date_detail);
      if (ret != 0) {
        return ret;
      }

      // encode cantian datetime to binary
      cm_encode_date(&date_detail, (date_t *)cantian_ptr);
  }
  return 0;
}

/**
  @brief
  convert numeric data from cantian to mysql
*/
static void convert_numeric_to_mysql(const field_cnvrt_aux_t *mysql_info, uchar *mysql_ptr,
                                     uchar *cantian_ptr, Field *mysql_field)
{
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_TINY:
      *(int8_t *)mysql_ptr = *(int32_t *)cantian_ptr;
      break;
    case MYSQL_TYPE_SHORT:
      *(int16_t *)mysql_ptr = *(int32_t *)cantian_ptr;
      break;
    case MYSQL_TYPE_INT24:
      // MEDIUMINT is 3 bytes at mysql side, get 3 bytes data from cantian_ptr
      memcpy(mysql_ptr, cantian_ptr, 3 * sizeof(char));
      break;
    case MYSQL_TYPE_LONG:
      *(int32_t *)mysql_ptr = *(int32_t *)cantian_ptr;
      break;
    case MYSQL_TYPE_FLOAT:
      *(float *)mysql_ptr = *(double *)cantian_ptr;
      break; 
    case MYSQL_TYPE_DOUBLE:
      *(double *)mysql_ptr = *(double *)cantian_ptr;
      break;
    case MYSQL_TYPE_BIT:
      bit_cnvt_cantian_mysql(cantian_ptr, mysql_ptr, mysql_field);
      break;
    case MYSQL_TYPE_LONGLONG:
      *(int64_t *)mysql_ptr = *(int64_t *)cantian_ptr;
      break;
    case MYSQL_TYPE_NEWDECIMAL: 
      decimal_cantian_to_mysql(mysql_ptr, cantian_ptr, mysql_field);
      break;
    default:
      tse_log_error("[cantian2mysql]unsupport numeric datatype %d", mysql_info->mysql_field_type);
      assert(0);
      break;
  }
}
/**
  逻辑拷贝自datetime_with_no_zero_in_date_to_timeval,在mysql 8.0.28及后续版本中改函数第三个参数类型由timeval改为 my_timeval,为了兼容不同版本，这个地方函数由自己实现
  Converts a datetime in MYSQL_TIME representation to corresponding `struct
  timeval` value.

  `ltime` must be previously checked for `TIME_NO_ZERO_IN_DATE`.
  Things like '0000-01-01', '2000-00-01', '2000-01-00' are not allowed
  and asserted.

  Things like '0000-00-00 10:30:30' or '0000-00-00 00:00:00.123456'
  (i.e. empty date with non-empty time) return error.

  Zero datetime '0000-00-00 00:00:00.000000' is allowed and is mapped to
  {tv_sec=0, tv_usec=0}.

  @note In case of error, tm value is not initialized.

  @note `warnings` is not initialized to zero, so new warnings are added to the
  old ones. The caller must make sure to initialize `warnings`.

  @param[in]  ltime    Datetime value
  @param[in]  tz       Time zone to convert to.
  @param[out] tm       Timeval value
  @param[out] warnings Pointer to warnings.

  @return False on success, true on error.
*/
bool tse_datetime_with_no_zero_in_date_to_timeval(const MYSQL_TIME *ltime,
                                                         const Time_zone &tz,
                                                         struct timeval *tm,
                                                         int *warnings)
{
  if (!ltime->month) {
    /* Zero date */
    assert(!ltime->year && !ltime->day);
    if (non_zero_time(*ltime)) {
      /*
        Return error for zero date with non-zero time, e.g.:
        '0000-00-00 10:20:30' or '0000-00-00 00:00:00.123456'
      */
      *warnings |= MYSQL_TIME_WARN_TRUNCATED;
      return true;
    }
    tm->tv_sec = tm->tv_usec = 0;  // '0000-00-00 00:00:00.000000'
    return false;
  }

  bool is_in_dst_time_gap = false;
  if (!(tm->tv_sec = tz.TIME_to_gmt_sec(ltime, &is_in_dst_time_gap))) {
    /*
      Date was outside of the supported timestamp range.
      For example: '3001-01-01 00:00:00' or '1000-01-01 00:00:00'
    */
    *warnings |= MYSQL_TIME_WARN_OUT_OF_RANGE;
    return true;
  } else if (is_in_dst_time_gap) {
    /*
      Set MYSQL_TIME_WARN_INVALID_TIMESTAMP warning to indicate
      that date was fine but pointed to winter/summer time switch gap.
      In this case tm is set to the fist second after gap.
      For example: '2003-03-30 02:30:00 MSK' -> '2003-03-30 03:00:00 MSK'
    */
    *warnings |= MYSQL_TIME_WARN_INVALID_TIMESTAMP;
  }
  tm->tv_usec = ltime->second_part;
  return false;
}
/**
  逻辑拷贝自my_timestamp_to_binary,在mysql 8.0.28及后续版本中改函数第一个参数类型由timeval改为 my_timeval,为了兼容不同版本，这个地方函数由自己实现
  Convert in-memory timestamp representation to on-disk representation.

  @param        tm   The value to convert.
  @param [out]  ptr  The pointer to store the value to.
  @param        dec  Precision.
*/
static void tse_my_timestamp_to_binary(const struct timeval *tm, uchar *ptr, uint dec)
{
  assert(dec <= DATETIME_MAX_DECIMALS);
  /* Stored value must have been previously properly rounded or truncated */
  assert((tm->tv_usec %
          static_cast<int>(log_10_int[DATETIME_MAX_DECIMALS - dec])) == 0);
  mi_int4store(ptr, tm->tv_sec);
  switch (dec) {
    case 0:
    default:
      break;
    case 1:
    case 2:
      ptr[4] =
          static_cast<unsigned char>(static_cast<char>(tm->tv_usec / 10000));
      break;
    case 3:
    case 4:
      mi_int2store(ptr + 4, tm->tv_usec / 100);
      break;
      /* Impossible second precision. Fall through */
    case 5:
    case 6:
      mi_int3store(ptr + 4, tm->tv_usec);
  }
}

/**
  @brief
  convert datetime data from cantian to mysql
*/
static void convert_datetime_to_mysql(tianchi_handler_t &tch, const field_cnvrt_aux_t* mysql_info,
  uint8_t *mysql_ptr, uchar *cantian_ptr, Field *mysql_field)
{
  if (mysql_info->mysql_field_type == MYSQL_TYPE_DATE) {
    memcpy(mysql_ptr, cantian_ptr, mysql_field->pack_length());
    return;
  }
  if (mysql_info->mysql_field_type == MYSQL_TYPE_TIME) {
    cnvrt_time_decimal(cantian_ptr, DATETIME_MAX_DECIMALS, mysql_ptr, mysql_field->decimals(), mysql_field->pack_length());
    return;
  }
  if (mysql_info->mysql_field_type == MYSQL_TYPE_DATETIME && !tch.read_only_in_ct) {
    cnvrt_datetime_decimal(cantian_ptr, DATETIME_MAX_DECIMALS, mysql_ptr, mysql_field->decimals(), mysql_field->pack_length());
    return;
  }
  date_detail_t date_detail;
  date_t date = *(int64 *)cantian_ptr;

  // decode cantian data from binary to date_detail_t
  cm_decode_date(date, &date_detail);

  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_DATETIME: {
      // Assigning Values for MYSQL_TIME
      MYSQL_TIME ltime;
      ltime.day = date_detail.day;
      ltime.month = date_detail.mon;
      ltime.year = date_detail.year;
      ltime.second = date_detail.sec;
      ltime.minute = date_detail.min;
      ltime.hour = date_detail.hour;
      ltime.second_part = 0;
      ltime.neg = false;
      ltime.time_zone_displacement = 0;
      ltime.time_type = MYSQL_TIMESTAMP_DATETIME;

      uchar data[8];
      longlong ll = TIME_to_longlong_datetime_packed(ltime);
      int dec = mysql_field->decimals();
      my_datetime_packed_to_binary(ll, data, dec);

      memcpy(mysql_ptr, data, mysql_field->pack_length());
      break;
    }
    case MYSQL_TYPE_YEAR:
      *(uint8_t *)mysql_ptr = date_detail.year == 0 ? (uint8_t)0 : (uint8_t)(date_detail.year - 1900);
      break;
    case MYSQL_TYPE_TIMESTAMP: {
      // Assigning Values for MYSQL_TIME
      MYSQL_TIME ltime;
      ltime.day = date_detail.day;
      ltime.month = date_detail.mon;
      ltime.year = date_detail.year;
      ltime.second = date_detail.sec;
      ltime.minute = date_detail.min;
      ltime.hour = date_detail.hour;
      ltime.second_part = (unsigned long)(date_detail.millisec * MICROSECS_PER_MILLISEC) + date_detail.microsec;
      ltime.neg = false;
      ltime.time_zone_displacement = 0;
      ltime.time_type = MYSQL_TIMESTAMP_DATETIME_TZ;

      struct timeval tm;
      int warnings = 0;
      int dec = mysql_field->decimals();

      tse_datetime_with_no_zero_in_date_to_timeval(&ltime, *my_tz_OFFSET0, &tm, &warnings);
      // Assume that since the value was properly stored, there're no warnings
      assert(!warnings);
      tse_my_timestamp_to_binary(&tm, mysql_ptr, dec);
      break;
    }
    default:
      tse_log_error("[cantian2mysql datetime]unsupport datatype %d", mysql_info->mysql_field_type);
      assert(0);
      break;
  }
}

/**
  @brief
  calculate the length of variable data and out put the mysql ptr offset
  @param[in]  mysql_info    mysql field type info.
  @param[in]  mysql_field   mysql field class.
  @param[in]  mysql_ptr     the value to read from, mysql data ptr.
  @param[out]  data_offset  space occupied by variable-length data types
  @return                   the data length of variable-length data type
*/
static uint32_t calculate_variable_len(const field_cnvrt_aux_t* mysql_info, Field *mysql_field,
                                       const uint8_t *mysql_ptr, uint8_t& data_offset)
{
  uint32_t field_len = 0;
  // check if it's mysql variable len field
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_VARCHAR:
      data_offset = (uint8_t)mysql_field->get_length_bytes();
      if (data_offset == 1) {
        field_len = *(const uint8_t *)mysql_ptr;
      } else if (data_offset == 2) {
        field_len = read_from_2_little_endian(mysql_ptr);
      } else {
        tse_log_error("[mysql2cantian]get varchar data length error: %u", data_offset);
      }
      break;
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_BLOB: {
      Field_blob *dst_blob = down_cast<Field_blob *>(mysql_field);
      data_offset = dst_blob->row_pack_length();
      field_len = dst_blob->get_length((const uchar*)mysql_ptr);
      break;
    }
    case MYSQL_TYPE_STRING: {
      field_len = mysql_field->row_pack_length();
      // Char类型内存优化
      if (mysql_field->real_type() == MYSQL_TYPE_STRING) {
        while (field_len > 0 && mysql_ptr[field_len - 1] == mysql_field->charset()->pad_char) {
          field_len--;
        }
      }
      data_offset = 0;
      break;
    }
    case MYSQL_TYPE_NEWDECIMAL: {
      // field_len indicates the max bytes occupied by the decimal(prec,scale) column of Cantian
      int scale = mysql_field->decimals();
      Field_new_decimal *f = dynamic_cast<Field_new_decimal *>(mysql_field);
      if (f == nullptr) {
        tse_log_error("[mysql2cantian calculate length]unknow data type: %d", mysql_info->mysql_field_type);
        data_offset = 0;
        break;
      }
      int prec = f->precision;
      // the number of cells(dec4_t) used to store the decimal(prec,scale) = (Integer part + decimal part)
      int ncells = ALIGN_UP(prec - scale, 4) + ALIGN_UP(scale, 4);
      field_len = (1 + ncells) * sizeof(uint16_t);
      data_offset = 0;
      break;
    }
    default:
      tse_log_error("[mysql2cantian calculate length]unknow data type: %d", mysql_info->mysql_field_type);
      data_offset = 0;
      break;
    }
    return field_len;
}
/**
  @brief
  Padding Byte Length According the data length, and return mysql ptr offset
*/
static uint8_t padding_variable_byte(const field_cnvrt_aux_t* mysql_info,
                                     uint8_t *mysql_ptr, uint32 field_len, Field *field)
{
  uint8_t mysql_offset = 0;
  switch (mysql_info->mysql_field_type) {
    case MYSQL_TYPE_VARCHAR:
      mysql_offset = field->get_length_bytes();
      if (mysql_offset == LOWER_LENGTH_BYTES) {
        *mysql_ptr = (uint8_t)field_len;
      } else {
        *(uint16_t *)mysql_ptr = field_len;
      }
      break;
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_BLOB: {
      // MYSQL_TYPE_X_BLOB corresponding to pack_length
      // which include MYSQL_TYPE_TINY_BLOB,MYSQL_TYPE_BLOB
      // MYSQL_TYPE_MEDIUM_BLOB and MYSQL_TYPE_LONG_BLOB
      Field_blob *dst_blob = down_cast<Field_blob *>(field);
      mysql_offset = dst_blob->row_pack_length();
      dst_blob->store_length(mysql_ptr, mysql_offset, field_len);
      break;
    }
    default:
      tse_log_debug("[cantian2mysql]mysql field type: %d do not need data head", mysql_info->mysql_field_type);
      break;
  }
  return mysql_offset;
}

static void get_map_and_field_len(uint32_t &field_len, const field_cnvrt_aux_t* mysql_info,
                                                 field_offset_and_len_t *field_offset_info,
                                                 Field *field, record_buf_info_t *record_buf)
{
  switch (mysql_info->cantian_map_type) {
    case CANTIAN_COL_BITS_4:
      field_len = 4;
      break;
    case CANTIAN_COL_BITS_8:
      field_len = 8;
      break;
    case CANTIAN_COL_BITS_VAR:
      // Cantian always use 2 bytes to store the data length for variable-length data type.
      // But when the type is MYSQL_TYPE_BLOB,the field_len need to calculate by lob_locator_t obj follow.
      // The value may greater than 65535 only when the data type is LOB_DATA.
      // Therefore, uint16_t is sufficient here.
      // In addition, you do not need to worry that two bytes are insufficient because data larger than 4000 on the 
      // Cantian side is stored outside the row.
      if (mysql_info->sql_data_type != LOB_DATA) {
        uint8_t mysql_data_length_bytes = 0;
        field_len = calculate_variable_len(mysql_info, field,
                                           &record_buf->mysql_record_buf[*field_offset_info->mysql_field_offset],
                                           mysql_data_length_bytes);
        *field_offset_info->mysql_field_offset += mysql_data_length_bytes;
        if (!((mysql_info->sql_data_type == STRING_DATA) &&
              VARCHAR_AS_BLOB(field->row_pack_length()))) {
          *(uint16_t *)&record_buf
               ->cantian_record_buf[*field_offset_info->cantian_field_offset] =
              field_len;
          *field_offset_info->cantian_field_offset += sizeof(uint16_t);
        }
      }
      break;
    default:
      tse_log_error("[mysql2cantian]unknow col bits: %d", mysql_info->cantian_map_type);
      assert(0);
      break;
  }
}

static int convert_data_from_mysql_to_cantian(const field_cnvrt_aux_t* mysql_info, Field *field,
                                              record_buf_info_t *record_buf, uint16_t *serial_column_offset,
                                              field_offset_and_len_t *field_offset_info,
                                              uint32_t &field_len, tianchi_handler_t &tch, uint column_id)
{
  int res = 0;
  switch (mysql_info->sql_data_type) {
    case NUMERIC_DATA:
      res = convert_numeric_to_cantian(mysql_info, &record_buf->mysql_record_buf[*field_offset_info->mysql_field_offset],
                                       &record_buf->cantian_record_buf[*field_offset_info->cantian_field_offset],
                                       field, &field_len);
      // 如果是自增列，记录自增列在cantian_record_buf中的offset
      if (field->is_flag_set(AUTO_INCREMENT_FLAG)) {
        *serial_column_offset = *field_offset_info->cantian_field_offset;
      }

      if (field->type() == MYSQL_TYPE_NEWDECIMAL) {
        *(uint16_t *)&record_buf->cantian_record_buf[*field_offset_info->cantian_field_offset - sizeof(uint16_t)] =
        field_len;
      }

      break;
    case DATETIME_DATA:
      res = convert_datetime_to_cantian(mysql_info,
                                        &record_buf->cantian_record_buf[*field_offset_info->cantian_field_offset],
                                        &record_buf->mysql_record_buf[*field_offset_info->mysql_field_offset], field);
      break;
    case STRING_DATA:
      if (!VARCHAR_AS_BLOB(field->row_pack_length())) {
        memcpy(&record_buf
                    ->cantian_record_buf[*field_offset_info->cantian_field_offset],
               &record_buf
                    ->mysql_record_buf[*field_offset_info->mysql_field_offset],
               field_len);
      } else {
        res = convert_blob_to_cantian(
            record_buf->cantian_record_buf,
            *field_offset_info->cantian_field_offset,
            &record_buf
                 ->mysql_record_buf[*field_offset_info->mysql_field_offset],
            field_len, tch, column_id, field_len);
      }
      break;
    case LOB_DATA: {
      Field_blob *dst_blob = down_cast<Field_blob *>(field);
      uchar *blob_str = dst_blob->get_blob_data();
      uint32_t remain_size = dst_blob->get_length();
      res = convert_blob_to_cantian(record_buf->cantian_record_buf,
                                    *field_offset_info->cantian_field_offset,
                                    blob_str, remain_size, tch, column_id, field_len);
    } break;
    case UNKNOW_DATA:
    default:
      tse_log_error("[mysql2cantian]unsupport sql_data_type %d", mysql_info->sql_data_type);
      assert(0);
      break;
  }
  return res;
}

int mysql_record_to_cantian_record(const TABLE &table, record_buf_info_t *record_buf, tianchi_handler_t &tch,
                                   uint16_t *serial_column_offset, std::vector<uint16_t> *fields)
{
  *serial_column_offset = 0;
  bool is_update = fields != nullptr;
  bool has_gcol = table.has_gcol();
  uint n_fields = is_update ? fields->size() : table.s->fields;
  // Count the number of generated columns
  uint32 virtual_gcol_cnt = 0;
  if (has_gcol) {
    cal_gcol_cnts_for_update(table.field, n_fields, &virtual_gcol_cnt);
  }
  uint32 ex_maps = col_bitmap_ex_size(n_fields - virtual_gcol_cnt);
  virtual_gcol_cnt = 0;
  uint32 cantian_field_offset = sizeof(row_head_t) + ex_maps;
  row_head_t *row_head = (row_head_t *)record_buf->cantian_record_buf;
  
  for (uint column_cnt = 0; column_cnt < n_fields; column_cnt++) {
    uint column_id = is_update ? fields->at(column_cnt) : column_cnt;
    Field *field = table.field[column_id];

    if (field->is_virtual_gcol()) {
      if (is_update) {
        fields->erase(fields->begin() + column_cnt);
        column_cnt--;
        n_fields--;
      } else {
        virtual_gcol_cnt++;
      }
      continue;
    }

    // mark as not intialized
    uint8_t map = 0xFF;
    if (field->is_nullable()) {
        uint mysql_null_byte_offset = field->null_offset();
        uint mysql_null_bit_mask = field->null_bit;
        /* If the field is null */
        if (record_buf->mysql_record_buf[mysql_null_byte_offset] & mysql_null_bit_mask) {
            map = 0;
            if (is_update && has_gcol) {
              cal_gcol_cnts_for_update(table.field, column_id, &virtual_gcol_cnt);
              row_set_column_bits2(row_head, map, column_cnt);
              fields->at(column_cnt) -= virtual_gcol_cnt;
            } else {
              row_set_column_bits2(row_head, map, column_cnt - virtual_gcol_cnt);
            }
            continue;
        }
    }

    uint32_t field_len = 0;
    uint mysql_field_offset = field->offset(table.record[0]);

    // Get map and field_len , according to mysql_field type
    const field_cnvrt_aux_t* mysql_info = get_auxiliary_for_field_convert(field, field->type());
    assert(mysql_info != NULL);
    map = mysql_info->cantian_map_type;
    field_offset_and_len_t field_offset_info = {nullptr, nullptr, &cantian_field_offset, &mysql_field_offset};
    get_map_and_field_len(field_len, mysql_info, &field_offset_info, field, record_buf);

    field_offset_info = {nullptr, nullptr, &cantian_field_offset, &mysql_field_offset};
    int res = convert_data_from_mysql_to_cantian(mysql_info, field, record_buf, serial_column_offset,
                                                 &field_offset_info, field_len, tch, column_id);
    if (res != 0) {
      return res;
    }

    // set cantian bitmap for this column
    if (is_update && has_gcol) {
      cal_gcol_cnts_for_update(table.field, column_id, &virtual_gcol_cnt);
      row_set_column_bits2(row_head, map, column_cnt);
      fields->at(column_cnt) -= virtual_gcol_cnt;
    } else {
      row_set_column_bits2(row_head, map, column_cnt - virtual_gcol_cnt);
    }
    
    // Cantian align variable field (including prefix) to 4 bytes
    if (map == 3) {
      field_len += sizeof(uint16_t);
      field_len = ROUND_UP(field_len, 4);
      field_len -= sizeof(uint16_t);
    }
    // proceed to next column in cantian record
    cantian_field_offset += field_len;
  }

  *record_buf->cantian_record_buf_size = cantian_field_offset;
  row_head->size = (uint16_t)cantian_field_offset;
  row_head->column_count = is_update ? n_fields : n_fields - virtual_gcol_cnt;
  row_head->flags = 0;
  return 0;
}

void copy_column_data_to_mysql(field_info_t *field_info, const field_cnvrt_aux_t* mysql_info,
                               tianchi_handler_t &tch, bool is_index_only)
{
  if (!bitmap_is_set(field_info->field->table->read_set, field_info->field->field_index()) &&
      tch.sql_command == SQLCOM_SELECT) {
    return;
  }
  uchar *src = NULL;
  uint16_t src_len = 0;
  switch (mysql_info->sql_data_type) {
    case NUMERIC_DATA:
      convert_numeric_to_mysql(
          mysql_info, field_info->mysql_cur_field, field_info->cantian_cur_field, field_info->field);
      break;
    case DATETIME_DATA:
      convert_datetime_to_mysql(
          tch, mysql_info, field_info->mysql_cur_field, field_info->cantian_cur_field, field_info->field);
      break;
    case STRING_DATA: {
      lob_locator_t *locator = NULL;
      if (!VARCHAR_AS_BLOB(field_info->field->row_pack_length()) || tch.read_only_in_ct) {
        src = field_info->cantian_cur_field;
        src_len = field_info->field_len;
      } else {
        convert_blob_to_mysql(field_info->cantian_cur_field, field_info->field,
                              tch, field_info->mysql_cur_field);
        locator = (lob_locator_t *)(uint8 *)field_info->cantian_cur_field;
        src_len = (uint32)locator->head.size;
        assert(src_len);
        src = *(uchar **)field_info->mysql_cur_field;
      }
      memcpy(field_info->mysql_cur_field, src, src_len);
      // Char类型内存优化，mysql行数据尾部填充
      if (mysql_info->mysql_field_type == MYSQL_TYPE_STRING &&
          field_info->field->real_type() == MYSQL_TYPE_STRING &&
          field_info->field->row_pack_length() > field_info->field_len) {
        memset(field_info->mysql_cur_field + field_info->field_len, field_info->field->charset()->pad_char,
               field_info->field->row_pack_length() - field_info->field_len);
      }
      if (locator) {
        my_free(src);
        src = NULL;
      }
    } break;
    case LOB_DATA: {
      if (is_index_only) {
        uint32_t blob_len = field_info->field_len;
        char *blob_buf = (char *)my_malloc(PSI_NOT_INSTRUMENTED, blob_len * sizeof(char), MYF(MY_WME));
        memcpy(blob_buf, field_info->cantian_cur_field, blob_len);
        memcpy(field_info->mysql_cur_field, &blob_buf, sizeof(char *));
        bitmap_set_bit(field_info->field->table->read_set, field_info->field->field_index());
      } else {
        convert_blob_to_mysql(field_info->cantian_cur_field, field_info->field, tch, field_info->mysql_cur_field);
      }
    }
      break;
    case UNKNOW_DATA:
    /* fall through */
    default:
      tse_log_error("[cantian2mysql]unsupport datatype %d", mysql_info->sql_data_type);
      assert(0);
      break;
  }
}

bool parse_cantian_column_and_update_offset(Field *field, uint8 cantian_col_type, const field_cnvrt_aux_t* cnvrt_aux,
  record_buf_info_t *record_buf, field_offset_and_len_t *size, bool is_index_only)
{
  switch (cantian_col_type) {
    case CANTIAN_COL_BITS_NULL:
      // set null bit & continue to next field if current column is null
      record_buf->mysql_record_buf[field->null_offset()] |= field->null_bit;
      return true;
    case CANTIAN_COL_BITS_4:
      *size->field_len = 4;
      *size->aligned_field_len = 4;
      break;
    case CANTIAN_COL_BITS_8:
      *size->field_len = 8;
      *size->aligned_field_len = 8;
      break;
    case CANTIAN_COL_BITS_VAR:
      // all fields in cantian are 4 bytes aligned except index_only decimal
      // for variable types there're two bytes in the front
      // representing length of the following data
      // is_index_only && decimal: There are no two bytes of field_len after the row_head and bitmap.
      // is_index_only && other variable-length types: Two bytes with field_len after row_head and bitmap.
      if (!field->table->s->tmp_table && is_index_only && cnvrt_aux->mysql_field_type == MYSQL_TYPE_NEWDECIMAL) {
        uint off = *size->cantian_field_offset;
        *size->field_len = cm_dec4_stor_sz((dec4_t *)((char*)record_buf->cantian_record_buf + off));
        *size->aligned_field_len = ROUND_UP(*size->field_len, 4);
      } else {
        *size->field_len = *((uint16_t *)&record_buf->cantian_record_buf[*size->cantian_field_offset]);
        *size->cantian_field_offset += sizeof(uint16_t);
        *size->aligned_field_len = ROUND_UP(*size->field_len + sizeof(uint16_t), 4) - sizeof(uint16_t);
      }

      if (!is_index_only &&
          (cnvrt_aux->mysql_field_type == MYSQL_TYPE_BLOB ||
          cnvrt_aux->mysql_field_type == MYSQL_TYPE_JSON ||
          ((cnvrt_aux->mysql_field_type == MYSQL_TYPE_VARCHAR) &&
           VARCHAR_AS_BLOB(field->row_pack_length())))) {
        // when the type is blob,the lob data length is not field_len
        // which should decode from lob_locator_t struct, locator->head.size
        lob_locator_t *locator = (lob_locator_t *)(uint8*)&record_buf->cantian_record_buf[*size->cantian_field_offset];
        uint32 lob_len = (uint32)locator->head.size;
        *size->mysql_field_offset += padding_variable_byte(cnvrt_aux,
                                                           &record_buf->mysql_record_buf[*size->mysql_field_offset],
                                                           lob_len, field);
      } else {
        *size->mysql_field_offset += padding_variable_byte(cnvrt_aux,
                                                           &record_buf->mysql_record_buf[*size->mysql_field_offset],
                                                           *size->field_len, field);
      }
      break;
    default:
      tse_log_error("[cantian2mysql]unknow col bits: %u", cantian_col_type);
      assert(0);
      break;
  }
  return false;
}

void convert_cantian_field_to_mysql_field(Field *field, field_offset_and_col_type *filed_offset,
                                          record_buf_info_t *record_buf, tianchi_handler_t &tch, bool is_index_only)
{
    // Get auxiliary info for field convertion
    const field_cnvrt_aux_t* cnvrt_aux_info = get_auxiliary_for_field_convert(field, field->type());
    assert(cnvrt_aux_info != NULL);

    uint16_t field_len = 0;
    uint16_t aligned_field_len = 0;
    field_offset_and_len_t size = {&field_len, &aligned_field_len, filed_offset->cantian_field_offset,
                                   filed_offset->mysql_field_offset};
    if (parse_cantian_column_and_update_offset(field, filed_offset->cantian_col_type, cnvrt_aux_info, record_buf,
      &size, is_index_only)) {
      // continue to next column if current field is null
      return;
    }

    field_info_t field_info = {field, &record_buf->cantian_record_buf[*filed_offset->cantian_field_offset],
                               &record_buf->mysql_record_buf[*filed_offset->mysql_field_offset], field_len};
    copy_column_data_to_mysql(&field_info, cnvrt_aux_info, tch, is_index_only);

    // move cantian offset to next column
    *filed_offset->cantian_field_offset += aligned_field_len;
}

void cantian_record_to_mysql_record(const TABLE &table, index_info_t *index, record_buf_info_t *record_buf,
                                    tianchi_handler_t &tch)
{
  row_head_t *row_head = (row_head_t*)record_buf->cantian_record_buf;
  bool is_index_only = false;
  tse_log_debug("size %u, column cnt %u, is_changed:%hu, is_deleted:%hu, "
                "is_link:%hu, is_migr:%hu, self_chg:%hu, is_csf:%hu",
                row_head->size, row_head->column_count, row_head->is_changed,
                row_head->is_deleted, row_head->is_link, row_head->is_migr,
                row_head->self_chg, row_head->is_csf);

  if (row_head->is_csf) {
    tse_log_error("csf format is not supported yet");
    assert(0);
  }

  if (IS_SPRS_ROW(row_head)) {
    tse_log_error("sparse row not supported yet");
    assert(0);
  }

  // cantian bitmap is 3 byte by default and may be larger
  // for a table with more than 12 columns
  uint n_fields = table.s->fields;
  uint ex_maps = col_bitmap_ex_size(row_head->column_count);
  uint cantian_field_offset = sizeof(row_head_t) + ex_maps;
  uint virtual_gcol_cnt = 0;
  // update offset if it's a migrated row
  cantian_field_offset += (row_head->is_migr) ? 8 : 0;
  // initialize null bitmap
  memset(record_buf->mysql_record_buf, 0x00, table.s->null_bytes);
  for (uint column_id = 0; column_id < n_fields; column_id++) {
    // early return if current column exceeds the max column id we wanted
    if (column_id > index->max_col_idx) {
      return;
    }

    Field *field = table.field[column_id];
    if (field->is_virtual_gcol()) {
        virtual_gcol_cnt++;
        continue;
    }
    uint mysql_field_offset = field->offset(table.record[0]);
    uint8 cantian_col_type = (column_id - virtual_gcol_cnt >= row_head->column_count) ?
                              CANTIAN_COL_BITS_NULL : row_get_column_bits2((row_head_t *)record_buf->cantian_record_buf, column_id - virtual_gcol_cnt);
    
    field_offset_and_col_type filed_offset = {&cantian_field_offset, &mysql_field_offset, cantian_col_type};
    convert_cantian_field_to_mysql_field(field, &filed_offset, record_buf, tch, is_index_only);
  }
}



// @note that this cnvrt func can only be used for record fetched directly
// from index structures(i.e. circumstances like index-only)
void cantian_index_record_to_mysql_record(const TABLE &table, index_info_t *index, record_buf_info_t *record_buf,
                                          tianchi_handler_t &tch)
{
  auto index_info = table.key_info[index->active_index];
  uint n_fields = index_info.actual_key_parts;
  bool is_index_only = true;

  row_head_t *row_head = (row_head_t *)record_buf->cantian_record_buf;
  uint ex_maps = col_bitmap_ex_size(row_head->column_count);
  uint cantian_field_offset = sizeof(row_head_t) + ex_maps;

  // initialize null bitmap, the size of bitmap is decided by field num
  memset(record_buf->mysql_record_buf, 0x00, table.s->null_bytes);

  // convert and copy data by columns
  for (uint key_id = 0; key_id < n_fields; key_id++) {
    Field *field = index_info.key_part[key_id].field;
    uint mysql_field_offset = field->offset(table.record[0]);
    uint8 cantian_col_type = row_get_column_bits2((row_head_t *)record_buf->cantian_record_buf, key_id);
    field_offset_and_col_type filed_offset = {&cantian_field_offset, &mysql_field_offset, cantian_col_type};
    uint col_id = field->field_index();
    convert_cantian_field_to_mysql_field(field, &filed_offset, record_buf, tch, is_index_only);

    // early return if current column exceeds the last column id we wanted
    if (col_id == index->max_col_idx) {
      return;
    }
  }
}

int isolation_level_to_cantian(enum_tx_isolation isolation_level)
{
  switch (isolation_level) {
    case ISO_READ_UNCOMMITTED:
    case ISO_READ_COMMITTED:
      return (ISOLATION_READ_COMMITTED);
    case ISO_REPEATABLE_READ:
    case ISO_SERIALIZABLE:
      return (ISOLATION_SERIALIZABLE);
    default:
      assert(0);
      return (0);
  }
}

int get_cantian_record_length(const TABLE *table)
{
  Field *mysql_field = NULL;
  uint total_record_length = sizeof(row_head_t) + col_bitmap_ex_size(table->s->fields);
  uint16_t field_len;
  
  for (Field **field = table->field; *field; field++) {
    mysql_field = *field;
    field_len = 0;
    switch (mysql_field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
        field_len = 4;
        break;
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_TIME:
        field_len = 8;
        break;
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
        field_len = ROUND_UP(mysql_field->row_pack_length() + sizeof(uint16_t), 4);
        break;
      case MYSQL_TYPE_NEWDECIMAL: {
        int scale = mysql_field->decimals();
        Field_new_decimal *f = dynamic_cast<Field_new_decimal *>(mysql_field);
        TSE_RET_ERR_IF_NULL(f);
        int prec = f->precision; 
        int ncells = ALIGN_UP(prec - scale, 4) + ALIGN_UP(scale, 4);
        field_len = ROUND_UP((1 + ncells) * sizeof(uint16_t) + sizeof(uint16_t), 4);
        }
        break;
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_JSON:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
        // assume they're all outline-stored
        field_len = ROUND_UP(sizeof(lob_locator_t) + sizeof(uint16_t), 4);
        break;
      default:
        tse_log_error("unsupported column type %d", mysql_field->type());
        return -1;
    }
    total_record_length += field_len;
  }  // for field
  return total_record_length;
}
