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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Bon, MA 02110-1301  USA
*/
#include "ctc_cbo.h"
#include "ha_ctc.h"
#include "ctc_log.h"
#include "sql/field.h"
#include "ctc_srv_mq_module.h"
#include "datatype_cnvrtr.h"
#include "ctc_util.h"

static void convert_enum_key_to_variant(const uchar *enum_key, cache_variant_t *variant, capacity_usage cap_usage_of_enum_key)
{
  switch (cap_usage_of_enum_key) {
    case ONE_BYTE:
      variant->v_uint32 = *(uint8_t *)const_cast<uchar *>(enum_key);
      break;
    case TWO_BYTES:
      variant->v_uint32 = *(uint16_t *)const_cast<uchar *>(enum_key);
      break;
    default:
      break;
  }
}

static void convert_set_key_to_variant(const uchar *set_key, cache_variant_t *variant, capacity_usage cap_usage_of_set_key)
{
  switch (cap_usage_of_set_key) {
    case ONE_BYTE:
      variant->v_uint32 = *(uint8_t *)const_cast<uchar *>(set_key);
      break;
    case TWO_BYTES:
      variant->v_uint32 = *(uint16_t *)const_cast<uchar *>(set_key);
      break;
    case THREE_BYTES:
      variant->v_uint32 = uint3korr(set_key);
      break;
    case FOUR_BYTES:
      variant->v_uint32 = *(uint32_t *)const_cast<uchar *>(set_key);
      break;
    case EIGHT_BYTES:
      variant->v_ubigint = *(uint64_t *)const_cast<uchar *>(set_key);
      break;
    default:
      break;
  }
}

bool handle_null_and_single_boundary(ctc_key *rKey, Field *field, cache_variant_t *ret_val,
                                     cache_variant_t * value, uint32_t key_offset)
{
  bool access_boundary = true;

  if (rKey->cmp_type == CMP_TYPE_NULL) {
    *ret_val = *value;
    rKey->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
    return access_boundary;
  }

  if (field->is_nullable() && *(rKey->key + key_offset) == 1) {
    /*The curr field is nullable, and first byte value indicates that it is an SQL NULL value*/
    *ret_val = *value;
    rKey->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
    return access_boundary;
  }

  return !access_boundary;
}

template <typename T, typename U>
void convert_int_to_variant(const uchar *key, cache_variant_t *variant, bool is_unsigned) {
  if (is_unsigned) {
    variant->v_uint32 = *(U *)const_cast<uchar *>(key);
  } else {
    variant->v_int = *(T *)const_cast<uchar *>(key);
  }
}

