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

#ifndef __TSE_CBO_H__
#define __TSE_CBO_H__

#include "tse_srv.h"
#include "sql/table.h"
#include "sql/dd/types/table.h"
#include "srv_mq_msg.h"


typedef enum en_tse_cmp_type {
    CMP_TYPE_UNKNOWN = 0,
    CMP_TYPE_EQUAL,
    CMP_TYPE_GREAT,
    CMP_TYPE_LESS
} tse_cmp_type_t;
 
/* range key type */
typedef struct {
    const char *key;
    uint len;
    tse_cmp_type_t cmp_type;
    uint64_t col_map;
} tse_range_key;

typedef struct {
    uint32_t part_id;
    uint32_t subpart_id;
    uint32_t part_num;
    uint32_t subpart_num;
} part_info_t;

double calc_density_one_table(uint16_t idx_id, tse_range_key *min_key, tse_range_key *max_key, 
                              part_info_t part_info, tianchi_cbo_stats_t *cbo_stats, const TABLE &table);

#endif
