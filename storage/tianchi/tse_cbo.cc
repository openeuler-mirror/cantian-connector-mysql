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
#include "tse_cbo.h"
#include "ha_tse.h"
#include "tse_log.h"
#include "sql/field.h"
#include "tse_srv_mq_module.h"
#include "datatype_cnvrtr.h"
#include "tse_util.h"

void r_key2variant(tse_key *rKey, KEY_PART_INFO *cur_index_part, cache_variant_t *ret_val, cache_variant_t * value, uint32_t key_offset)
{
  if (rKey->cmp_type == CMP_TYPE_NULL) {
    *ret_val = *value;
    rKey->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
    return;
  }

  Field *field = cur_index_part->field;
  uint32_t offset = 0;
  if (cur_index_part->field->is_nullable()) {
    /* The first byte in the field tells if this is an SQL NULL value */
    if(*(rKey->key + key_offset) == 1) {
      *ret_val = *value;
      rKey->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
      return;
    }
    offset = 1;
  }
  const uchar *key = rKey->key + key_offset + offset;
  uchar tmp_ptr[TSE_BYTE_8] = {0};
  const field_cnvrt_aux_t *mysql_info = get_auxiliary_for_field_convert(field, field->type());
  switch(field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
      ret_val->v_int = *(int32_t *)const_cast<uchar *>(key);
      break;
    case MYSQL_TYPE_FLOAT:
      ret_val->v_real = *(float *)const_cast<uchar *>(key);
      break; 
    case MYSQL_TYPE_DOUBLE:
      ret_val->v_real = *(double *)const_cast<uchar *>(key);
      break;
    case MYSQL_TYPE_LONGLONG:
      ret_val->v_bigint = *(int64_t *)const_cast<uchar *>(key);
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
      cnvrt_datetime_decimal(const_cast<uchar *>(key), cur_index_part->field->decimals(), tmp_ptr, DATETIME_MAX_DECIMALS, TSE_BYTE_8);
      ret_val->v_date = *(date_t *)tmp_ptr;
      break;
    case MYSQL_TYPE_TIME2:
      cnvrt_time_decimal(const_cast<uchar *>(key), cur_index_part->field->decimals(), tmp_ptr, DATETIME_MAX_DECIMALS, TSE_BYTE_8);
      ret_val->v_date = *(date_t *)tmp_ptr;
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      decimal_mysql_to_double(const_cast<uchar *>(key), tmp_ptr, field);
      ret_val->v_real = *(double *)tmp_ptr;
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

en_tse_compare_type compare(cache_variant_t *right, cache_variant_t *left, enum_field_types field_type)
{
  double compare_value = 0;
  switch(field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
      compare_value = right->v_int - left->v_int;
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      compare_value = (right->v_real - left->v_real);
      break;
    case MYSQL_TYPE_LONGLONG:
      compare_value =  (right->v_bigint - left->v_bigint);
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

static double calc_frequency_hist_equal_density(tse_cbo_stats_column_t *col_stat, cache_variant_t *val,
                                                enum_field_types field_type)
{
  en_tse_compare_type cmp_result;
  int64 result = 0;
  double density = col_stat->density;
  tse_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  for (uint32 i = 0; i < col_stat->hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, val, field_type);

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

static double calc_balance_hist_equal_density(tse_cbo_stats_column_t *col_stat, cache_variant_t *val,
                                              enum_field_types field_type)
{
  uint32 popular_count = 0;
  en_tse_compare_type cmp_result;
  tse_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  for (uint32 i = 0; i < col_stat->hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, val, field_type);

    if (cmp_result == EQUAL) {
        // ep_number is different from oracle, when compress balance histogram, need to change this
      popular_count++;
    } else if (cmp_result  == GREAT) {
      break;
    }
  }
  if (popular_count > 1 && col_stat->num_buckets > 0) {
    return (double)popular_count / col_stat->num_buckets;
  }
  return col_stat->density;
}

static double calc_equal_null_density(tse_cbo_stats_table_t *cbo_stats, uint32 col_id, bool is_null)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = DEFAULT_RANGE_DENSITY;
  if (cbo_stats->estimate_rows > 0) {
    density = (double)col_stat->num_null / cbo_stats->estimate_rows;
  }
  return is_null ? density : (double)1 - density;
}

double calc_hist_equal_density(tse_cbo_stats_table_t *cbo_stats, cache_variant_t *val,
                               uint32 col_id, enum_field_types field_type)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = col_stat->density;
  uint32 hist_count = col_stat->hist_count;
  if (hist_count == 0) {
    return density;
  }
  if (col_stat->hist_type == FREQUENCY_HIST) {
    // HISTOGRAM_FREQUENCY
    density = calc_frequency_hist_equal_density(col_stat, val, field_type);
  } else {
    // HISTOGRAM_BALANCE
    density = calc_balance_hist_equal_density(col_stat, val, field_type);
  }
  return density;
}

static double calc_hist_between_frequency(tse_cbo_stats_table_t *cbo_stats, field_stats_val stats_val, enum_field_types field_type, uint32 col_id)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = col_stat->density;
  uint32 hist_count = col_stat->hist_count;
  if (hist_count == 0) {
      return density;
  }

  tse_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  uint32 end_pos = hist_count - 1;
  int64 total_nums = hist_infos[end_pos].ep_number;
  int64 low_nums = 0;
  int64 high_nums = total_nums;
  en_tse_compare_type cmp_result;

  // HISTOGRAM_FREQUNCEY
  for (uint32 i = 0; i < hist_count; i++) {

    cmp_result = compare(&hist_infos[i].ep_value, stats_val.min_key_val, field_type);
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

    cmp_result = compare(&hist_infos[i].ep_value, stats_val.max_key_val, field_type);

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

double percent_in_bucket(tse_cbo_stats_column_t *col_stat, uint32 high,
                         cache_variant_t *key, enum_field_types field_type)
{
  double percent = 0.0D;
  tse_cbo_column_hist_t *hist_infos = col_stat->column_hist;
  cache_variant_t *ep_high = high >= col_stat->hist_count ? &col_stat->high_value : &hist_infos[high].ep_value;
  cache_variant_t *ep_low = high < 1 ? &col_stat->low_value : &hist_infos[high - 1].ep_value;
  double denominator;
  switch(field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
      if (ep_high->v_int - ep_low->v_int > 0) {
        percent = (double)(ep_high->v_int - key->v_int) / (ep_high->v_int - ep_low->v_int);
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
      if (ep_high->v_bigint - ep_low->v_bigint > 0) {
        percent = (double)(ep_high->v_bigint - key->v_bigint) / (ep_high->v_bigint - ep_low->v_bigint);
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
    default:
      return DEFAULT_RANGE_DENSITY;
  }

  return percent;
}

static int calc_hist_range_boundary(field_stats_val stats_val, enum_field_types field_type, tse_cbo_stats_column_t *col_stat,
                                    double *percent)
{
  en_tse_compare_type cmp_result;
  uint32 i, lo_pos, hi_pos;
  uint32 hist_count = col_stat->hist_count;
  tse_cbo_column_hist_t *hist_infos = col_stat->column_hist;

  lo_pos = hi_pos = hist_count - 1;

  for (i = 0; i < hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, stats_val.min_key_val, field_type);
    if (cmp_result == GREAT) {
      lo_pos = i;
      break;
    }
  }

  // calc the part of value below lo_pos
  *percent += percent_in_bucket(col_stat, i, stats_val.min_key_val, field_type);

  for (i = lo_pos; i < hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, stats_val.max_key_val, field_type);
    if (cmp_result == GREAT || cmp_result == EQUAL) {
      hi_pos = i;
      break;
    }
  }

  // calc the part of value below hi_pos
  *percent -= percent_in_bucket(col_stat, i, stats_val.max_key_val, field_type);

  if (col_stat->num_buckets > 0) {
    *percent = *percent / col_stat->num_buckets;
  }

  if (stats_val.min_type == CMP_TYPE_CLOSE_INTERNAL) {
    *percent += calc_balance_hist_equal_density(col_stat, stats_val.min_key_val, field_type);
  }

  if (stats_val.max_type == CMP_TYPE_CLOSE_INTERNAL) {
    *percent += calc_balance_hist_equal_density(col_stat, stats_val.max_key_val, field_type);
  }

  // return complete bucket number
  return hi_pos - lo_pos;
}

static double calc_hist_between_balance(tse_cbo_stats_table_t *cbo_stats, field_stats_val stats_val, enum_field_types field_type, uint32 col_id)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  double density = col_stat->density;
  uint32 hist_count = col_stat->hist_count;
  if (hist_count == 0) {
      return density;
  }
  double percent = 0;

  int bucket_range = calc_hist_range_boundary(stats_val, field_type, col_stat, &percent);

  if (col_stat->num_buckets > 0) {
    density = (double)bucket_range / col_stat->num_buckets + percent;
  } else {
    density = percent;
  }
  return density;
}

static double calc_hist_between_density(tse_cbo_stats_table_t *cbo_stats,
                                        uint32 col_id, enum_field_types field_type, field_stats_val stats_val)
{
  double density;
  tse_cbo_stats_column_t *col_stat = &cbo_stats->columns[col_id];
  if (col_stat->hist_type == FREQUENCY_HIST) {
    // HISTOGRAM_FREQUENCY
    density = calc_hist_between_frequency(cbo_stats, stats_val, field_type, col_id);
  } else {
    // HISTOGRAM_BALANCE
    density = calc_hist_between_balance(cbo_stats, stats_val, field_type, col_id);
  }
  return density;
}

double calc_density_by_cond(tse_cbo_stats_table_t *cbo_stats, KEY_PART_INFO cur_index_part, tse_range_key *key,
                            uint32_t key_offset)
{
  double density = DEFAULT_RANGE_DENSITY;
  uint32 col_id = cur_index_part.field->field_index();
  tse_key *min_key = key->min_key;
  tse_key *max_key = key->max_key;

  if (cur_index_part.field->is_nullable()) {
    if (*(min_key->key + key_offset) == 1 && max_key->cmp_type == CMP_TYPE_NULL) {
      return calc_equal_null_density(cbo_stats, col_id, false);
    }

    if (*(min_key->key + key_offset) == 1 && *(max_key->key + key_offset) == 1) {
      return calc_equal_null_density(cbo_stats, col_id, true);
    }
  }

  cache_variant_t *low_val;
  cache_variant_t *high_val;

  low_val = &cbo_stats->columns[col_id].low_value;
  high_val = &cbo_stats->columns[col_id].high_value;

  cache_variant_t min_key_val;
  cache_variant_t max_key_val;
  r_key2variant(min_key, &cur_index_part, &min_key_val, low_val, key_offset);
  r_key2variant(max_key, &cur_index_part, &max_key_val, high_val, key_offset);
  enum_field_types field_type = cur_index_part.field->real_type();
  if (compare(&max_key_val, low_val, field_type) == LESS || compare(&min_key_val, high_val, field_type) == GREAT) {
    return 0;
  }
  en_tse_compare_type comapare_value = compare(&max_key_val, &min_key_val, field_type);
  if (comapare_value == EQUAL && min_key->cmp_type == CMP_TYPE_CLOSE_INTERNAL &&
      max_key->cmp_type == CMP_TYPE_CLOSE_INTERNAL) {
    return calc_hist_equal_density(cbo_stats, &max_key_val, col_id, field_type);
  } else if (comapare_value == UNCOMPARABLE) {
    return DEFAULT_RANGE_DENSITY;
  } else if (comapare_value == LESS) {
    return 0;
  }

  field_stats_val stats_val = {min_key->cmp_type, max_key->cmp_type, &max_key_val, &min_key_val};
  density = calc_hist_between_density(cbo_stats, col_id, field_type, stats_val);

  return density;
}

double calc_density_one_table(uint16_t idx_id, tse_range_key *key,
                              tse_cbo_stats_table_t *cbo_stats, const TABLE &table)
{
  if (cbo_stats->estimate_rows == 0) { // no stats or empty table
    return 0;
  }
  double density = 1.0;
  uint32 col_id;
  uint32_t key_offset = 0;//列在索引中的偏移量
  uint64_t col_map = max(key->min_key->col_map, key->max_key->col_map);
  uint32_t key_len = max(key->min_key->len, key->max_key->len);
  KEY cur_index = table.key_info[idx_id];

  /*
  * For all columns in used index,
  * density = 1.0 / (column[0]->num_distinct * ... * column[n]->num_distinct)
  */
  for (uint32_t idx_col_num = 0; idx_col_num < cur_index.actual_key_parts; idx_col_num++) {
    double col_product = 1.0;
    if (col_map & ((uint64_t)1 << idx_col_num)) {
      KEY_PART_INFO cur_index_part = cur_index.key_part[idx_col_num];
      col_id = cur_index_part.field->field_index();
      uint32_t offset = cur_index_part.field->is_nullable() ? 1 : 0;//null值标记位

      if (cbo_stats->columns[col_id].total_rows == 0) { //空表
        col_product = 0;
      } else if (key_offset + offset + cur_index_part.field->key_length() == key_len) {//
        col_product = calc_density_by_cond(cbo_stats, cur_index_part, key, key_offset);
      } else if ((offset == 1) && *(key->min_key->key + key_offset) == 1) { //null值
        col_product = calc_equal_null_density(cbo_stats, col_id, true);
      } else {
        col_product = calc_density_by_cond(cbo_stats, cur_index_part, key, key_offset);//联合索引
        // col_product = calc_equal_density(part_info, QUERY_TYPE_EQUAL, cbo_stats, col_id);
        // col_product = calc_hist_equal_density(cbo_stats, &max_key_val, col_id, field_type);
      }
      col_product = eval_density_result(col_product);
      key_offset += (offset + cur_index_part.field->key_length());
      density = density * col_product;
    }
  }

  /*
  * This is a safe-guard logic since we don't handle tse call error in this method,
  * we need this to make sure that our optimizer continue to work even when we
  * miscalculated the density, and it's still prefer index read
  */
  if (density < 0.0 || density > 1.0) {
    density = PREFER_RANGE_DENSITY;
  }
  return density;
}

void tse_index_stats_update(TABLE *table, tianchi_cbo_stats_t *cbo_stats)
{
  rec_per_key_t rec_per_key = 0.0f;
  KEY sk;
  uint32_t *n_diff = cbo_stats->ndv_keys;
  uint32_t records;
  uint32_t table_part_num = cbo_stats->part_cnt == 0 ? 1 : cbo_stats->part_cnt;
  
  if (cbo_stats->records == 0) {
    return;
  }

  for (uint32 i = 0; i < table->s->keys; i++) {
    sk = table->key_info[i];
    for (uint32 j = 0; j < sk.actual_key_parts; j++) {
      bool all_n_diff_is_zero = true;
      rec_per_key = 0.0f;
      uint32 fld_idx = sk.key_part[j].field->field_index();
      for (uint32 k = 0; k < table_part_num; k++) {
        records = cbo_stats->tse_cbo_stats_table[k].estimate_rows;
        uint32 n_null = cbo_stats->tse_cbo_stats_table[k].columns[fld_idx].num_null;
        uint32 n_diff_part = *(n_diff + i * MAX_KEY_COLUMNS + j);
        do {
          if (!n_diff_part) {
            break;
          } else if (n_diff_part <= n_null) {
            rec_per_key += 1.0f;
          } else {
            rec_per_key += static_cast<rec_per_key_t>(records - n_null) / static_cast<rec_per_key_t>(n_diff_part - n_null);
          }
          all_n_diff_is_zero = false;
        } while(0);
      }

      // if all n_diff(s) values 0, take records itself as rec_per_key
      if (all_n_diff_is_zero) {
        for (uint32 k = 0; k < table_part_num; k++) {
          rec_per_key += static_cast<rec_per_key_t>(cbo_stats->tse_cbo_stats_table[k].estimate_rows);
        }
      }

      if (rec_per_key < 1.0) {
        rec_per_key = 1.0;
      }
      sk.set_records_per_key(j, rec_per_key);
    }
  }
}