void extract_boundary_value(ctc_key *rKey, KEY_PART_INFO *cur_index_part, cache_variant_t *ret_val,
                            cache_variant_t * value, uint32_t key_offset)
{
  Field *field = cur_index_part->field;
  
  if (handle_null_and_single_boundary(rKey, field, ret_val, value, key_offset)) {
    return;
  }
  
  uint32_t str_key_len = 0;
  uint32_t varchar_offset = 0;
  uint32_t null_offset = field->is_nullable() ? 1 : 0;
  if (field->real_type() == MYSQL_TYPE_VARCHAR) {
    str_key_len = *(uint16_t *)const_cast<uint8_t *>(rKey->key + key_offset + null_offset);
    varchar_offset = OFFSET_VARCHAR_TYPE;
  } else if (field->real_type() == MYSQL_TYPE_STRING) {
      uint32_t data_len = field->key_length();
      while (data_len > 0 && rKey->key[data_len + key_offset + null_offset - 1] == field->charset()->pad_char) {
        data_len--;
      }
      str_key_len = data_len;
  }
  uint32_t str_copy_len = str_key_len > CBO_STRING_MAX_LEN-1 ? CBO_STRING_MAX_LEN-1 : str_key_len;
  const uchar *key = rKey->key + key_offset + varchar_offset + null_offset;
  uchar tmp_ptr[CTC_BYTE_8] = {0};
  const field_cnvrt_aux_t *mysql_info = get_auxiliary_for_field_convert(field, field->type());
  bool is_unsigned = field->is_unsigned();
  switch (field->real_type()) {
    case MYSQL_TYPE_SET:
      convert_set_key_to_variant(key, ret_val, (capacity_usage)field->pack_length());
      break;
    case MYSQL_TYPE_ENUM:
      convert_enum_key_to_variant(key, ret_val, (capacity_usage)field->pack_length());
      break;
    case MYSQL_TYPE_BIT:
      ret_val->v_ubigint = bit_cnvt_mysql_cantian(key, field);
      break;
    case MYSQL_TYPE_TINY:
      convert_int_to_variant<int8_t, uint8_t>(key, ret_val, is_unsigned);
      break;
    case MYSQL_TYPE_SHORT:
      convert_int_to_variant<int16_t, uint16_t>(key, ret_val, is_unsigned);
      break;
    case MYSQL_TYPE_INT24:
      if (is_unsigned) {
        ret_val->v_uint32 = uint3korr(key);
      } else {
        ret_val->v_int = sint3korr(key);
      }
      break;
    case MYSQL_TYPE_LONG:
      convert_int_to_variant<int32_t, uint32_t>(key, ret_val, is_unsigned);
      break;
    case MYSQL_TYPE_FLOAT:
      ret_val->v_real = *(float *)const_cast<uchar *>(key);
      break;
    case MYSQL_TYPE_DOUBLE:
      ret_val->v_real = *(double *)const_cast<uchar *>(key);
      break;
    case MYSQL_TYPE_LONGLONG:
      if (is_unsigned) {
        ret_val->v_ubigint = *(uint64_t *)const_cast<uchar *>(key);
      } else {
        ret_val->v_bigint = *(int64_t *)const_cast<uchar *>(key);
      }
      break;
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_YEAR:
      convert_datetime_to_cantian(mysql_info, tmp_ptr, const_cast<uchar *>(key), field);
      ret_val->v_date = *(date_t *)tmp_ptr;
      break;
    case MYSQL_TYPE_NEWDATE:
      ret_val->v_date = *(date_t *)const_cast<uchar *>(key);
      break;
    case MYSQL_TYPE_DATETIME2:
      cnvrt_datetime_decimal(const_cast<uchar *>(key), field->decimals(), tmp_ptr, DATETIME_MAX_DECIMALS, CTC_BYTE_8);
      ret_val->v_date = *(date_t *)tmp_ptr;
      break;
    case MYSQL_TYPE_TIME2:
      cnvrt_time_decimal(const_cast<uchar *>(key), field->decimals(), tmp_ptr, DATETIME_MAX_DECIMALS, CTC_BYTE_8);
      ret_val->v_date = *(date_t *)tmp_ptr;
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      decimal_mysql_to_double(const_cast<uchar *>(key), tmp_ptr, field);
      ret_val->v_real = *(double *)tmp_ptr;
      break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      strncpy(ret_val->v_str, (const char *)key, str_copy_len);
      break;
    default:
      break;
  }
}

double date_compare(const uchar *date1, const uchar *date2)
{
  int date1_int = *date1 + (((int)*(date1 + 1)) << 8) + (((int)*(date1 + 2)) << 16);
  int date2_int = *date2 + (((int)*(date2 + 1)) << 8) + (((int)*(date2 + 2)) << 16);
  return date1_int - date2_int;
}

double time_compare(const uchar *time1, const uchar *time2)
{
  longlong time1_int = my_time_packed_from_binary(time1, DATETIME_MAX_DECIMALS);
  longlong time2_int = my_time_packed_from_binary(time2, DATETIME_MAX_DECIMALS);
  return time1_int - time2_int;
}

double datetime_compare(const uchar *datetime1, const uchar *datetime2)
{
  longlong datetime1_int = my_datetime_packed_from_binary(datetime1, DATETIME_MAX_DECIMALS);
  longlong datetime2_int = my_datetime_packed_from_binary(datetime2, DATETIME_MAX_DECIMALS);
  return datetime1_int - datetime2_int;
}

template <typename T>
static inline en_ctc_compare_type two_nums_compare(T a, T b) {
  if (a > b) { return GREAT; }
  if (a < b) { return LESS; }
  return EQUAL;
}

