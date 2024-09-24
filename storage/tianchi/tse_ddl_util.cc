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

#include "tse_ddl_util.h"
#include "tse_log.h"
#include "sql/sql_class.h"
#include "sql/create_field.h"
#include "sql/sql_lex.h"
#include "sql/sql_table.h"

using namespace std;

const char *ibd_suffix = ".ibd";

void *tse_ddl_alloc_mem(char **mem_start, char *mem_end,
                        size_t malloc_size) {
  assert(mem_start != NULL && *mem_start != NULL && mem_end != NULL);
  if (*mem_start + malloc_size >= mem_end) {
    tse_log_error("alloc ddl mem failed,size:%u", (uint32_t)malloc_size);
    return NULL;
  }
  void *ptr = *mem_start;
  *mem_start += malloc_size;
  return ptr;
}

int check_tse_identifier_name(const char *in_name) {
  if (in_name == nullptr) {
    return 0;
  }
  if (strlen(in_name) > TSE_IDENTIFIER_MAX_LEN) {
    my_error(ER_TOO_LONG_IDENT, MYF(0), in_name);
    return -1;
  }
  return 0;
}

bool check_file_name_has_suffix(const char *file_name, const char *suffix) {
  size_t suffix_len = strlen(suffix);
  size_t file_name_len = strlen(file_name);
  if (file_name_len <= suffix_len || (strcmp(file_name + file_name_len - suffix_len, suffix) != 0)) {
    return true;
  }
  return false;
}

bool check_file_name_prefix(const char *file_name) {
  if (file_name[0] == ' ' || file_name[0] == '/') {
    return true;
  }
  return false;
}

bool check_data_file_name(const char *data_file_name) {
  if (data_file_name == nullptr || data_file_name[0] == '\0') {
    my_printf_error(ER_WRONG_FILE_NAME, "The ADD DATAFILE filepath does not have a proper filename", MYF(0));
    return true;
  }

  // The datafile name can not start with '/' or ' '
  if (check_file_name_prefix(data_file_name)) {
      my_printf_error(ER_WRONG_FILE_NAME, "The DATAFILE location must be in a known directory.", MYF(0));
      return true;
  }

  // The datafile name must be suffixed with '.ibd'
  if (check_file_name_has_suffix(data_file_name, ibd_suffix)) {
      my_printf_error(ER_WRONG_FILE_NAME, "The ADD DATAFILE filepath must end with '%s'.", MYF(0), ibd_suffix);
      return true;
  }

  // the datafile name is too long (Maximum value: 128 byte).
  if (strlen(data_file_name) > SMALL_RECORD_SIZE) {
      my_printf_error(ER_WRONG_FILE_NAME, "The ADD DATAFILE filename is too long (Maximum value: 128 byte).", MYF(0));
      return true;
  }

  return false;
}

Field *tse_get_field_by_name(TABLE *form, const char *name) {
  for (uint32_t i = 0; i < form->s->fields; i++) {
    if (strcasecmp(form->field[i]->field_name, name) == 0) {
      return form->field[i];
    }
  }
  return nullptr;
}

