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

void r_key2variant(tse_range_key *rKey, KEY_PART_INFO *cur_index_part, cache_variant_t *ret_val) {
  enum_field_types field_type = cur_index_part->field->real_type();

  uint16_t offset = 0;
  if (cur_index_part->field->is_nullable()) {
    offset = 1;
  }

  ret_val->is_null = 0;
  ret_val->type = field_type;

  switch(field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
        ret_val->v_int = *(int32_t *)const_cast<char *>(rKey->key + offset);
        break;
    case MYSQL_TYPE_FLOAT:
      ret_val->v_real = *(float *)const_cast<char *>(rKey->key + offset);
      break; 
    case MYSQL_TYPE_DOUBLE:
      ret_val->v_real = *(double *)const_cast<char *>(rKey->key + offset);
      break;
    case MYSQL_TYPE_LONGLONG:
      ret_val->v_bigint = *(int64_t *)const_cast<char *>(rKey->key + offset);
      break;
    default:
      ret_val->is_null = 1;
      break;
  }
}

double eval_density_result(cache_variant_t *result)
{
  /*
    * key range is beyond the actual index range, 
    * don't have any records in this range
    */
  if (result->v_real < 0) {
      return 0;
  }
  /* 
    * key range is larger than the actual index range, 
    * any key with this range shoule be deemed as not selective 
    */
  if (result->v_real > 1) {
      return 1;
  }
  return result->v_real;
}

int calc_density_low(KEY_PART_INFO *cur_index_part, cache_variant_t *high_val, cache_variant_t *low_val,
                     cache_variant_t *left_val, cache_variant_t *right_val, cache_variant_t *result)
{
    double density = DEFAULT_RANGE_DENSITY;
    if (low_val->is_null == 1 || right_val->is_null == 1) {
      return ERR_SQL_SYNTAX_ERROR;
    }

    enum_field_types field_type = cur_index_part->field->real_type();

    double numerator, denominator;
    switch(field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
        numerator = (int64_t)left_val->v_int - (int64_t)right_val->v_int;
        denominator = (int64_t)high_val->v_int - (int64_t)low_val->v_int;
        density = (double)numerator/(double)denominator;
        break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
        numerator = left_val->v_real - right_val->v_real;
        denominator = high_val->v_real - low_val->v_real;
        density = (double)numerator/(double)denominator;
      break; 
    case MYSQL_TYPE_LONGLONG:
        numerator = (int64_t)left_val->v_bigint - (int64_t)right_val->v_bigint;
        denominator = (int64_t)high_val->v_bigint - (int64_t)low_val->v_bigint;
        density = (double)numerator/(double)denominator;
      break;
    default:
        break;
  }

    result->v_real = density;

    return CT_SUCCESS;
}