static en_ctc_compare_type compare(cache_variant_t *right, cache_variant_t *left, Field *field,
                                   const CHARSET_INFO *cs)
{
  double compare_value = 0;
  bool is_unsigned = field->is_unsigned();
  switch (field->real_type()) {
    case MYSQL_TYPE_SET:
      if ((capacity_usage)field->pack_length() <= FOUR_BYTES) {
        return two_nums_compare(right->v_uint32, left->v_uint32);
      }
      return two_nums_compare(right->v_ubigint, left->v_ubigint);
    case MYSQL_TYPE_ENUM:
      return two_nums_compare(right->v_uint32, left->v_uint32);
    case MYSQL_TYPE_BIT:
      return two_nums_compare(right->v_ubigint, left->v_ubigint);
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      if (is_unsigned) {
        return two_nums_compare(right->v_uint32, left->v_uint32);
      } else {
        return two_nums_compare(right->v_int, left->v_int);
      }
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      compare_value = (right->v_real - left->v_real);
      break;
    case MYSQL_TYPE_LONGLONG: 
      if (is_unsigned) {
        return two_nums_compare(right->v_ubigint, left->v_ubigint);
      } else {
        return two_nums_compare(right->v_bigint, left->v_bigint);
      }
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP2:
      compare_value = (right->v_date - left->v_date);
      break;
    case MYSQL_TYPE_NEWDATE:
      compare_value = date_compare((const uchar *)&right->v_date, (const uchar *)&left->v_date);
      break;
    case MYSQL_TYPE_DATETIME2:
      compare_value = datetime_compare((const uchar *)&(right->v_date), (const uchar *)&(left->v_date));
      break;
    case MYSQL_TYPE_TIME2:
      compare_value = time_compare((const uchar *)&(right->v_date), (const uchar *)&(left->v_date));
      break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      compare_value = cs->coll->strnncollsp(cs, (const uchar *)right->v_str, strlen(right->v_str),
                      (const uchar *)left->v_str, strlen(left->v_str));
      break;
    default:
      return UNCOMPARABLE;
  }
  if (abs(compare_value) < REAL_EPSINON) {
    return EQUAL;
  } else if (compare_value > 0) {
    return GREAT;
  }
  return LESS;
}

double eval_density_result(double density)
{
  /*
    * key range is beyond the actual index range,
    * don't have any records in this range
    */
  if (density < 0) {
    return 0;
  }
  /*
    * key range is larger than the actual index range,
    * any key with this range shoule be deemed as not selective
    */
  if (density > 1) {
    return 1;
  }
  return density;
}

static double calc_frequency_hist_equal_density(ctc_cbo_stats_column_t *col_stat, cache_variant_t *val,
                                                Field *field, const CHARSET_INFO *cs)
{
  en_ctc_compare_type cmp_result;
  int64 result = 0;
  double density = col_stat->density;
  ctc_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  for (uint32 i = 0; i < col_stat->hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, val, field, cs);

    if (cmp_result == EQUAL) {
        result = (i == 0) ? hist_infos[i].ep_number : hist_infos[i].ep_number - hist_infos[i - 1].ep_number;
        break;
    } else if (cmp_result == GREAT) {
        break;
    }
  }

  uint32 end_pos = col_stat->hist_count - 1;
  if (result > 0 && hist_infos[end_pos].ep_number > 0) {
    density = (double)result / hist_infos[end_pos].ep_number;
  } else if (result == 0) {
    density = 0;
    //calc_density_by_sample_rate(stmt, entity, &density);
  }
  return density;
}

static double calc_balance_hist_equal_density(ctc_cbo_stats_column_t *col_stat, cache_variant_t *val,
                                              Field *field, const CHARSET_INFO *cs)
{
  uint32 popular_count = 0;
  en_ctc_compare_type cmp_result;
  ctc_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  for (uint32 i = 0; i < col_stat->hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, val, field, cs);

    if (cmp_result == EQUAL) {
        // ep_number is different from oracle, when compress balance histogram, need to change this
      popular_count++;
    } else if (cmp_result  == GREAT) {
      break;
    }
  }
  if (popular_count > 1 && col_stat->hist_count > 0) {
    return (double)popular_count / col_stat->hist_count;
  }
  return col_stat->density;
}