const Create_field *tse_get_create_field_by_column_name(THD *thd, const char* field_name) {
  // 处理普通建表
  Alter_info *alter_info = thd->lex->alter_info;
  const Create_field *field = NULL;
  if (alter_info != nullptr) {
    List_iterator_fast<Create_field> field_it(alter_info->create_list);
    while ((field = field_it++)) {
      if(strcmp(field->field_name, field_name) == 0) {
        return field;
      }
    }

    // 处理create table as select * from table 的特殊情况
    // 此时alter_info->create_list为空，需生成Create_field
    TABLE tmp_table MY_ATTRIBUTE((unused));
    // lex execution is not started, item->field cannot be got.
    if(!thd->lex->is_exec_started()) {
      return nullptr;
    }
    mem_root_deque<Item *> *items = thd->lex->unit->get_unit_column_types();
    for (Item *item : VisibleFields(*items)) {
      if(strcmp(item->item_name.ptr(), field_name) == 0) {
        Create_field *cr_field = generate_create_field(thd, item, &tmp_table);
        if (cr_field == nullptr) {
          break;
        }
        return cr_field;
      }
    }
  }

  // 处理create table like 情况
  // 此时alter_info为空，需通过prepare_alter_table生成alter_info
  if (thd->lex->sql_command != SQLCOM_CREATE_TABLE) {
    return nullptr;
  }
  Alter_info local_alter_info(thd->mem_root);
  const dd::Table *src_table = nullptr;
  Alter_table_ctx local_alter_ctx MY_ATTRIBUTE((unused));
  TABLE_LIST *table_list = thd->lex->query_tables->next_global;
  if (table_list != nullptr && table_list->table != nullptr) {
    /* Fill HA_CREATE_INFO and Alter_info with description of source table. */
    HA_CREATE_INFO create_info;
    create_info.db_type = table_list->table->s->db_type();
    // This should be ok even if engine substitution has taken place since
    // row_type denontes the desired row_type, and a different row_type may be
    // assigned to real_row_type later.
    create_info.row_type = table_list->table->s->row_type;
    create_info.init_create_options_from_share(table_list->table->s, create_info.used_fields);

    // Prepare Create_field and Key_spec objects for ALTER and upgrade.
    prepare_fields_and_keys(thd, src_table, table_list->table, &create_info, &local_alter_info,
                            &local_alter_ctx, create_info.used_fields);
    List_iterator_fast<Create_field> field_like_it(local_alter_info.create_list);
    while ((field = field_like_it++)) {
      if(strcmp(field->field_name, field_name) == 0) {
        return field;
      }
    }
  }
  
  return nullptr;
}

tse_alter_table_drop_type tse_ddl_get_drop_type_from_mysql_type(
    Alter_drop::drop_type drop_type) {
  switch (drop_type) {
    case Alter_drop::drop_type::KEY:
      return TSE_ALTER_TABLE_DROP_KEY;
    case Alter_drop::drop_type::COLUMN:
      return TSE_ALTER_TABLE_DROP_COLUMN;
    case Alter_drop::drop_type::FOREIGN_KEY:
      return TSE_ALTER_TABLE_DROP_FOREIGN_KEY;
    case Alter_drop::drop_type::CHECK_CONSTRAINT:
      return TSE_ALTER_TABLE_DROP_CHECK_CONSTRAINT;
    case Alter_drop::drop_type::ANY_CONSTRAINT:
      return TSE_ALTER_TABLE_DROP_ANY_CONSTRAINT;
    default:
      return TSE_ALTER_TABLE_DROP_UNKNOW;
  }
}

tse_alter_column_type tse_ddl_get_alter_column_type_from_mysql_type(
    Alter_column::Type alter_column_type) {
  switch (alter_column_type) {// SET_DEFAULT, DROP_DEFAULT, RENAME_COLUMN
    case Alter_column::Type::SET_DEFAULT:
      return TSE_ALTER_COLUMN_SET_DEFAULT;
    case Alter_column::Type::DROP_DEFAULT:
      return TSE_ALTER_COLUMN_DROP_DEFAULT;
    case Alter_column::Type::RENAME_COLUMN:
      return TSE_ALTER_COLUMN_RENAME_COLUMN;
    case Alter_column::Type::SET_COLUMN_VISIBLE:
      return TSE_ALTER_COLUMN_SET_COLUMN_VISIBLE;
    case Alter_column::Type::SET_COLUMN_INVISIBLE:
      return TSE_ALTER_COLUMN_SET_COLUMN_INVISIBLE;
    default:
      return TSE_ALTER_COLUMN_UNKNOW;
  }
}

bool get_tse_key_type(const KEY *key_info, int32_t *ret_type) {
  *ret_type = TSE_KEYTYPE_UNKNOW;
  if (key_info->flags & HA_SPATIAL) {
    *ret_type = TSE_KEYTYPE_SPATIAL;
  } else if (key_info->flags & HA_NOSAME) {
    if (!my_strcasecmp(system_charset_info, key_info->name, primary_key_name)) {
      *ret_type = TSE_KEYTYPE_PRIMARY;
    } else {
      *ret_type = TSE_KEYTYPE_UNIQUE;
    }
  } else if (key_info->flags & HA_FULLTEXT) {
    *ret_type = TSE_KEYTYPE_FULLTEXT;
  } else if (key_info->flags & HA_GENERATED_KEY) {
    *ret_type = TSE_KEYTYPE_FOREIGN;
  } else {
    *ret_type = TSE_KEYTYPE_MULTIPLE;
  }
  switch (*ret_type) {
    case TSE_KEYTYPE_SPATIAL:
      *ret_type = TSE_KEYTYPE_UNKNOW;
      TSE_ENGINE_ERROR("unsupport index type:TSE_KEYTYPE_SPATIAL");
      return false;
    case TSE_KEYTYPE_FULLTEXT:
      *ret_type = TSE_KEYTYPE_UNKNOW;
      TSE_ENGINE_ERROR("unsupport index type:TSE_KEYTYPE_FULLTEXT");
      return false;
    default:
      break;
  }
  return true;
}

