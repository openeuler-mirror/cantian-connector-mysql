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

#ifndef __CTC_CBO_H__
#define __CTC_CBO_H__

#include "ctc_srv.h"
#include "sql/table.h"
#include "sql/dd/types/table.h"
#include "srv_mq_msg.h"

#define REAL_EPSINON 0.00001

typedef enum en_ctc_compare_type {
    GREAT = 0,
    EQUAL,
    LESS,
    UNCOMPARABLE
} compare_type;

typedef enum en_ctc_query_type {
    QUERY_TYPE_NULL = 0,
    QUERY_TYPE_NOT_NULL,
    QUERY_TYPE_EQUAL
} query_type;

typedef enum en_ctc_cmp_type {
    CMP_TYPE_NULL = 0,
    CMP_TYPE_OPEN_INTERNAL,
    CMP_TYPE_CLOSE_INTERNAL
} ctc_cmp_type_t;

typedef struct {
    const uchar *key;
    uint len;
    ctc_cmp_type_t cmp_type;
    uint64_t col_map;
} ctc_key;

typedef struct {
    ctc_key *min_key;
    ctc_key *max_key;
} ctc_range_key;

typedef struct {
    ctc_cmp_type_t min_type;
    ctc_cmp_type_t max_type;
    cache_variant_t *max_key_val;
    cache_variant_t *min_key_val;
} field_stats_val;

typedef struct {
    uint32_t part_id;
    uint32_t subpart_id;
    uint32_t part_num;
    uint32_t subpart_num;
} part_info_t;

double calc_density_one_table(uint16_t idx_id, ctc_range_key *key,
                              ctc_cbo_stats_table_t *cbo_stats, const TABLE &table);
void calc_accumulate_gcol_num(uint num_fields, Field** field, uint32_t *acc_gcol_num);
void ctc_index_stats_update(TABLE *table, ctc_cbo_stats_t *cbo_stats);
#endif