static double calc_equal_null_density(ctc_cbo_stats_table_t *cbo_stats, uint32 col_id)
{
  ctc_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = DEFAULT_RANGE_DENSITY;
  if (cbo_stats->estimate_rows > 0) {
    density = (double)col_stat->num_null / cbo_stats->estimate_rows;
  }
  return density ;
}

double calc_hist_equal_density(ctc_cbo_stats_table_t *cbo_stats, cache_variant_t *val,
                               uint32 col_id, Field *field, const CHARSET_INFO *cs)
{
  ctc_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = col_stat->density;
  uint32 hist_count = col_stat->hist_count;
  if (hist_count == 0) {
    ctc_log_note("hist_count: 0, column id: %u", col_id);
    return density;
  }
  if (hist_count > STATS_HISTGRAM_MAX_SIZE) {
    ctc_log_error("Error hist_count: %u, column id: %u", hist_count, col_id);
    assert(0);
    return col_stat->density;
  }

  if (col_stat->hist_type == FREQUENCY_HIST) {
    // HISTOGRAM_FREQUENCY
    density = calc_frequency_hist_equal_density(col_stat, val, field, cs);
  } else {
    // HISTOGRAM_BALANCE
    density = calc_balance_hist_equal_density(col_stat, val, field, cs);
  }
  return density;
}

static double  calc_hist_between_frequency(ctc_cbo_stats_table_t *cbo_stats, field_stats_val stats_val,
                                           Field *field, uint32 col_id, const CHARSET_INFO *cs)
{
  ctc_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = col_stat->density;
  uint32 hist_count = col_stat->hist_count;
  ctc_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  uint32 end_pos = hist_count - 1;
  int64 total_nums = hist_infos[end_pos].ep_number;
  int64 low_nums = 0;
  int64 high_nums = total_nums;
  en_ctc_compare_type cmp_result;

  // HISTOGRAM_FREQUNCEY
  for (uint32 i = 0; i < hist_count; i++) {

    cmp_result = compare(&hist_infos[i].ep_value, stats_val.min_key_val, field, cs);
    if ((stats_val.min_type == CMP_TYPE_CLOSE_INTERNAL && (cmp_result == GREAT || cmp_result == EQUAL))
         || (stats_val.min_type == CMP_TYPE_OPEN_INTERNAL && cmp_result == GREAT)) {
        if (i > 0) {
            low_nums = hist_infos[i - 1].ep_number;
        }
      low_nums = total_nums - low_nums;
      break;
    }
  }

  for (uint32 i = 0; i < hist_count; i++) {

    cmp_result = compare(&hist_infos[i].ep_value, stats_val.max_key_val, field, cs);

    if ((stats_val.max_type == CMP_TYPE_OPEN_INTERNAL && (cmp_result == GREAT || cmp_result == EQUAL))
        || (stats_val.max_type == CMP_TYPE_CLOSE_INTERNAL && cmp_result == GREAT)) {
      high_nums = (i == 0) ? 0 : hist_infos[i - 1].ep_number;
      break;
    }
  }

    if (total_nums > 0) {
        return ((double)(low_nums + high_nums - total_nums) / total_nums) ;
    } else {
        return density;
    }

}