bool get_tse_key_algorithm(ha_key_alg algorithm, int32_t *ret_algorithm) {
  *ret_algorithm = (int32_t)TSE_HA_KEY_ALG_BTREE;
  switch (algorithm) {
    case HA_KEY_ALG_RTREE:
      tse_log_error("unsupport index hash_type:HA_KEY_ALG_RTREE");
      return true;
    case HA_KEY_ALG_SE_SPECIFIC:
      TSE_ENGINE_ERROR("unsupport index hash_type:HA_KEY_ALG_SE_SPECIFIC");
      return false;
    case HA_KEY_ALG_FULLTEXT:
      TSE_ENGINE_ERROR("unsupport index hash_type:HA_KEY_ALG_FULLTEXT");
      return false;
    case TSE_HA_KEY_ALG_HASH:
      // tse_log_error("unsupport index hash_type:TSE_HA_KEY_ALG_HASH");
      return true;
    default:
      break;
  }
  // mysql字符序和daac的参数对接
  static map<const ha_key_alg, const tse_ha_key_alg>
      g_tse_key_algorithm_map = {
          {HA_KEY_ALG_SE_SPECIFIC, TSE_HA_KEY_ALG_SE_SPECIFIC},
          {HA_KEY_ALG_BTREE, TSE_HA_KEY_ALG_BTREE},
          {HA_KEY_ALG_RTREE, TSE_HA_KEY_ALG_RTREE},
          {HA_KEY_ALG_HASH, TSE_HA_KEY_ALG_HASH},
          {HA_KEY_ALG_FULLTEXT, TSE_HA_KEY_ALG_FULLTEXT}};
  auto it = g_tse_key_algorithm_map.find(algorithm);
  if (it != g_tse_key_algorithm_map.end()) {
    *ret_algorithm = (int32_t)it->second;
    return true;
  }
  return false;
}

bool tse_ddl_get_data_type_from_mysql_type(Field *field,
     const enum_field_types &mysql_type, int32_t *ret_type) {
  if (field != nullptr) {
    const field_cnvrt_aux_t *mysql_info = get_auxiliary_for_field_convert(field, mysql_type);
    if (mysql_info != nullptr) {
      *ret_type = mysql_info->ddl_field_type;
      return true;
    }
  }

  tse_log_error("unsupport mysql datatype:%d", (int32_t)mysql_type);
  *ret_type = TSE_DDL_TYPE_UNKNOW;
  return false;
}

// SET类型最多只能包含64个成员, 1～8成员的集合占1个字节 ,9～16成员的集合占2个字节
// 17～24成员的集合占3个字节, 25～32成员的集合占4个字节, 33～64成员的集合占8个字节
bool set_column_datatype(size_t set_num, TcDb__TseDDLColumnDef *column) {
  if (set_num < 9) {
    column->datatype->datatype = TSE_DDL_TYPE_TINY;
    return true;
  } else if (set_num > 8 && set_num < 17) {
    column->datatype->datatype = TSE_DDL_TYPE_SHORT;
    return true; 
  } else if (set_num > 16 && set_num < 33) {
    column->datatype->datatype = TSE_DDL_TYPE_INT24;
    return true; 
  } else if (set_num > 32 && set_num < 65) {
    column->datatype->datatype = TSE_DDL_TYPE_LONGLONG;
    return true; 
  } 

  return false;
}

