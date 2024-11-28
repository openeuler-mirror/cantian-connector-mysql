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
#include "tse_srv.h"
#include "datatype_cnvrtr.h"
#include "decimal_convert.h"

int tse_fill_index_key_info(TABLE *table, const uchar *key, uint key_len, const key_range *end_range, 
                            index_key_info_t *index_key_info, bool index_skip_scan);

int tse_convert_index_datatype(TABLE *table, index_key_info_t *index_key_info, bool has_right_key, dec4_t *data);