static double percent_in_bucket(ctc_cbo_stats_column_t *col_stat, uint32 high,
                         cache_variant_t *key, Field *field)
{
  double percent = 0.0D;
  ctc_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  cache_variant_t *ep_high = high >= col_stat->hist_count ? &col_stat->high_value : &hist_infos[high].ep_value;
  cache_variant_t *ep_low = high < 1 ? &col_stat->low_value : &hist_infos[high - 1].ep_value;
  double denominator;
  bool is_unsigned = field->is_unsigned();
  switch (field->real_type()) {
    case MYSQL_TYPE_SET:
      if ((capacity_usage)field->pack_length() <= FOUR_BYTES) {
        if (ep_high->v_uint32 > ep_low->v_uint32) {
          percent = (double)(ep_high->v_uint32 - key->v_uint32) / (ep_high->v_uint32 - ep_low->v_uint32);
        }
      } else {
        if (ep_high->v_ubigint > ep_low->v_ubigint) {
          percent = (double)(ep_high->v_ubigint - key->v_ubigint) / (ep_high->v_ubigint - ep_low->v_ubigint);
        }
      }
      break;
    case MYSQL_TYPE_ENUM:
      if (ep_high->v_uint32 > ep_low->v_uint32) {
        percent = (double)(ep_high->v_uint32 - key->v_uint32) / (ep_high->v_uint32 - ep_low->v_uint32);
      }
      break;
    case MYSQL_TYPE_BIT:
      if (ep_high->v_ubigint > ep_low->v_ubigint) {
        percent = (double)(ep_high->v_ubigint - key->v_ubigint) / (ep_high->v_ubigint - ep_low->v_ubigint);
      }
      break;
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      if (is_unsigned) {
        if (ep_high->v_uint32 > ep_low->v_uint32) {
          percent = (double)(ep_high->v_uint32 - key->v_uint32) / (ep_high->v_uint32 - ep_low->v_uint32);
        }
      } else {
        if (ep_high->v_int > ep_low->v_int) {
          percent = (double)(ep_high->v_int - key->v_int) / (ep_high->v_int - ep_low->v_int);
        }
      }
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      if (ep_high->v_real - ep_low->v_real > 0) {
        percent = (double)(ep_high->v_real - key->v_real) / (ep_high->v_real - ep_low->v_real);
      }
      break;
    case MYSQL_TYPE_LONGLONG:
      if (is_unsigned) {  
        if (ep_high->v_ubigint > ep_low->v_ubigint) {
          percent = (double)(ep_high->v_ubigint - key->v_ubigint) / (ep_high->v_ubigint - ep_low->v_ubigint);
        }
      } else {
        if (ep_high->v_bigint > ep_low->v_bigint) {
          percent = (double)(ep_high->v_bigint - key->v_bigint) / (ep_high->v_bigint - ep_low->v_bigint);
        }
      }
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP2:
      denominator = ep_high->v_date - ep_low->v_date;
      if (denominator > 0) {
        percent = (double)(ep_high->v_date - key->v_date) / denominator;
      }
      break;
    case MYSQL_TYPE_NEWDATE:
      denominator = date_compare((const uchar *)&ep_high->v_date, (const uchar *)&ep_low->v_date);
      if (denominator > 0) {
        percent = date_compare((const uchar *)&ep_high->v_date, (const uchar *)&key->v_date) / denominator;
      }
      break;
    case MYSQL_TYPE_DATETIME2:
      denominator = datetime_compare((const uchar *)&ep_high->v_date, (const uchar *)&ep_low->v_date);
      if (denominator > 0) {
        percent = datetime_compare((const uchar *)&ep_high->v_date, (const uchar *)&key->v_date) / denominator;
      }
      break;
    case MYSQL_TYPE_TIME2:
      denominator = time_compare((const uchar *)&ep_high->v_date, (const uchar *)&ep_low->v_date);
      if (denominator > 0) {
        percent = time_compare((const uchar *)&ep_high->v_date, (const uchar *)&key->v_date) / denominator;
      }
      break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    default:
      return DEFAULT_RANGE_DENSITY;
  }

  return percent;
}

static int calc_hist_range_boundary(field_stats_val stats_val, Field *field, ctc_cbo_stats_column_t *col_stat,
                                    double *percent, const CHARSET_INFO *cs)
{
  en_ctc_compare_type cmp_result;
  uint32 i, lo_pos, hi_pos;
  uint32 hist_count = col_stat->hist_count;
  ctc_cbo_column_hist_t *hist_infos = col_stat->column_hist;

  lo_pos = hi_pos = hist_count - 1;

  for (i = 0; i < hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, stats_val.min_key_val, field, cs);
    if (cmp_result == GREAT) {
      lo_pos = i;
      break;
    }
  }

  // calc the part of value below lo_pos
  *percent += percent_in_bucket(col_stat, i, stats_val.min_key_val, field);

  for (i = lo_pos; i < hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, stats_val.max_key_val, field, cs);
    if (cmp_result == GREAT || cmp_result == EQUAL) {
      hi_pos = i;
      break;
    }
  }

  // calc the part of value below hi_pos
  *percent -= percent_in_bucket(col_stat, i, stats_val.max_key_val, field);

  if (col_stat->hist_count > 0) {
    *percent = *percent / col_stat->hist_count;
  }

  if (stats_val.min_type == CMP_TYPE_CLOSE_INTERNAL) {
    *percent += calc_balance_hist_equal_density(col_stat, stats_val.min_key_val, field, cs);
  }

  if (stats_val.max_type == CMP_TYPE_CLOSE_INTERNAL) {
    *percent += calc_balance_hist_equal_density(col_stat, stats_val.max_key_val, field, cs);
  }

  // return complete bucket number
  return hi_pos - lo_pos;
}