bool tse_is_with_default_value(Field *field, const dd::Column *col_obj) {
  bool has_default_value = false;
  if (col_obj != nullptr) {
    const bool has_default = ((!field->is_flag_set(NO_DEFAULT_VALUE_FLAG) &&
                            !(field->auto_flags & Field::NEXT_NUMBER)) &&
                            !col_obj->is_default_value_null()) ||
                            field->m_default_val_expr;
    has_default_value = has_default && (col_obj->default_value_utf8().data()) != NULL;
  }
  const bool has_default_timestamp = field->has_insert_default_datetime_value_expression();
  if (has_default_value || has_default_timestamp) {
    return true;
  }
  return false;
}

tse_ddl_fk_rule tse_ddl_get_foreign_key_rule(fk_option rule) {
  switch (rule) {
    case FK_OPTION_UNDEF:
    case FK_OPTION_NO_ACTION:
    case FK_OPTION_RESTRICT:
    case FK_OPTION_DEFAULT:
      return TSE_DDL_FK_RULE_RESTRICT;
    case FK_OPTION_CASCADE:
      return TSE_DDL_FK_RULE_CASCADE;
    case FK_OPTION_SET_NULL:
      return TSE_DDL_FK_RULE_SET_NULL;
    default:
      break;
  }
  tse_log_error("unknown foreign key option %d", (int)rule);
  assert(0);
  return TSE_DDL_FK_RULE_UNKNOW;
}

tse_ddl_fk_rule tse_ddl_get_foreign_key_rule(dd::Foreign_key::enum_rule rule) {
  switch (rule) {
    case dd::Foreign_key::enum_rule::RULE_NO_ACTION:
    case dd::Foreign_key::enum_rule::RULE_RESTRICT:
    case dd::Foreign_key::enum_rule::RULE_SET_DEFAULT:
      return TSE_DDL_FK_RULE_RESTRICT;
    case dd::Foreign_key::enum_rule::RULE_CASCADE:
      return TSE_DDL_FK_RULE_CASCADE;
    case dd::Foreign_key::enum_rule::RULE_SET_NULL:
      return TSE_DDL_FK_RULE_SET_NULL;
    default:
      break;
  }
  tse_log_error("unknown foreign key option %d", (int)rule);
  assert(0);
  return TSE_DDL_FK_RULE_UNKNOW;
}

const dd::Index *tse_ddl_get_index_by_name(const dd::Table *tab_obj,
                                           const char *index_name) {
  for (const dd::Index *index : tab_obj->indexes()) {
    if (strcmp(index->name().data(), index_name) == 0) {
      return index;
    }
  }
  return nullptr;
}

const dd::Column *tse_ddl_get_column_by_name(const dd::Table *table_def,
                                             const char *col_name) {
  for (auto iter : table_def->columns()) {
    if (my_strcasecmp(system_charset_info, iter->name().data(), col_name) == 0) {
     return iter;
    }
  }
  return nullptr;
}

bool tse_ddl_get_create_key_type(dd::Index::enum_index_type type, int32_t *ret_type) {
  *ret_type = TSE_KEYTYPE_UNKNOW;
  switch (type) {
    case dd::Index::enum_index_type::IT_PRIMARY:
      *ret_type = TSE_KEYTYPE_PRIMARY;
      return true;
    case dd::Index::enum_index_type::IT_UNIQUE:
      *ret_type = TSE_KEYTYPE_UNIQUE;
      return true;
    case dd::Index::enum_index_type::IT_MULTIPLE:
      *ret_type = TSE_KEYTYPE_MULTIPLE;
      return true;
    case dd::Index::enum_index_type::IT_FULLTEXT:
      TSE_ENGINE_ERROR("unsupport index type:IT_FULLTEXT");
      return false;
    case dd::Index::enum_index_type::IT_SPATIAL:
      TSE_ENGINE_ERROR("unsupport index type:IT_SPATIAL");
      return false;
    default:
      break;
  }
  return true;
}

