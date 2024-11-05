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

#include "ha_ctc.h"
#include "my_inttypes.h"
#include <unordered_map>
#include "sql/my_decimal.h"
#include "sql/sql_time.h"

#ifndef DATATYPE_CNVRTR_H
#define DATATYPE_CNVRTR_H

#define get_rec_bits(bit_ptr, bit_ofs, bit_len)                          \
  (((((uint16)(bit_ptr)[1] << 8) | (uint16)(bit_ptr)[0]) >> (bit_ofs)) & \
   ((1 << (bit_len)) - 1))

/*
 * the internal data structure for storing timezone information.
 * the purpose of it is to save space when transforming the information via network,
 * or storing the information with TIMESTAMP WITH TIMEZONE data.
 *
 * the timezone_info_t stands for the offset (in minutes) of a timezone.
 * for instance, the content of timezone_info_t for CMT(GMT+8:00) is 480
 * while EST(GMT-5:00) is -300
 */
typedef int16 timezone_info_t;
/*
 * The date type is represented by a 64-bit signed integer. The minimum unit
 * is 1 microsecond. This indicates the precision can reach up to 6 digits after
 * the decimal point.
 */

typedef int64 date_t;
#define CTC_BYTE_8 8
#define CANTIAN_COL_BITS_NULL (uint8)0x00 // NULL in Cantian Storage
#define CANTIAN_COL_BITS_4    (uint8)0x01 // 4 Bytes in Cantian Storage
#define CANTIAN_COL_BITS_8    (uint8)0x02 // 8 Bytes in Cantian Storage
#define CANTIAN_COL_BITS_VAR  (uint8)0x03 // Variable Bytes in Cantian Storage
#define CTC_LOB_LOCATOR_BUF_SIZE (uint32)4012
#define CTC_LOB_MAX_INLINE_SIZE (CTC_LOB_LOCATOR_BUF_SIZE - offsetof(lob_locator_t, data))
#define ALIGN_UP(x, align)    ((x) / (align) + ((x) % (align) ? 1 : 0))
#define ROUND_UP(x, align)    ((x) / (align) + ((x) % (align) ? 1 : 0)) * (align)
#define MICROSECS_PER_MILLISEC  1000U
#define MILLISECS_PER_SECOND    1000U
#define LOB_DATA_SIZE_100K (1024 * 100)
#define LOB_DATA_SIZE_1M (1024 * 1024)
#define LOB_DATA_SIZE_8M (1024 * 1024 * 8)
/* LOB_DATA_BUFFER_SIZE must bigger than 4000,Otherwise, incorrect data may be written to the row. */
#define LOB_DATA_BUFFER_SIZE_8K (1024 * 8)
#define CTC_DDL_MAX_VARCHAR_COLUMN_SIZE 8000
#define VARCHAR_AS_BLOB(len) ((len) > CTC_DDL_MAX_VARCHAR_COLUMN_SIZE)

#define CM_ALL_ZERO_DATETIME ((date_t)-63113904000000000LL) /* == cm_encode_date(00-00-00 00:00:00.000000) */
longlong bit_cnvt_mysql_cantian(const uchar *ptr, Field *mysql_field);
void bit_cnvt_cantian_mysql(const uchar *cantian_ptr, uchar *mysql_ptr, Field *mysql_field);
enum enum_sql_data_types
{ 
  UNKNOW_DATA = 0,
  NUMERIC_DATA,
  DATETIME_DATA,
  STRING_DATA,
  LOB_DATA
};

typedef enum {
  ONE_BYTE = 1,
  TWO_BYTES,
  THREE_BYTES,
  FOUR_BYTES,
  EIGHT_BYTES = 8
} capacity_usage;

#pragma pack(4)
// row format
typedef struct st_row_head {
  union {
    struct {
      uint16_t
          size;  // row size, must be the first member variable in row_head_t
      uint16_t column_count : 10;  // column count
      uint16_t flags : 6;          // total flags
    };

    struct {
      uint16_t aligned1;        // aligned row size
      uint16_t aligned2 : 10;   // aligned column_count
      uint16_t is_deleted : 1;  // deleted flag
      uint16_t is_link : 1;     // link flag
      uint16_t is_migr : 1;     // migration flag
      uint16_t self_chg : 1;    // statement self changed flag for PCR
      uint16_t is_changed : 1;  // changed flag after be locked
      uint16_t is_csf : 1;      // CSF(Compact Stream Format)
    };
  };

  union {
    struct {
      uint16_t sprs_count;     // sparse column count
      uint8_t sprs_itl_id;     // sparse itl_id;
      uint8_t sprs_bitmap[1];  // sparse bitmap
    };

    struct {
      uint8_t itl_id;     // row itl_id
      uint8_t bitmap[3];  // bitmap is no used for CSF
    };
  };
} row_head_t;  // following is bitmap of column
#pragma pack()

#pragma pack(4)
/* To represent all parts of a date type */
typedef struct st_date_detail {
  uint16 year;
  uint8 mon;
  uint8 day;
  uint8 hour;
  uint8 min;
  uint8 sec;
  uint16 millisec;           /* millisecond: 0~999, 1000 millisec = 1 sec */
  uint16 microsec;           /* microsecond: 0~999, 1000 microsec = 1 millisec */
  uint16 nanosec;            /* nanosecond:  0~999, 1000 nanoseconds = 1 millisec */
  timezone_info_t tz_offset; /* time zone */
} date_detail_t;
#pragma pack()

