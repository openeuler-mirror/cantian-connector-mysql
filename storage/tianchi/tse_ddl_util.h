/*
  Copyright (C) 2022. Huawei Technologies Co., Ltd. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under  version 2 of the GNU General Public License (GPLv2)  as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
*/

#ifndef __TSE_DDL_UTIL_H__
#define __TSE_DDL_UTIL_H__

#include "sql/table.h"
#include "sql/sql_alter.h"
#include "tse_srv.h"
#include "protobuf/tc_db.pb-c.h"
#include "ha_tse_ddl.h"

void *tse_ddl_alloc_mem(char **mem_start, char *mem_end, size_t malloc_size);
int check_tse_identifier_name(const char *in_name);
bool check_file_name_has_suffix(const char *file_name, const char *suffix);
bool check_file_name_prefix(const char *file_name);
bool check_data_file_name(const char *data_file_name);
Field *tse_get_field_by_name(TABLE *form, const char *name);
const Create_field *tse_get_create_field_by_column_name(THD *thd, const char* field_name);
tse_alter_table_drop_type tse_ddl_get_drop_type_from_mysql_type(Alter_drop::drop_type drop_type);
tse_alter_column_type tse_ddl_get_alter_column_type_from_mysql_type(const Alter_column *alt_col);
bool get_tse_key_type(const KEY *key_info, int32_t *ret_type);
bool get_tse_key_algorithm(ha_key_alg algorithm, int32_t *ret_algorithm);
bool tse_ddl_get_data_type_from_mysql_type(Field *field,
     const enum_field_types &mysql_type, int32_t *ret_type);
bool set_column_datatype(size_t set_num, TcDb__TseDDLColumnDef *column);
bool tse_is_with_default_value(Field *field);
tse_ddl_fk_rule tse_ddl_get_foreign_key_rule(enum enum_fk_option rule);

const KEY*tse_ddl_get_index_by_name(const char *index_name);
const Create_field* tse_ddl_get_column_by_name(const char *col_name);
bool tse_ddl_get_create_key_type(ha_base_keytype type, int32_t *ret_type);
bool tse_ddl_get_create_key_algorithm(ha_key_alg algorithm, int32_t *ret_algorithm);
uint16 get_prefix_index_len(const Field *field, const uint16 key_length);
//int convert_tse_part_type(partition_type mysql_part_type, uint32_t *tse_part_type);
//int convert_tse_subpart_type(partition_type mysql_subpart_type, uint32_t *tse_part_type);
#endif // __TSE_DDL_UTIL_H__