bool tse_ddl_get_create_key_algorithm(dd::Index::enum_index_algorithm algorithm,
                                      int32_t *ret_algorithm) {
  *ret_algorithm = (int32_t)TSE_HA_KEY_ALG_BTREE;
  switch (algorithm) {
    case dd::Index::enum_index_algorithm::IA_RTREE:
      // tse_log_error("unsupport index hash_type:IA_RTREE,use TSE_HA_KEY_ALG_BTREE");
      return true;
    case dd::Index::enum_index_algorithm::IA_SE_SPECIFIC:
      TSE_ENGINE_ERROR("unsupport index hash_type:IA_SE_SPECIFIC");
      return false;
    case dd::Index::enum_index_algorithm::IA_FULLTEXT:
      TSE_ENGINE_ERROR("unsupport index hash_type:IA_FULLTEXT");
      return false;
    case dd::Index::enum_index_algorithm::IA_HASH:
      // tse_log_error("unsupport index hash_type:IA_HASH USE TSE_HA_KEY_ALG_BTREE");
      return true;
    default:
      break;
  }

  static map<const dd::Index::enum_index_algorithm, const tse_ha_key_alg>
      g_tse_create_key_algorithm_map = {
          {dd::Index::enum_index_algorithm::IA_SE_SPECIFIC, TSE_HA_KEY_ALG_SE_SPECIFIC},
          {dd::Index::enum_index_algorithm::IA_BTREE, TSE_HA_KEY_ALG_BTREE},
          {dd::Index::enum_index_algorithm::IA_RTREE, TSE_HA_KEY_ALG_RTREE},
          {dd::Index::enum_index_algorithm::IA_HASH, TSE_HA_KEY_ALG_HASH},
          {dd::Index::enum_index_algorithm::IA_FULLTEXT, TSE_HA_KEY_ALG_FULLTEXT}};
  auto it = g_tse_create_key_algorithm_map.find(algorithm);
  if (it != g_tse_create_key_algorithm_map.end()) {
    *ret_algorithm = (int32_t)it->second;
    return true;
  }
  my_printf_error(ER_DISALLOWED_OPERATION, 
        "get index algorithm failed, unsuported index algorithm", MYF(0));
  return false;
}

uint16 get_prefix_index_len(const Field *field, const uint16 key_length) {
  uint16 prefix_len = 0;

  /* No prefix index on multi-value field */
  if (!field->is_array() &&
      (field->is_flag_set(BLOB_FLAG)||
      (key_length < field->pack_length() &&
      field->type() == MYSQL_TYPE_STRING) ||
      (field->type() == MYSQL_TYPE_VARCHAR &&
      key_length < field->pack_length() - field->get_length_bytes()))) {
    prefix_len = key_length;
    prefix_len /= field->charset()->mbmaxlen;
    assert(!field->gcol_info);
  }
  return prefix_len;
}

int convert_tse_part_type(dd::Table::enum_partition_type mysql_part_type, uint32_t *tse_part_type) {
  *tse_part_type = TSE_PART_TYPE_INVALID;
  switch (mysql_part_type) {
    case dd::Table::PT_RANGE: 
    case dd::Table::PT_RANGE_COLUMNS:
      *tse_part_type = TSE_PART_TYPE_RANGE;
      break;
    case dd::Table::PT_LIST:
    case dd::Table::PT_LIST_COLUMNS:
      *tse_part_type = TSE_PART_TYPE_LIST;
      break;
    case dd::Table::PT_HASH:
    case dd::Table::PT_LINEAR_HASH:
    case dd::Table::PT_KEY_51:
    case dd::Table::PT_KEY_55:
    case dd::Table::PT_LINEAR_KEY_51:
    case dd::Table::PT_LINEAR_KEY_55:
      *tse_part_type = TSE_PART_TYPE_HASH;
      break;
    default :
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
        "Partition tables support only hash partitions, range partitions, list partitions, and columns partitions.");
      tse_log_system("unsupported partition type : %d.", mysql_part_type);
      return 1;
  }
  return 0;
}

int convert_tse_subpart_type(dd::Table::enum_subpartition_type mysql_subpart_type, uint32_t *tse_part_type) {
  *tse_part_type = TSE_PART_TYPE_INVALID;
  switch (mysql_subpart_type) {
    case dd::Table::ST_NONE:
      *tse_part_type = TSE_PART_TYPE_INVALID;
      break;
    case dd::Table::ST_HASH:
    case dd::Table::ST_LINEAR_HASH:
    case dd::Table::ST_KEY_51:
    case dd::Table::ST_KEY_55:
    case dd::Table::ST_LINEAR_KEY_51:
    case dd::Table::ST_LINEAR_KEY_55:
      *tse_part_type = TSE_PART_TYPE_HASH;
      break;
    default :
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
        "SubPartition tables support only hash partitions");
      tse_log_system("unsupported partition type : %d.", mysql_subpart_type);
      return 1;
  }
  return 0;
}
