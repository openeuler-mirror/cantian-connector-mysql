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

void r_key2variant(tse_key *rKey, KEY_PART_INFO *cur_index_part, cache_variant_t *ret_val, cache_variant_t * value, uint32_t key_offset)
{
  ret_val->is_null = 0;
  if (rKey->cmp_type == CMP_TYPE_NULL) {
    *ret_val = *value;
    rKey->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
    return;
  }

  enum_field_types field_type = cur_index_part->field->real_type();
  ret_val->type = field_type;
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

  switch(field_type) {
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
    default:
      ret_val->is_null = 1;
      break;
  }
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
      compare_value = (right->v_real - left->v_real);
      break;
    case MYSQL_TYPE_LONGLONG:
      compare_value =  (right->v_bigint - left->v_bigint);
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
  if (popular_count > 1) {
    return (double)popular_count / col_stat->num_buckets;
  }
  return col_stat->density;
}

static double calc_equal_null_density(tse_cbo_stats_table_t cbo_stats, uint32 col_id, bool is_null)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats.columns[col_id];
  double density = (double)col_stat->num_null / cbo_stats.estimate_rows;
  return is_null ? density : (double)1 - density;
}

double calc_hist_equal_density(tse_cbo_stats_table_t cbo_stats, cache_variant_t *val,
                               uint32 col_id, enum_field_types field_type)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats.columns[col_id];
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

static double calc_hist_between_frequency(tse_cbo_stats_table_t cbo_stats, field_stats_val stats_val, enum_field_types field_type, uint32 col_id)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats.columns[col_id];
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

  if (stats_val.min_type == CMP_TYPE_CLOSE_INTERNAL) {
    *percent += calc_balance_hist_equal_density(col_stat, stats_val.min_key_val, field_type);
  }

  for (i = lo_pos; i < hist_count; i++) {
    cmp_result = compare(&hist_infos[i].ep_value, stats_val.max_key_val, field_type);

    if (cmp_result == GREAT || cmp_result == EQUAL) {
      hi_pos = i;
      break;
    }
  }

  if (stats_val.max_type == CMP_TYPE_CLOSE_INTERNAL) {
    *percent += calc_balance_hist_equal_density(col_stat, stats_val.max_key_val, field_type);
  }

  return hi_pos - lo_pos;
}

static double calc_hist_between_balance(tse_cbo_stats_table_t cbo_stats, field_stats_val stats_val, enum_field_types field_type, uint32 col_id)
{
  tse_cbo_stats_column_t *col_stat = &cbo_stats.columns[col_id];
  double density = col_stat->density;
  uint32 hist_count = col_stat->hist_count;
  if (hist_count == 0) {
      return density;
  }
  double percent = 0;

  int bucket_range = calc_hist_range_boundary(stats_val, field_type, col_stat, &percent);

  density = (double)bucket_range / col_stat->num_buckets + percent;
  return density;
}

static double calc_hist_between_density(tse_cbo_stats_table_t cbo_stats,
                                        uint32 col_id, enum_field_types field_type, field_stats_val stats_val)
{
  double density;
  tse_cbo_stats_column_t *col_stat = &cbo_stats.columns[col_id];
  if (col_stat->hist_type == FREQUENCY_HIST) {
    // HISTOGRAM_FREQUENCY
    density = calc_hist_between_frequency(cbo_stats, stats_val, field_type, col_id);
  } else {
    // HISTOGRAM_BALANCE
    density = calc_hist_between_balance(cbo_stats, stats_val, field_type, col_id);
  }
  return density;
}

double calc_density_by_cond(tse_cbo_stats_table_t cbo_stats, KEY_PART_INFO cur_index_part, tse_range_key *key,
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

  low_val = &cbo_stats.columns[col_id].low_value;
  high_val = &cbo_stats.columns[col_id].high_value;

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
                              tse_cbo_stats_table_t cbo_stats, const TABLE &table)
{
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

      if (cbo_stats.columns[col_id].total_rows == 0) { //空表
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
  rec_per_key_t rec_per_key;
  bool is_part_table = (cbo_stats->tse_cbo_stats_part_table != nullptr);
  uint32_t estimate_rows = 0;

  KEY sk;
  uint32_t *n_diff;
  if (is_part_table) {
    // set to the biggest part
    assert(cbo_stats->part_cnt);
    for (uint32 part_id = 0; part_id<cbo_stats->part_cnt; part_id++) {
      if (estimate_rows < cbo_stats->tse_cbo_stats_part_table[part_id].estimate_rows) {
        estimate_rows = cbo_stats->tse_cbo_stats_part_table[part_id].estimate_rows;
        n_diff = cbo_stats->tse_cbo_stats_part_table[part_id].ndv_keys;
      }
    }
  } else {
    n_diff = cbo_stats->tse_cbo_stats_table.ndv_keys;
    estimate_rows = cbo_stats->tse_cbo_stats_table.estimate_rows;
  }
  for (uint32 i = 0; i < table->s->keys; i++){
    sk = table->key_info[i];
    if (*(n_diff+i) == 0) {
      rec_per_key = static_cast<rec_per_key_t>(estimate_rows);
    } else {
      rec_per_key = static_cast<rec_per_key_t>(estimate_rows / *(n_diff+i));
    }
    if (rec_per_key < 1.0) {
      rec_per_key = 1.0;
    }
    for (uint32 j=0;j<sk.actual_key_parts;j++) {
      sk.set_records_per_key(j, rec_per_key);
    }
  }
}