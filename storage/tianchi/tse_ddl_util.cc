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
#include "my_global.h"
#include "tse_ddl_util.h"
#include "tse_log.h"
#include "sql/sql_class.h"
//#include "sql/create_field.h"
#include "sql/sql_lex.h"
#include "sql/sql_table.h"
#include "sql/template_utils.h"
#include "sql/extra_defs.h"

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
    if (strcasecmp(form->field[i]->field_name.str, name) == 0) {
      return form->field[i];
    }
  }
  return nullptr;
}

const Create_field *tse_get_create_field_by_column_name(THD *thd, const char* field_name) {
  // 处理普通建表
  Alter_info *alter_info = &thd->lex->alter_info;
  const Create_field *field = NULL;
  if (alter_info != nullptr) {
    List_iterator_fast<Create_field> field_it(alter_info->create_list);
    while ((field = field_it++)) {
      if(strcmp(field->field_name.str, field_name) == 0) {
        return field;
      }
    }

    // 处理create table as select * from table 的特殊情况
    // 此时alter_info->create_list为空，需生成Create_field
    TABLE tmp_table MY_ATTRIBUTE((unused));
    // lex execution is not started, item->field cannot be got.
    /*if(!thd->lex->is_exec_started()) { //how could we get here before execution started?
      return nullptr;
    }*/
/*
    List_iterator<Item> inner_col_it(*item_in->unit->get_column_types(false));
    Item *outer_col, *inner_col;

    for (uint i= 0; i < item_in->left_expr->cols(); i++) 
    {    
      outer_col= item_in->left_expr->element_index(i);
      inner_col= inner_col_it++;

*/
	List_iterator<Item> itr(*(thd->lex->unit.get_column_types(false)));
	Item_ident *item;
    for (;;) { // all fields are visible for mariadb
      item = down_cast<Item_ident*>(itr++);
      if(strcmp(item->field_name.str, field_name) == 0) {
        Field *table_field;
//        Create_field *cr_field = generate_create_field(thd, item, &tmp_table);
//        if (cr_field == nullptr) {
//          break;
//        }
        Field *tmp_field= item->create_field_for_create_select(thd->mem_root,
                                                               &tmp_table);

        if (!tmp_field)
          break;

        switch (item->type())
        {
        /*
          We have to take into account both the real table's fields and
          pseudo-fields used in trigger's body. These fields are used
          to copy defaults values later inside constructor of
          the class Create_field.
        */
        case Item::FIELD_ITEM:
        case Item::TRIGGER_FIELD_ITEM:
          table_field= ((Item_field *) item)->field;
          break;
        default:
          table_field= NULL;
        }

        Create_field *cr_field= new (thd->mem_root)
                                  Create_field(thd, tmp_field, table_field);
        if (!cr_field)
          break;

        return cr_field;
      }
    }
  }

  // 处理create table like 情况
  // 此时alter_info为空，需通过prepare_alter_table生成alter_info
  if (thd->lex->sql_command != SQLCOM_CREATE_TABLE) {
    return nullptr;
  }

	//Alter_info*	local_alter_info;
  Alter_table_ctx local_alter_ctx MY_ATTRIBUTE((unused));
#ifdef FEATURE_X_FOR_MYSQL_32
  Table_ref *table_list = thd->lex->query_tables->next_global;
#else
  TABLE_LIST *table_list = thd->lex->query_tables->next_global;
#endif
  if (table_list != nullptr && table_list->table != nullptr) {
    /* Fill HA_CREATE_INFO and Alter_info with description of source table. */
    HA_CREATE_INFO create_info;
    create_info.db_type = table_list->table->s->db_type();
    // This should be ok even if engine substitution has taken place since
    // row_type denontes the desired row_type, and a different row_type may be
    // assigned to real_row_type later.
    create_info.row_type = table_list->table->s->row_type;
#if 0
    create_info.init_create_options_from_share(table_list->table->s, create_info.used_fields);

    // Prepare Create_field and Key_spec objects for ALTER and upgrade.
    prepare_fields_and_keys(thd, table_list->table, &create_info, &local_alter_info,
                            &local_alter_ctx, create_info.used_fields);
    List_iterator_fast<Create_field> field_like_it(local_alter_info->create_list);
    while ((field = field_like_it++)) {
      if(strcmp(field->field_name.str, field_name) == 0) {
        return field;
      }
    }
#endif
  }

  return nullptr;
}

tse_alter_table_drop_type tse_ddl_get_drop_type_from_mysql_type(Alter_drop::drop_type drop_type) {
		/*
 PERIOD?
		*/
  switch (drop_type) {
    case Alter_drop::KEY:
      return TSE_ALTER_TABLE_DROP_KEY;
    case Alter_drop::COLUMN:
      return TSE_ALTER_TABLE_DROP_COLUMN;
    case Alter_drop::FOREIGN_KEY:
      return TSE_ALTER_TABLE_DROP_FOREIGN_KEY;
    case Alter_drop::CHECK_CONSTRAINT:
      return TSE_ALTER_TABLE_DROP_CHECK_CONSTRAINT;
    default:
      return TSE_ALTER_TABLE_DROP_UNKNOW;
  }
}