//  lob data define
#pragma pack(4)
typedef struct st_lob_head {
  /* size + type must be defined first!!! */
  uint32 size;
  uint32 type;
  uint32 is_outline : 1;
  uint32 unused : 31;
} lob_head_t;
typedef struct st_lob_locator {
  lob_head_t head;
  union {
      uint8 pading[32];
      uint8 data[0];
  };
} lob_locator_t;
#pragma pack()

typedef struct st_lob_text {
  char *str;
  unsigned int len;
} lob_text_t;

typedef enum en_isolation_level {
  ISOLATION_READ_COMMITTED = 1,  // read committed isolation level(default)
  ISOLATION_SERIALIZABLE = 3,    // serializable isolation level
  // value 2 is internal isolation level of daac, ignore for now
} isolation_level_t;

typedef struct st_mysql_to_cantian_field_convert {
  enum_field_types mysql_field_type;  
  uint8_t cantian_map_type;                 
  enum_sql_data_types  sql_data_type;
  enum_ctc_ddl_field_types ddl_field_type;
} field_cnvrt_aux_t;

typedef struct {
  Field *field;
  uchar *cantian_cur_field;
  uchar *mysql_cur_field; 
  uint16_t field_len;
} field_info_t;

typedef struct {
  uint16_t *field_len;
  uint16_t *aligned_field_len; 
  uint *cantian_field_offset;
  uint *mysql_field_offset;
} field_offset_and_len_t;

typedef struct {
  uchar *cantian_record_buf;
  uchar *mysql_record_buf;
  int *cantian_record_buf_size;
} record_buf_info_t;

typedef struct {
  uint active_index;
  uint max_col_idx = UINT_MAX;
} index_info_t;

typedef struct {
  uint *cantian_field_offset;
  uint *mysql_field_offset;
  uint8 cantian_col_type;
} field_offset_and_col_type;

static std::unordered_map<uint32_t, uint32_t> g_blob_field_buffer_size_map =
{
  {LOB_DATA_SIZE_100K, LOB_DATA_BUFFER_SIZE_8K},
  {LOB_DATA_SIZE_1M, LOB_DATA_SIZE_100K},
  {LOB_DATA_SIZE_8M, LOB_DATA_SIZE_1M},
};

typedef void (*cnvrt_to_mysql_fn)(const TABLE &table, index_info_t *index, record_buf_info_t *record_buf,
                                  ctc_handler_t &tch, record_info_t *record_info);

void cantian_record_to_mysql_record(const TABLE &table, index_info_t *index, record_buf_info_t *record_buf,
                                    ctc_handler_t &tch, record_info_t *record_info);
int mysql_record_to_cantian_record(const TABLE &table, record_buf_info_t *record_buf,
                                   ctc_handler_t &tch, uint16_t *serial_column_offset,
                                   std::vector<uint16_t> *fields=nullptr);
void cal_gcol_cnts_for_update(Field **field, uint column_id, uint32_t *virtual_gcol_cnt);
void cantian_index_record_to_mysql_record(const TABLE &table, index_info_t *index, record_buf_info_t *record_buf,
                                          ctc_handler_t &tch, record_info_t *record_info);
int isolation_level_to_cantian(
    enum_tx_isolation isolation_level);

int get_cantian_record_length(const TABLE *table);

const field_cnvrt_aux_t* get_auxiliary_for_field_convert(Field *field, enum_field_types mysql_field_type);

bool check_datetime_vaild(const date_detail_t& date_detail);

void decode_mysql_datetime(MYSQL_TIME& ltime, const field_cnvrt_aux_t* mysql_info,
                                  const uchar *mysql_ptr, Field *mysql_field);

int convert_datetime_to_cantian(const field_cnvrt_aux_t* mysql_info , uchar *cantian_ptr,
                                const uchar *mysql_ptr, Field *mysql_field);
int decimal_mysql_to_cantian(const uint8_t *mysql_ptr, uchar *cantian_ptr, Field *mysql_field, uint32 *length);
int convert_numeric_to_cantian(const field_cnvrt_aux_t *mysql_info, const uchar *mysql_ptr, uchar *cantian_ptr,
                               Field *mysql_field, uint32_t *length);
void cm_encode_date(const date_detail_t *detail, date_t *date);
int assign_mysql_date_detail(enum_field_types mysql_field_type, MYSQL_TIME ltime, date_detail_t *date_detail);

bool ctc_datetime_with_no_zero_in_date_to_timeval(const MYSQL_TIME *ltime, const Time_zone &tz,
                                                  struct timeval *tm, int *warnings);
bool check_zero_date(const date_detail_t& datetime);
bool check_zero_time_ltime(const MYSQL_TIME ltime);

void cnvrt_time_decimal(const uchar *src_ptr, int src_dec, uchar *des_ptr, int des_dec, uint32 length);
void cnvrt_datetime_decimal(const uchar *src_ptr, int src_dec, uchar *des_ptr, int des_dec, uint32 length);
int decimal_mysql_to_double(const uint8_t *mysql_ptr, uchar *double_ptr, Field *mysql_field);
#endif