static double calc_hist_between_balance(ctc_cbo_stats_table_t *cbo_stats, field_stats_val stats_val,
                                        Field *field, uint32 col_id, const CHARSET_INFO *cs)
{
  ctc_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = col_stat->density;
  double percent = 0;

  int bucket_range = calc_hist_range_boundary(stats_val, field, col_stat, &percent, cs);

  if (col_stat->hist_count > 0) {
    density = (double)bucket_range / col_stat->hist_count + percent;
  } else {
    density = percent;
  }
  return density;
}

static double calc_hist_between_density(ctc_cbo_stats_table_t *cbo_stats,
                                        uint32 col_id, Field *field, field_stats_val stats_val, const CHARSET_INFO *cs)
{
  double density;
  ctc_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  if (col_stat->hist_count == 0) {
    ctc_log_note("hist_count: 0, column id: %u", col_id);
    return col_stat->density;
  }
  if (col_stat->hist_count > STATS_HISTGRAM_MAX_SIZE) {
    ctc_log_error("Error hist_count: %u, column id: %u", col_stat->hist_count, col_id);
    assert(0);
    return col_stat->density;
  }
  if (col_stat->hist_type == FREQUENCY_HIST) {
    // HISTOGRAM_FREQUENCY
    density = calc_hist_between_frequency(cbo_stats, stats_val, field, col_id, cs);
  } else {
    // HISTOGRAM_BALANCE
    density = calc_hist_between_balance(cbo_stats, stats_val, field, col_id, cs);
  }
  return density;
}

double calc_density_by_cond(ctc_cbo_stats_table_t *cbo_stats, KEY_PART_INFO cur_index_part, ctc_range_key *key,
                            uint32_t key_offset, uint32_t col_id, const CHARSET_INFO *cs, bool *continue_calc)
{
  double density = DEFAULT_RANGE_DENSITY;
  ctc_key *min_key = key->min_key;
  ctc_key *max_key = key->max_key;

  cache_variant_t *low_val;
  cache_variant_t *high_val;

  low_val = &cbo_stats->columns[col_id].low_value;
  high_val = &cbo_stats->columns[col_id].high_value;

  cache_variant_t min_key_val = { 0 };
  cache_variant_t max_key_val = { 0 };
  enum_field_types field_type = cur_index_part.field->real_type();
  char v_str_min[CBO_STRING_MAX_LEN] = { 0 };
  char v_str_max[CBO_STRING_MAX_LEN] = { 0 };
  if (field_type == MYSQL_TYPE_VARCHAR || field_type == MYSQL_TYPE_VAR_STRING || field_type == MYSQL_TYPE_STRING) {
    min_key_val.v_str = v_str_min;
    max_key_val.v_str = v_str_max;
  }
  extract_boundary_value(min_key, &cur_index_part, &min_key_val, low_val, key_offset);
  extract_boundary_value(max_key, &cur_index_part, &max_key_val, high_val, key_offset);

  en_ctc_compare_type comapare_value = compare(&max_key_val, &min_key_val, cur_index_part.field, cs);
  if (comapare_value != EQUAL) {
    // 联合索引上下界不同时，当前列为范围查询，下一列开始不符合联合索引的最左匹配原则，无需继续计算
    *continue_calc = false;
  }
  if (compare(&max_key_val, low_val, cur_index_part.field, cs) == LESS ||
      compare(&min_key_val, high_val, cur_index_part.field, cs) == GREAT) {
    return 0;
  }

  if (comapare_value == EQUAL && min_key->cmp_type == CMP_TYPE_CLOSE_INTERNAL &&
      max_key->cmp_type == CMP_TYPE_CLOSE_INTERNAL) {
    return calc_hist_equal_density(cbo_stats, &max_key_val, col_id, cur_index_part.field, cs);
  } else if (comapare_value == UNCOMPARABLE) {
    return DEFAULT_RANGE_DENSITY;
  } else if (comapare_value == LESS) {
    return 0;
  }

  field_stats_val stats_val = {min_key->cmp_type, max_key->cmp_type, &max_key_val, &min_key_val};
  density = calc_hist_between_density(cbo_stats, col_id, cur_index_part.field, stats_val, cs);

  return density;
}