tse_alter_column_type tse_ddl_get_alter_column_type_from_mysql_type(
    Alter_column *alt_col) {
  if (alt_col->is_rename()) return TSE_ALTER_COLUMN_RENAME_COLUMN;
  if (alt_col->default_value) return TSE_ALTER_COLUMN_SET_DEFAULT;
  else return TSE_ALTER_COLUMN_DROP_DEFAULT;
}

bool get_tse_key_type(const KEY *key_info, int32_t *ret_type) {
  *ret_type = TSE_KEYTYPE_UNKNOW;
  if (key_info->flags & HA_SPATIAL) {
    *ret_type = TSE_KEYTYPE_SPATIAL;
  } else if (key_info->flags & HA_NOSAME) {
    if (!my_strcasecmp(system_charset_info, key_info->name.str, primary_key_name.str)) {
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

struct key_alg_map_entry {
  ha_key_alg ha_alg;
  tse_ha_key_alg tse_alg;
};

static key_alg_map_entry g_key_alg_map[] = {
          {HA_KEY_ALG_UNDEF, TSE_HA_KEY_ALG_SE_SPECIFIC},
          {HA_KEY_ALG_BTREE, TSE_HA_KEY_ALG_BTREE},
          {HA_KEY_ALG_RTREE, TSE_HA_KEY_ALG_RTREE},
          {HA_KEY_ALG_HASH, TSE_HA_KEY_ALG_HASH},
          {HA_KEY_ALG_LONG_HASH, TSE_HA_KEY_ALG_HASH},
          {HA_KEY_ALG_FULLTEXT, TSE_HA_KEY_ALG_FULLTEXT}
};

bool get_tse_key_algorithm(ha_key_alg algorithm, int32_t *ret_algorithm) {
  *ret_algorithm = (int32_t)TSE_HA_KEY_ALG_BTREE;
  switch (algorithm) {
    case HA_KEY_ALG_RTREE:
      tse_log_error("unsupport index hash_type:HA_KEY_ALG_RTREE");
      return true;
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
  for (uint i = 0; i < sizeof(g_key_alg_map)/sizeof(key_alg_map_entry); i++)
  {
    auto *entry = g_key_alg_map + i;
    if (algorithm == entry->ha_alg)
	{
	  *ret_algorithm = (int32_t)entry->tse_alg;
	  return true;
	}
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

bool tse_is_with_default_value(Field *field) {
#if 0
  bool has_default_value = false;
  const bool has_default = ((!field_has_flag(field, NO_DEFAULT_VALUE_FLAG) &&
                            !(field->unireg_check & Field::NEXT_NUMBER)) &&
                            true) || //!col_obj->is_default_value_null()) || TODO
                            field->default_value ;// m_default_val_expr;
  has_default_value = has_default && field->table->s->default_values;//(col_obj->default_value_utf8().data()) != NULL;
  
    const bool has_default_timestamp = field->has_insert_default_datetime_value_expression();
  if (has_default_value || has_default_timestamp) {
    return true;
  }
#endif
  return false;
}


tse_ddl_fk_rule tse_ddl_get_foreign_key_rule(enum enum_fk_option rule) {

  switch (rule) {
    case FK_OPTION_UNDEF:
    case FK_OPTION_RESTRICT:
	case FK_OPTION_NO_ACTION:
	case FK_OPTION_SET_DEFAULT:
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

const KEY*tse_ddl_get_index_by_name(TABLE *tbl, const char *index_name) {
  for (uint i = 0; i < tbl->s->keys; i++)
  {
    KEY *key = tbl->s->key_info + i;
    if (strcmp(key->name.str, index_name) == 0) {
      return key;
    }
  }
  return nullptr;
}

const Field *tse_ddl_get_column_by_name(const TABLE *tbl, const char *col_name) {

  for (uint i = 0; i < tbl->s->fields; i++)
  {
    Field *fld = tbl->field[i];

    if (my_strcasecmp(system_charset_info, fld->field_name.str, col_name) == 0) {
     return fld;
    }
  }
  return nullptr;
}



uint16 get_prefix_index_len(const Field *field, const uint16 key_length) {
  uint16 prefix_len = 0;

  /* No prefix index on multi-value field */
  if ( // !field->is_array() &&
      (field_has_flag(field, BLOB_FLAG)||
      (key_length < field->pack_length() &&
      field->type() == MYSQL_TYPE_STRING) ||
      (field->type() == MYSQL_TYPE_VARCHAR &&
      key_length < field->pack_length() - down_cast<Field_str*>(const_cast<Field *>(field))->length_size()))) {
    prefix_len = key_length;
    prefix_len /= field->charset()->mbmaxlen;
    assert(!field->vcol_info);
  }
  return prefix_len;
}

#if 0
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
#endif

#if 0
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
#endif