double calc_density_by_cond(tianchi_cbo_stats_t *cbo_stats, KEY *cur_index, 
                            tse_range_key *tse_min_key, tse_range_key *tse_max_key, part_info_t part_info) {
  double default_density = DEFAULT_RANGE_DENSITY;
  KEY_PART_INFO cur_index_part = cur_index->key_part[0];
  uint32 col_id = cur_index_part.field->field_index();
  uint32_t part_id = part_info.part_id;
  uint32_t subpart_id = part_info.subpart_id;
  uint32_t total_part_count = IS_TSE_PART(subpart_id) ? part_info.part_num * part_info.subpart_num : part_info.part_num;

  if (!IS_TSE_PART(part_id) && cbo_stats->tse_cbo_stats_table.num_distincts[col_id] == 0) {
    return DEFAULT_RANGE_DENSITY;
  }

  uint32 index_no = (IS_TSE_PART(part_id) & IS_TSE_PART(subpart_id)) ?
                      (total_part_count * col_id) + (part_id * part_info.subpart_num + subpart_id) :
                      total_part_count * col_id + part_id;

  if (IS_TSE_PART(part_id) && cbo_stats->tse_cbo_stats_table.part_table_num_distincts[index_no] == 0) {
    return DEFAULT_RANGE_DENSITY;
  }

  cache_variant_t *low_val, *high_val;
  cache_variant_t result;

  if (IS_TSE_PART(part_id)) {
    low_val = &(cbo_stats->tse_cbo_stats_table.part_table_low_values[index_no]);
    high_val = &(cbo_stats->tse_cbo_stats_table.part_table_high_values[index_no]);
  } else {
    low_val = cbo_stats->tse_cbo_stats_table.low_values + col_id;
    high_val = cbo_stats->tse_cbo_stats_table.high_values + col_id;
  }

  if (tse_min_key->cmp_type == CMP_TYPE_GREAT && tse_max_key->cmp_type == CMP_TYPE_LESS) {
      cache_variant_t min_key_val, max_key_val;
      r_key2variant(tse_min_key, &cur_index_part,  &min_key_val);
      r_key2variant(tse_max_key, &cur_index_part,  &max_key_val);

      if (calc_density_low(&cur_index_part, high_val, low_val, &max_key_val, &min_key_val, &result) != CT_SUCCESS) {
          return default_density;
      }
      return eval_density_result(&result);
  }

  if (tse_min_key->cmp_type == CMP_TYPE_GREAT) {
      cache_variant_t min_key_val;
      r_key2variant(tse_min_key, &cur_index_part,  &min_key_val);

      if (calc_density_low(&cur_index_part, high_val, low_val, high_val, &min_key_val, &result) != CT_SUCCESS) {
          return default_density;
      }
      return eval_density_result(&result);
  }

  if (tse_max_key->cmp_type == CMP_TYPE_LESS) {
      cache_variant_t max_key_val;
      r_key2variant(tse_max_key, &cur_index_part,  &max_key_val);

      if (calc_density_low(&cur_index_part, high_val, low_val, &max_key_val, low_val, &result) != CT_SUCCESS) {
          return default_density;
      }
      return eval_density_result(&result);
  }

  return default_density;
}

double calc_density_one_table(uint16_t idx_id, tse_range_key *min_key, tse_range_key *max_key, 
                              part_info_t part_info, tianchi_cbo_stats_t *cbo_stats, const TABLE &table)
{
    double density = DEFAULT_RANGE_DENSITY;
    if (!cbo_stats->is_updated) {
        tse_log_debug("table %s has not been analyzed", table.alias);
        return density;
    }
    uint32_t part_id = part_info.part_id;
    uint32_t subpart_id = part_info.subpart_id;
    uint32_t total_part_count = IS_TSE_PART(subpart_id) ? part_info.part_num * part_info.subpart_num : part_info.part_num;

    uint32 col_id;
    if (min_key->cmp_type == CMP_TYPE_EQUAL) {
        double col_product = 1.0;
        uint64_t col_map = min_key->col_map;
        KEY cur_index = table.key_info[idx_id];
        /*
        * For all columns in used index,
        * density = 1.0 / (column[0]->num_distinct * ... * column[n]->num_distinct)
        */
        for (uint32_t idx_col_num = 0; idx_col_num < cur_index.actual_key_parts; idx_col_num++) {
            if (col_map & ((uint64_t)1 << idx_col_num)) {
                KEY_PART_INFO cur_index_part = cur_index.key_part[idx_col_num];
                col_id = cur_index_part.field->field_index();
                if (!IS_TSE_PART(part_id) && cbo_stats->tse_cbo_stats_table.num_distincts[col_id] != 0) {
                  col_product = col_product * cbo_stats->tse_cbo_stats_table.num_distincts[col_id];
                }

                uint32 index_no = (IS_TSE_PART(part_id) & IS_TSE_PART(subpart_id)) ? 
                                  (total_part_count * col_id) + (part_id * part_info.subpart_num + subpart_id) :
                                  total_part_count * col_id + part_id;

                if (IS_TSE_PART(part_id) && cbo_stats->tse_cbo_stats_table.part_table_num_distincts[index_no] != 0) {
                  col_product = col_product * cbo_stats->tse_cbo_stats_table.part_table_num_distincts[index_no];
                }
            }
        }
        density = 1.0 / col_product;
    } else {
        KEY cur_index = table.key_info[idx_id];
        density = calc_density_by_cond(cbo_stats, &cur_index, min_key, max_key, part_info);
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