void calc_accumulate_gcol_num(uint num_fields, Field** field, uint32_t *acc_gcol_num)
{
  uint32_t count = 0;
  for (uint i = 0; i < num_fields; i++) {
    acc_gcol_num[i] = count;
    if (field[i]->is_virtual_gcol()) {
      count++;
    }
  }
}

double calculate_column_selectivity(ctc_cbo_stats_table_t *cbo_stats, ctc_range_key* key, KEY_PART_INFO *cur_index_part,
                                    uint32_t ctc_col_id,  uint32_t key_offset, bool is_nullable, const CHARSET_INFO *cs,
                                    bool *continue_calc)
{
    double col_selectivity = 0.0;

    if (is_nullable &&
        (key->min_key->key != nullptr && *(key->min_key->key + key_offset) == 1) &&
        (key->max_key->key == nullptr || key_offset >= key->max_key->len)) {
        /*when the col of min_key has null-val and max_key is not exists --> select * from table where col is not null*/
        col_selectivity = (double)1 - calc_equal_null_density(cbo_stats, ctc_col_id);
    } else if (is_nullable &&
               (key->min_key->key != nullptr && *(key->min_key->key + key_offset) == 1) &&
               (key->max_key->key != nullptr && *(key->max_key->key + key_offset) == 1)) {
        /*when the col of min_key and max_key both have null-val --> select * from table where col is null*/
        col_selectivity = calc_equal_null_density(cbo_stats, ctc_col_id);
    } else {
        col_selectivity = calc_density_by_cond(cbo_stats, *cur_index_part, key, key_offset,
                                               ctc_col_id, cs, continue_calc);
    }

    return col_selectivity;
}

void set_key_boundary_types(ctc_range_key *key, uint32_t id, ctc_cmp_type_t min_key_cmp_type, ctc_cmp_type_t max_key_cmp_type)
{
    uint64_t min_key_map = key->min_key->col_map;
    uint64_t max_key_map = key->max_key->col_map;

    uint64_t min_key_field_valid = min_key_map & ((uint64_t)1 << id);
    uint64_t max_key_field_valid = max_key_map & ((uint64_t)1 << id);
    uint64_t min_key_field_next = min_key_map & ((uint64_t)1 << (id + 1));
    uint64_t max_key_field_next = max_key_map & ((uint64_t)1 << (id + 1));

    if (min_key_field_valid && max_key_field_valid) {
        /*The upper and lower boundarys of current field both exist*/
        if (min_key_field_next || max_key_field_next) {
            /*The current field is not the last valid field, the boundary type is set as closed*/
            key->min_key->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
            key->max_key->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
            return;
        }
    }
    /*The current field is the last valid field in the condition,
    1) Field has only one boundary value (e.g., >=, <=, <, >, is not null)
    2) The upper and lower boundarys of current field both exist (e.g., column > 3 and column <= 5)*/
    key->min_key->cmp_type = min_key_cmp_type;
    key->max_key->cmp_type = max_key_cmp_type;
}

double calc_density_one_table(uint16_t idx_id, ctc_range_key *key,
                              ctc_cbo_stats_table_t *cbo_stats, const TABLE &table)
{
  if (cbo_stats->estimate_rows == 0) { // no stats or empty table
    return 0;
  }
  uint32_t acc_gcol_num[CTC_MAX_COLUMNS] = {0};
  calc_accumulate_gcol_num(table.s->fields, table.s->field, acc_gcol_num);
  KEY cur_index = table.key_info[idx_id];

  double density = 1.0;
  uint32_t mysql_col_id;
  uint32_t ctc_col_id;
  uint32_t key_offset = 0; // column offset in the index
  uint64_t valid_map = max(key->min_key->col_map, key->max_key->col_map);

  /*cmp_type of the last valid field in min_key and max_key*/
  ctc_cmp_type_t min_key_cmp_type = key->min_key->cmp_type;
  ctc_cmp_type_t max_key_cmp_type = key->max_key->cmp_type;

  /*
  * For all columns in used index,
  * density = 1.0 / (column[0]->num_distinct * ... * column[n]->num_distinct)
  */
  bool continue_calc = true;
  for (uint32_t idx_col_num = 0; idx_col_num < cur_index.actual_key_parts && continue_calc; idx_col_num++) {
    double col_selectivity = 0.0;
    if ((valid_map & ((uint64_t)1 << idx_col_num)) == 0) {
      break;
    }

    KEY_PART_INFO cur_index_part = cur_index.key_part[idx_col_num];
    if (cur_index_part.field->is_virtual_gcol()) {
      continue;
    }

    mysql_col_id = cur_index_part.field->field_index();
    ctc_col_id = mysql_col_id - acc_gcol_num[mysql_col_id];

    uint32_t null_offset = cur_index_part.field->is_nullable() ? 1 : 0;
    uint32_t varchar_offset = cur_index_part.field->real_type() == MYSQL_TYPE_VARCHAR ? OFFSET_VARCHAR_TYPE : 0;

    set_key_boundary_types(key, idx_col_num, min_key_cmp_type, max_key_cmp_type);
    col_selectivity = calculate_column_selectivity(cbo_stats, key, &cur_index_part, ctc_col_id, 
                                                   key_offset, null_offset, table.field[mysql_col_id]->charset(),
                                                   &continue_calc);
    col_selectivity = eval_density_result(col_selectivity);

    key_offset += (varchar_offset + null_offset + cur_index_part.field->key_length());
    density = density * col_selectivity;
  }

  /*
  * This is a safe-guard logic since we don't handle ctc call error in this method,
  * we need this to make sure that our optimizer continue to work even when we
  * miscalculated the density, and it's still prefer index read
  */
  if (density < 0.0 || density > 1.0) {
    density = PREFER_RANGE_DENSITY;
  }
  return density;
}

static inline bool check_index_is_contain_null(ctc_cbo_stats_t *cbo_stats, uint32_t fld_idx)
{
  uint32_t table_part_num = cbo_stats->part_cnt == 0 ? PART_NUM_OF_NORMAL_TABLE : cbo_stats->part_cnt;
  for (uint32 k = 0; k < table_part_num; k++) {
    if (cbo_stats->ctc_cbo_stats_table[k].columns[fld_idx].num_null) {
      return true;
    }
  }
  return false;
}

void ctc_index_stats_update(TABLE *table, ctc_cbo_stats_t *cbo_stats)
{
  rec_per_key_t rec_per_key = 0.0f;
  KEY sk;
  uint32_t *n_diff = cbo_stats->ndv_keys;
  uint32_t records = cbo_stats->records;
  uint32_t acc_gcol_num[CTC_MAX_COLUMNS] = {0};
  calc_accumulate_gcol_num(table->s->fields, table->s->field, acc_gcol_num);
  if (cbo_stats->records == 0) {
    return;
  }

  for (uint32 i = 0; i < table->s->keys; i++) {
    sk = table->key_info[i];
    for (uint32 j = 0; j < sk.actual_key_parts; j++) {
      rec_per_key = 0.0f;
      uint32 fld_idx = sk.key_part[j].field->field_index();
      fld_idx = fld_idx - acc_gcol_num[fld_idx];
      uint32 has_null = check_index_is_contain_null(cbo_stats, fld_idx) ? 1 : 0;
      uint32 n_diff_part = *(n_diff + i * MAX_KEY_COLUMNS + j);
      if (n_diff_part) {
        rec_per_key = static_cast<rec_per_key_t>(records) / static_cast<rec_per_key_t>(n_diff_part + has_null);
      } else {
          // if all n_diff(s) values 0, take records itself as rec_per_key
        rec_per_key = static_cast<rec_per_key_t>(records);
      }

      if (rec_per_key < 1.0) {
        rec_per_key = 1.0;
      }
      sk.set_records_per_key(j, rec_per_key);
    }
  }
}