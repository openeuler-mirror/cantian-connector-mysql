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
#include "storage/ctc/ha_ctc.h"
#include "storage/ctc/ha_ctc_ddl.h"
#include "storage/ctc/ha_ctcpart.h"
#include <errno.h>

#include <algorithm>
#include <map>
#include "my_sqlcommand.h"
#include "sql/create_field.h"
#include "sql/dd/collection.h"
#include "sql/dd/dd_table.h"
#include "sql/dd/types/table.h"
#include "sql/sql_tablespace.h"
#include "sql/dd/types/tablespace.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/strfunc.h"  // find_type2
#include "protobuf/tc_db.pb-c.h"
#include "scope_guard.h"                       // create_scope_guard
#include "sql/dd/types/foreign_key.h"          // dd::Foreign_key
#include "sql/dd/types/foreign_key_element.h"  // dd::Foreign_key_element
#include "sql/dd/types/index.h"                // dd::Index
#include "sql/dd/types/index_element.h"        // dd::Index_element
#include "sql/sql_initialize.h"                // opt_initialize_insecure
#include "sql/dd/types/partition.h"
#include "sql/dd/types/partition_index.h"
#include "sql/dd/types/partition_value.h"
#include "sql/dd/types/partition_value.h"
#include "sql/partition_info.h"
#include "sql/partition_element.h"
#include "sql/dd/impl/utils.h"
#include "sql/sql_table.h"  // primary_key_name
#include "sql/sql_partition.h"
#include "sql/item_func.h"
#include "sql/item_json_func.h"
#include "sql/table_function.h"

#include "my_time.h"
#include "decimal.h"

#include "srv_mq_msg.h"
#include "decimal_convert.h"
#include "datatype_cnvrtr.h"
#include "ctc_error.h"
#include "ctc_log.h"
#include "ctc_util.h"
#include "ctc_ddl_util.h"
#include <mysql.h>
#include <boost/algorithm/string.hpp>

using namespace std;

#define CTC_MAX_COLUMN_LEN 65

#define CT_UNSPECIFIED_NUM_PREC 0
#define CT_UNSPECIFIED_NUM_SCALE (-100)

const uint32_t CTC_DDL_PROTOBUF_MEM_SIZE = 1024 * 1024 * 10;  // 10M
mutex m_ctc_ddl_protobuf_mem_mutex;
char ctc_ddl_req_mem[CTC_DDL_PROTOBUF_MEM_SIZE];
extern uint32_t ctc_instance_id;
extern handlerton *ctc_hton;

size_t ctc_ddl_stack_mem::ctc_ddl_req_msg_mem_max_size = 0;
size_t ctc_ddl_stack_mem::ctc_ddl_req_msg_mem_use_heap_cnt = 0;

int fill_delete_table_req(const char *full_path_name, const dd::Table *table_def,
  THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem) {
  TcDb__CtcDDLDropTableDef req;

  tc_db__ctc_ddldrop_table_def__init(&req);

  // 填充req信息
  char db_name[SMALL_RECORD_SIZE] = {0};
  char user_name[SMALL_RECORD_SIZE] = {0};
  char table_name[SMALL_RECORD_SIZE] = {0};

  bool is_tmp_table = ctc_is_temporary(table_def);
  ctc_split_normalized_name(full_path_name, db_name, SMALL_RECORD_SIZE, table_name, SMALL_RECORD_SIZE, &is_tmp_table);
  if (is_tmp_table) {
    ddl_ctrl->table_flags |= CTC_FLAG_TMP_TABLE;
    req.name = table_name;
  } else {
    req.name = const_cast<char *>(table_def->name().c_str());
  }
  ctc_copy_name(user_name, db_name, SMALL_RECORD_SIZE);

  req.user = user_name;
  req.db_name = CTC_GET_THD_DB_NAME(thd);

  // 拷贝算法语句在delete_table接口广播
  string drop_sql;
  if (is_alter_table_copy(thd)) {
    req.options |= CTC_DROP_FOR_MYSQL_COPY;
    ddl_ctrl->is_alter_copy = true;
  }

  if (is_tmp_table) { // 删除临时表不需要广播
    req.sql_str = nullptr;
  } else {
    drop_sql = string(thd->query().str).substr(0, thd->query().length);
    req.sql_str = const_cast<char *>(drop_sql.c_str());
  }

  if (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS) {
    req.options |= CTC_DROP_NO_CHECK_FK;
  } else if (table_def && !table_def->foreign_key_parents().empty()) {
    // broadcasted mysql need set to ignore FKs and cantian also ignore FKs
    req.options |= (CTC_DROP_NO_CHECK_FK_FOR_CANTIAN_AND_BROADCAST | CTC_DROP_NO_CHECK_FK);
  }

  size_t msg_len = tc_db__ctc_ddldrop_table_def__get_packed_size(&req);
  stack_mem->set_mem_size(msg_len + sizeof(ddl_ctrl_t));
  void *ctc_ddl_req_msg_mem = stack_mem->get_buf();
  if (ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  if (tc_db__ctc_ddldrop_table_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem) != msg_len) {
    assert(false);
  }

  ddl_ctrl->msg_len = msg_len;
  return 0;
}

static int ctc_init_create_tablespace_def(TcDb__CtcDDLSpaceDef *req, char **mem_start,
                                      char *mem_end) {
  tc_db__ctc_ddlspace_def__init(req);
  req->n_datafiles_list = 1;
  req->datafiles_list = (TcDb__CtcDDLDataFileDef **)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLDataFileDef*)); // mysql最多只有一个datafile，此处按1计算
  if (req->datafiles_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  req->datafiles_list[0] = (TcDb__CtcDDLDataFileDef *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLDataFileDef));
  if (req->datafiles_list[0] == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }

  tc_db__ctc_ddldata_file_def__init(req->datafiles_list[0]);
  TcDb__CtcDDLDataFileDef *datafile = req->datafiles_list[0];
  datafile->autoextend = (TcDb__CtcDDLAutoExtendDef *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLAutoExtendDef));
  if (datafile->autoextend == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  tc_db__ctc_ddlauto_extend_def__init(datafile->autoextend);
  return 0;
}

static void ctc_ddl_fill_datafile_by_alter_info(TcDb__CtcDDLDataFileDef *datafile, st_alter_tablespace *alter_info) {
  datafile->name = const_cast<char *>(alter_info->data_file_name);
  datafile->size = 1024 * 1024; // 8 * 1024 * 1024
  datafile->autoextend->enabled = true;
  if(alter_info->autoextend_size.has_value()) {
    datafile->autoextend->nextsize = (uint64_t)alter_info->autoextend_size.value();
  } else {
      datafile->autoextend->nextsize = 0;
  }

  // TODO:其他参数目前不确定如何填写 包含autoextend
}

static int ctc_create_tablespace_handler(handlerton *hton, THD *thd,
                                    st_alter_tablespace *alter_info,
                                    dd::Tablespace *dd_space MY_ATTRIBUTE((unused))) {
  if (engine_ddl_passthru(thd)) {
    return CT_SUCCESS;
  }
  ct_errno_t ret;
  size_t msg_len = 0;
  ctc_ddl_stack_mem stack_mem(0);
  void *ctc_ddl_req_msg_mem = nullptr;
  {
    TcDb__CtcDDLSpaceDef req;
    char *req_mem_start = ctc_ddl_req_mem;
    char *req_mem_end = req_mem_start + CTC_DDL_PROTOBUF_MEM_SIZE;
    lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
    ret = (ct_errno_t)ctc_init_create_tablespace_def(&req, &req_mem_start, req_mem_end);
    assert(req_mem_start <= req_mem_end);
    if (ret != 0) {
        return ret;
    }

    // fill parameter for create tablespace
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(alter_info->tablespace_name));
    req.name = const_cast<char *>(alter_info->tablespace_name);
    req.db_name = CTC_GET_THD_DB_NAME(thd);
    string sql = string(thd->query().str).substr(0, thd->query().length);
    req.sql_str = const_cast<char *>(sql.c_str());

    // fill datafile parameter
    TcDb__CtcDDLDataFileDef *datafile = req.datafiles_list[0];
    if(check_data_file_name(alter_info->data_file_name)) {
      return HA_ERR_WRONG_FILE_NAME;
    }

    ctc_ddl_fill_datafile_by_alter_info(datafile, alter_info);

    msg_len = tc_db__ctc_ddlspace_def__get_packed_size(&req);
    stack_mem.set_mem_size(msg_len + sizeof(ddl_ctrl_t));
    ctc_ddl_req_msg_mem = stack_mem.get_buf();
    if(ctc_ddl_req_msg_mem == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }

    if (tc_db__ctc_ddlspace_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem) != msg_len) {
      assert(false);
    }
  }
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ddl_ctrl.msg_len = msg_len;
  ret = (ct_errno_t)ctc_create_tablespace(ctc_ddl_req_msg_mem, &ddl_ctrl);
  memcpy(&tch, &ddl_ctrl.tch, sizeof(ctc_handler_t));
  ctc_ddl_hook_cantian_error("ctc_create_tablespace_cantian_error", thd, &ddl_ctrl, &ret);
  update_sess_ctx_by_tch(tch, hton, thd);
  return ctc_ddl_handle_fault("ctc_create_tablespace", thd, &ddl_ctrl, ret, alter_info->tablespace_name);
}

static int ctc_alter_tablespace_handler(handlerton *hton, THD *thd,
                                        st_alter_tablespace *alter_info,
                                        const dd::Tablespace *old_dd_space,
                                        dd::Tablespace *new_dd_space) {
  // 只支持修改表空间名称 以及设置属性
  if (alter_info->ts_alter_tablespace_type != ALTER_TABLESPACE_RENAME &&
      alter_info->ts_alter_tablespace_type != ALTER_TABLESPACE_OPTIONS) {
    return HA_ADMIN_NOT_IMPLEMENTED;
  }
  if (engine_ddl_passthru(thd)) {
    return CT_SUCCESS;
  }
  // TODO 先检查旧表空间是否存在
  // TODO 检查是否是undo表空间 undo表空间需要指定 alter undo tablespace
  // TODO 处理加密相关操作
  ct_errno_t ret;
  const char *from = old_dd_space->name().c_str();
  const char *to = new_dd_space->name().c_str();
  size_t msg_len = 0;
  ctc_ddl_stack_mem stack_mem(0);
  void *ctc_ddl_req_msg_mem = nullptr;
  {
    lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
    // TODO 检测from表名 不能和系统表一致（即不能修改系统表）
    if (alter_info->ts_alter_tablespace_type == ALTER_TABLESPACE_RENAME &&
        my_strcasecmp(system_charset_info, from, to) == 0) {
      my_printf_error(ER_WRONG_TABLESPACE_NAME,
                      "tablespace name is same for rename", MYF(0));
      return HA_WRONG_CREATE_OPTION;
    }
    auto ctc_alter_action =
        g_ctc_alter_tablespace_map.find(alter_info->ts_alter_tablespace_type);
    if (ctc_alter_action == g_ctc_alter_tablespace_map.end()) {
      return ER_ILLEGAL_HA;
    }

    TcDb__CtcDDLAlterSpaceDef req;
    tc_db__ctc_ddlalter_space_def__init(&req);

    req.action = ctc_alter_action->second;
    
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(from));
    req.name = const_cast<char *>(from);
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(to));
    req.new_name = const_cast<char *>(to);

    req.db_name = CTC_GET_THD_DB_NAME(thd);
    string sql = string(thd->query().str).substr(0, thd->query().length);
    req.sql_str = const_cast<char *>(sql.c_str());
    if (alter_info->autoextend_size != 0 && alter_info->autoextend_size.has_value()) {
      req.auto_extend_size = (uint64_t)alter_info->autoextend_size.value();
    }
    msg_len = tc_db__ctc_ddlalter_space_def__get_packed_size(&req);
    stack_mem.set_mem_size(msg_len + sizeof(ddl_ctrl_t));
    ctc_ddl_req_msg_mem = stack_mem.get_buf();
    if(ctc_ddl_req_msg_mem == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }

    if (tc_db__ctc_ddlalter_space_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem) != msg_len) {
      assert(false);
    }
  }
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ddl_ctrl.msg_len = msg_len;
  ret = (ct_errno_t)ctc_alter_tablespace(ctc_ddl_req_msg_mem, &ddl_ctrl);
  memcpy(&tch, &ddl_ctrl.tch, sizeof(ctc_handler_t));
  ctc_ddl_hook_cantian_error("ctc_alter_tablespace_cantian_error", thd, &ddl_ctrl, &ret);
  update_sess_ctx_by_tch(tch, hton, thd);
  return ctc_ddl_handle_fault("ctc_alter_tablespace", thd, &ddl_ctrl, ret);
}

static int ctc_drop_tablespace_handler(handlerton *hton, THD *thd,
                                       st_alter_tablespace *alter_info,
                                       const dd::Tablespace *dd_space  MY_ATTRIBUTE((unused))) {
  if (engine_ddl_passthru(thd)) {
    return CT_SUCCESS;
  }
  ct_errno_t ret;
  size_t msg_len = 0;
  ctc_ddl_stack_mem stack_mem(0);
  void *ctc_ddl_req_msg_mem = nullptr;
  {
    lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
    TcDb__CtcDDLDropSpaceDef req;
    tc_db__ctc_ddldrop_space_def__init(&req);
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(alter_info->tablespace_name));
    req.obj_name = const_cast<char *>(alter_info->tablespace_name);
    
    req.db_name = CTC_GET_THD_DB_NAME(thd);
    string sql = string(thd->query().str).substr(0, thd->query().length);
    req.sql_str = const_cast<char *>(sql.c_str());


    msg_len = tc_db__ctc_ddldrop_space_def__get_packed_size(&req);
    stack_mem.set_mem_size(msg_len + sizeof(ddl_ctrl_t));
    ctc_ddl_req_msg_mem = stack_mem.get_buf();
    if(ctc_ddl_req_msg_mem == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }

    if (tc_db__ctc_ddldrop_space_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem) != msg_len) {
      assert(false);
    }
  }
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ddl_ctrl.msg_len = msg_len;

  ret = (ct_errno_t)ctc_drop_tablespace(ctc_ddl_req_msg_mem, &ddl_ctrl);
  memcpy(&tch, &ddl_ctrl.tch, sizeof(ctc_handler_t));
  ctc_ddl_hook_cantian_error("ctc_drop_tablespace_cantian_error", thd, &ddl_ctrl, &ret);
  update_sess_ctx_by_tch(tch, hton, thd);
  return ctc_ddl_handle_fault("ctc_drop_tablespace", thd, &ddl_ctrl, ret, alter_info->tablespace_name);
}


/**
 alter tablespace.
 @param: hton in, ctc handlerton
 @param: thd in, handle to the MySQL thread
 @param: savepoint in, savepoint data
 @return: 0 if succeeds
*/
int ctcbase_alter_tablespace(handlerton *hton, THD *thd,
                             st_alter_tablespace *alter_info,
                             const dd::Tablespace *old_ts_def,
                             dd::Tablespace *new_ts_def) {
  DBUG_TRACE;

  // TODO：只读模式和强制恢复模式不能修改表空间
  switch (alter_info->ts_cmd_type) {
    case CREATE_TABLESPACE:
      return ctc_create_tablespace_handler(hton, thd, alter_info, new_ts_def);
    case ALTER_TABLESPACE:
      return ctc_alter_tablespace_handler(hton, thd, alter_info, old_ts_def, new_ts_def);
    case DROP_TABLESPACE:
      return ctc_drop_tablespace_handler(hton, thd, alter_info, old_ts_def);
    case CREATE_UNDO_TABLESPACE:
    case ALTER_UNDO_TABLESPACE:
    case DROP_UNDO_TABLESPACE:
      return CT_SUCCESS;
    case CREATE_LOGFILE_GROUP:
    case DROP_LOGFILE_GROUP:
    case ALTER_LOGFILE_GROUP:
      my_error(ER_FEATURE_UNSUPPORTED, MYF(0), "LOGFILE GROUP", "by CTC");
      break;
    case ALTER_ACCESS_MODE_TABLESPACE:
      my_error(ER_FEATURE_UNSUPPORTED, MYF(0), "ACCESS MODE", "by CTC");
      break;
    case CHANGE_FILE_TABLESPACE:
      my_error(ER_FEATURE_UNSUPPORTED, MYF(0), "CHANGE FILE", "by CTC");
      break;
    case TS_CMD_NOT_DEFINED:
    default:
      my_error(ER_FEATURE_UNSUPPORTED, MYF(0), "UNKNOWN", "by CTC");
  }

  return HA_ADMIN_NOT_IMPLEMENTED;
}

static bool ctc_fill_column_precision_and_scale(TcDb__CtcDDLColumnDef *column, Field *field) {
  switch (field->real_type()) {
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      if (field->decimals() == DECIMAL_NOT_SPECIFIED) {
        column->datatype->scale = CT_UNSPECIFIED_NUM_SCALE;
        column->datatype->precision = CT_UNSPECIFIED_NUM_PREC;
      } else {
        column->datatype->scale = field->decimals();
        Field_real *field_real = dynamic_cast<Field_real *>(field);
        column->datatype->precision = field_real->max_display_length();
      }
      break;
    }
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      Field_new_decimal *f = dynamic_cast<Field_new_decimal *>(field);
      column->datatype->precision = f->precision;
      column->datatype->scale = f->decimals();
      if (f->precision > MAX_NUMERIC_BUFF) {
        my_error(ER_TOO_BIG_PRECISION, MYF(0), static_cast<int>(f->precision),
                 column->name, static_cast<ulong>(MAX_NUMERIC_BUFF));
        return false;
      }
      break;
    }
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2: {
      column->datatype->precision = field->decimals();
      break;
    }
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      Field_enum *field_enum = dynamic_cast<Field_enum *>(field);
      column->datatype->precision = field_enum->typelib->count;
      break;
    }
    case MYSQL_TYPE_BIT:
      column->datatype->precision = field->max_display_length();
      break;
    default:
      break;
  }
  return true;
}

static void ctc_set_unsigned_column(Field *field, uint32 *is_unsigned) {
  if ((is_numeric_type(field->type()) && field->is_unsigned()) ||
       field->real_type() == MYSQL_TYPE_ENUM || field->real_type() == MYSQL_TYPE_BIT) {
    *is_unsigned = 1;
  } else {
    *is_unsigned = 0;
  }
}

static bool ctc_ddl_fill_column_by_field_fill_type(TcDb__CtcDDLColumnDef *column, Field *field) {
  if (!ctc_ddl_get_data_type_from_mysql_type(field, field->type(), &column->datatype->datatype)) {
    char info[300]; // max column name length(64) * max_mb_size(4) + redundancy
    sprintf(info, "column name: %s", field->field_name);
    my_error(ER_FEATURE_UNSUPPORTED, MYF(0), "*DataType Conversion*", info);
    return false;
  }

  column->datatype->size = field->pack_length();
  column->datatype->mysql_ori_datatype = field->real_type();

  if (field->type() == MYSQL_TYPE_VARCHAR) {
    uint32_t varchar_length = field->row_pack_length();
    if (VARCHAR_AS_BLOB(varchar_length)) {
      column->datatype->datatype = CTC_DDL_TYPE_CLOB;
    }
    column->datatype->size = varchar_length;
  }
  ctc_set_unsigned_column(field, &column->is_unsigned);

  if (!ctc_fill_column_precision_and_scale(column, field)) {
    ctc_log_error("fill column precision and scale failed");
    return false;
  }
  return true;
}

static int ctc_prepare_enum_field_impl(THD *thd, Create_field *sql_field, String *def) {
  DBUG_TRACE;
  assert(sql_field->sql_type == MYSQL_TYPE_ENUM);
  if (!sql_field->charset) {
    sql_field->charset = &my_charset_bin;
  }
  /* SQL "NULL" maps to NULL */
  if (def == nullptr) {
      if ((sql_field->flags & NOT_NULL_FLAG) != 0) {
        my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
        return CTC_ENUM_DEFAULT_NULL;
      }
  } else {
    def->length(sql_field->charset->cset->lengthsp(sql_field->charset, def->ptr(), def->length()));
    TYPELIB *interval = sql_field->interval;
    if (!interval) {
      interval = create_typelib(thd->mem_root, sql_field);
    }
    uint enum_index = find_type2(interval, def->ptr(), def->length(), sql_field->charset);
    if (enum_index == 0) {
      my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
      return CTC_ENUM_DEFAULT_INVALID;
    }
    return enum_index;
  }
  my_error(ER_INVALID_DEFAULT, MYF(0), "constant default is null");
  return CTC_ENUM_DEFAULT_INVALID;
}

static bool ctc_prepare_set_field_impl(THD *thd, Create_field *sql_field, ulonglong *set_bitmap,
                                       String *def, TcDb__CtcDDLColumnDef *column) {
  DBUG_TRACE;
  assert(sql_field->sql_type == MYSQL_TYPE_SET);

  if (!sql_field->charset) {
    sql_field->charset = &my_charset_bin;
  }
  TYPELIB *interval = sql_field->interval;
  if (!interval) {
    /*
      Create the typelib in runtime memory - we will free the
      occupied memory at the same time when we free this
      sql_field -- at the end of execution.
    */
    interval = create_typelib(thd->mem_root, sql_field);
  }

  // Comma is an invalid character for SET names
  char comma_buf[4]; /* 4 bytes for utf32 */
  int comma_length = sql_field->charset->cset->wc_mb(sql_field->charset, ',', reinterpret_cast<uchar *>(comma_buf),
      reinterpret_cast<uchar *>(comma_buf) + sizeof(comma_buf));
  assert(comma_length > 0);

  if (!set_column_datatype(interval->count, column)) {
    ctc_log_error("set column datatype failed, set num is %lu", interval->count);
    return false;
  }

  for (uint i = 0; i < interval->count; i++) {
    uint is_default_values = sql_field->charset->coll->strstr(sql_field->charset, interval->type_names[i],
                                         interval->type_lengths[i], comma_buf, comma_length, nullptr, 0);
    if (is_default_values != 0) {
      ErrConvString err(interval->type_names[i], interval->type_lengths[i], sql_field->charset);
      my_error(ER_ILLEGAL_VALUE_FOR_TYPE, MYF(0), "set", err.ptr());
      return false;
    }
  }

  const char *not_used;
  uint not_used2;
  bool not_found = false;
  // SQL "NULL" maps to NULL
  if (def == nullptr) {
    if ((sql_field->flags & NOT_NULL_FLAG) != 0) {
      my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
      return false;
    } else {
      // else, NULL is an allowed value
      *set_bitmap = find_set(interval, nullptr, 0, sql_field->charset, &not_used, &not_used2, &not_found);
    }
  } else {
    // default not NULL */
    *set_bitmap = find_set(interval, def->ptr(), def->length(), sql_field->charset, &not_used, &not_used2, &not_found);
  }

  if (not_found) {
    my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
    return false;
  }

  return true;
}

static bool ctc_process_string_default_value(TcDb__CtcDDLColumnDef *column, string &expr_str,
                                             char **mem_start, char *mem_end, bool is_blob_type) {
  if (!is_blob_type) {
    boost::algorithm::replace_all(expr_str, "'", "''");
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, expr_str.length() + 1);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for bit default text failed, mem_start is null");
      return false;
    }
    strncpy(column->default_text, expr_str.c_str(), expr_str.length() + 1);
  } else {
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, expr_str.length() * 2 + 1);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for bit default text failed, mem_start is null");
      return false;
    }
    int pos = 0;
    for (char &ch : expr_str) {
      sprintf(column->default_text + pos, "%02X", ch);
      pos += 2;
    }
    column->default_text[pos] = '\0';
  }
  return true;
}

static bool ctc_get_bit_default_value(
    THD *thd, TcDb__CtcDDLColumnDef *column, Field *field, const Create_field *fld, const dd::Column *col_obj,
    char **mem_start, char *mem_end, bool is_expr_value) {
  column->is_unsigned = 1;
  longlong num = 0;
  if (!is_expr_value) {
    char* bit_value_buf = const_cast<char *>(col_obj->default_value().data());
    uint32_t bit_value_len = (uint32_t)(col_obj->default_value().length());
    Field_bit *bitfield = dynamic_cast<Field_bit *>(field);
    Field_bit *new_field = bitfield->clone(thd->mem_root);
    uchar bit_ptr[CTC_MAX_BIT_LEN];
    for (uint32_t i = 0; i < bit_value_len; i++) {
      bit_ptr[i] = bit_value_buf[i];
    }
    new_field->set_field_ptr(bit_ptr);
    num = new_field->val_int();
  } else {
    Item *expr_item;
    if (fld == nullptr) {
      expr_item = field->m_default_val_expr->expr_item;
    } else {
      expr_item = fld->m_default_val_expr->expr_item;
    }
    if (expr_item->type() != Item::STRING_ITEM) {
      num = expr_item->val_int();
    } else {
      StringBuffer<MY_INT64_NUM_DECIMAL_DIGITS + 1> tmp;
      String *res = expr_item->val_str(&tmp);
      if (res == nullptr) {
        num = 0;
      } else {
        int err = 0;
        num = my_strntoll(res->charset(), res->ptr(), res->length(), 10, nullptr,
                          &err);
        if (err) {
          string expr_str(res->c_ptr());
          return ctc_process_string_default_value(column, expr_str, mem_start, mem_end, false);
        }
      }
    }
  }

  uint32_t num_len = to_string(num).length();
  column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, num_len + 1);
  if (column->default_text == nullptr) {
    ctc_log_error("alloc mem for bit default text failed, mem_start is null");
    return false;
  }
  sprintf(column->default_text, "%llu", num);
  column->default_text[num_len] = '\0';

  return true;
}

static bool ctc_get_datetime_default_value(
    TcDb__CtcDDLColumnDef *column, Field *field, const Create_field *fld, const dd::Column *col_obj,
    char **mem_start, char *mem_end, ctc_column_option_set_bit *option_set, bool is_expr_value) {
  if (field->has_insert_default_datetime_value_expression()) {
    // current_timestamp (or with ON UPDATE CURRENT_TIMESTAMP)
    option_set->is_default_func =  1;
    option_set->is_curr_timestamp = 1;
    column->default_text = const_cast<char *>(col_obj->default_value_utf8().data());
    return true;
  }
  if (field->has_update_default_datetime_value_expression() &&
      (!col_obj->default_value_utf8().data() ||
      strlen(col_obj->default_value_utf8().data()) == 0)) {
    // ON UPDATE CURRENT_TIMESTAMP without default_value
    return true;
  }
  date_detail_t date_detail;
  // decode mysql datetime from binary
  MYSQL_TIME ltime;
  memset(&ltime, 0, sizeof(MYSQL_TIME));
  memset(&date_detail, 0, sizeof(date_detail_t));
  const field_cnvrt_aux_t* mysql_info = get_auxiliary_for_field_convert(field, field->type());
  assert(mysql_info != NULL);
  string expr_str;
  if (is_expr_value) {
    String str;
    if (fld) {
      expr_str = (fld->m_default_val_expr->expr_item->val_str(&str))->c_ptr();
    } else {
      expr_str = (field->m_default_val_expr->expr_item->val_str(&str))->c_ptr();
    }
    MYSQL_TIME_STATUS status;
    str_to_datetime(expr_str.c_str(), expr_str.length(), &ltime, TIME_FRAC_TRUNCATE | TIME_FUZZY_DATE,
      &status);
  } else {
    expr_str = const_cast<char *>(col_obj->default_value_utf8().data());
    const uchar *mysql_ptr = (const uchar *)(col_obj->default_value().data());
    decode_mysql_datetime(ltime, mysql_info, mysql_ptr, field);
  }

  int ret = assign_mysql_date_detail(mysql_info->mysql_field_type, ltime, &date_detail);
  if (ret != CT_SUCCESS) {
    return false;
  }

  if (check_zero_date(date_detail)) {
    char *tmp_zero_date =  const_cast<char*>("0000-00-00 00:00:00");
    int len = strlen(tmp_zero_date);
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, len + 1);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for datetime default text failed.");
      return false;
    }
    strncpy(column->default_text, tmp_zero_date, len + 1);
    return true;
  }

  switch (field->real_type()) {
    case MYSQL_TYPE_TIME2: {
      int len = expr_str.length();
      column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(char) * (len + 1));
      if (column->default_text == nullptr) {
        ctc_log_error("alloc mem for datetime default text failed.");
        return false;
      }
      sprintf(column->default_text, "%s", expr_str.c_str());
      break;
    }
    case MYSQL_TYPE_TIMESTAMP2: {
      if (!is_expr_value) {
        char tmp_timestamp[MAX_DATE_STRING_REP_LENGTH];
        int len = my_datetime_to_str(ltime, tmp_timestamp, field->decimals());
        column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, len + 1);
        if (column->default_text == nullptr) {
          ctc_log_error("alloc mem for datetime default text failed.");
          return false;
        }
        strncpy(column->default_text, tmp_timestamp, len + 1);
        break;
      }
    }
    default: {
      int len = expr_str.length();
      column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, len + 1);
      if (column->default_text == nullptr) {
        ctc_log_error("alloc mem for datetime default text failed.");
        return false;
      }
      strncpy(column->default_text, expr_str.c_str(), len + 1);
      break;
    }
  }
  return true;
}

static int ctc_prepare_enum_field(THD *thd, Field *field, const Create_field *fld,
                                  const CHARSET_INFO *field_cs) {
  int is_enum = 0;
  String default_str;
  String *def;
  Create_field* sql_field;
  // fld == nullptr 为create，此时field_charset 为空值需处理置位
  if (fld == nullptr) {
    Create_field sql_field_local(field, field);
    if (!field_cs) {
      sql_field_local.charset = field_cs;
    }
    sql_field = sql_field_local.clone(thd->mem_root);
  } else {
    // fld != nullptr 为alter，此时field_charset 有值不用置位
    sql_field = const_cast<Create_field *>(fld);
  }
  if (sql_field->constant_default != nullptr) {
    def = sql_field->constant_default->val_str(&default_str);
  } else {
    def = sql_field->m_default_val_expr->expr_item->val_str(&default_str);
  }
  if (fld == nullptr) {
    // 修改ENUM default带charset
    // 或设置了全局charset找不到enum_index
    // 判断当前charset与field charset是否一致
    if(field_cs == nullptr || strcmp(def->charset()->csname, field_cs->csname) != 0) {
      sql_field->charset = def->charset();
    }
  }
  is_enum = ctc_prepare_enum_field_impl(thd, sql_field, def);
  return is_enum;
}

static bool ctc_get_enum_default_value(
    THD *thd, TcDb__CtcDDLColumnDef *column, const dd::Column *col_obj, Field *field, const Create_field *fld,
    char **mem_start, char *mem_end, const CHARSET_INFO *field_cs) {
  int is_enum;
  column->is_unsigned = 1;
  column->datatype->datatype = column->datatype->size == 1 ? CTC_DDL_TYPE_TINY : CTC_DDL_TYPE_SHORT;

  is_enum = ctc_prepare_enum_field(thd, field, fld, field_cs);

  if (is_enum == CTC_ENUM_DEFAULT_INVALID) {
    return false;
  }
  column->default_text = nullptr;
  if (is_enum != CTC_ENUM_DEFAULT_NULL) {
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, 10);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for enum default text failed, mem_start is null");
      return false;
    }
    sprintf(column->default_text, "%d", is_enum);
  } else {
    char *default_value = const_cast<char *>(col_obj->default_value_utf8().data());
    int len = strlen(default_value);
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, len + 1);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for enum default text failed, mem_start is null");
      return false;
    }
    strncpy(column->default_text, default_value, len + 1);
  }
  return true;
}

static bool ctc_prepare_set_field(THD *thd, Field *field, const Create_field *fld, const CHARSET_INFO *field_cs,
                                  ulonglong *set_bitmap, TcDb__CtcDDLColumnDef *column) {
  bool is_get_set_bitmap = 0;
  String default_str;
  String *def;
  Create_field* sql_field;
  // fld == nullptr 为create，此时field_charset 为空值需处理置位
  if (fld == nullptr) {
    Create_field sql_field_local(field, field);
    if (!field_cs) {
      sql_field_local.charset = field_cs;
    }
    sql_field = sql_field_local.clone(thd->mem_root);
  } else {
    // fld != nullptr 为alter，此时field_charset 有值不用置位
    sql_field = const_cast<Create_field *>(fld);
  }
  if (sql_field->constant_default != nullptr) {
    def = sql_field->constant_default->val_str(&default_str);
  } else {
    def = sql_field->m_default_val_expr->expr_item->val_str(&default_str);
  }
  if (fld == nullptr) {
    if(field_cs == nullptr || strcmp(def->charset()->csname, field_cs->csname) != 0) {
      sql_field->charset = def->charset();
    }
  }

  is_get_set_bitmap = ctc_prepare_set_field_impl(thd, sql_field, set_bitmap, def, column);
  return is_get_set_bitmap;
}

static bool ctc_get_set_default_value(
    THD *thd, TcDb__CtcDDLColumnDef *column, Field *field, const Create_field *fld, char **mem_start,
    char *mem_end, const CHARSET_INFO *field_cs) {
  ulonglong set_bitmap;
  bool is_get_set_bitmap = false;
  if (is_numeric_type(field->type()) && field->is_unsigned()) {
    column->is_unsigned = 1;
  }
  // fld == nullptr 为create，此时field_charset 为空值需处理置位
  is_get_set_bitmap = ctc_prepare_set_field(thd, field, fld, field_cs, &set_bitmap, column);

  if (!is_get_set_bitmap) {
    return false;
  } else {
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, 64);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for set default text failed, mem_start is null");
      return false;
    }
    sprintf(column->default_text, "%lld", set_bitmap);
  }

  return true;
}

static bool ctc_verify_string_default_length(TcDb__CtcDDLColumnDef *column, String* default_str, Field *field,
                                             const Create_field *fld) {
  if (column->datatype->datatype == CTC_DDL_TYPE_CLOB) {
    return true;
  }
  const CHARSET_INFO* col_charset = fld ? fld->charset : field->charset();
  int max_char_count = column->datatype->size / col_charset->mbmaxlen;
  int default_str_char_count = default_str->numchars();
  if (default_str_char_count > max_char_count) {
    my_error(ER_INVALID_DEFAULT, MYF(0), column->name);
    return false;
  }
  return true;
}

static bool ctc_get_string_default_value(
    TcDb__CtcDDLColumnDef *column, Field *field, const dd::Column *col_obj, const Create_field *fld,
    char **mem_start, char *mem_end, bool is_blob_type) {
  char *field_default_string = nullptr;
  if (fld == nullptr) {
    if (!field->m_default_val_expr) {
      if (field->real_type() == MYSQL_TYPE_STRING) {
        field_default_string = const_cast<char *>(col_obj->default_value().data());
      } else {
        field_default_string = const_cast<char *>((col_obj->default_value().data() + 1));
      }
    } else {
      String tmp_string;
      String* tmp_string_ptr;
      assert(field->m_default_val_expr);
      tmp_string_ptr = field->m_default_val_expr->expr_item->val_str(&tmp_string);
      if (!is_blob_type && !ctc_verify_string_default_length(column, tmp_string_ptr, field, fld)) {
        return false;
      }
      field_default_string = tmp_string_ptr->c_ptr();
    }
  } else {
    // for alter table add column
    String tmp_string;
    String* tmp_string_ptr;
    if (fld->constant_default != nullptr) {
      tmp_string_ptr = fld->constant_default->val_str(&tmp_string);
    } else {
      tmp_string_ptr = fld->m_default_val_expr->expr_item->val_str(&tmp_string);
    }
    if (!ctc_verify_string_default_length(column, tmp_string_ptr, field, fld)) {
        return false;
    }
    field_default_string = tmp_string_ptr->c_ptr();
  }
  string expr_str(field_default_string);
  return ctc_process_string_default_value(column, expr_str, mem_start, mem_end, is_blob_type);
}

static bool ctc_get_numeric_default_value(
    TcDb__CtcDDLColumnDef *column, Field *field, const dd::Column *col_obj, const Create_field *fld,
    char **mem_start, char *mem_end, bool is_expr_value)
{
  char *field_default_string = nullptr;
  if (is_expr_value) {
    String tmp_string;
    Item* expr_item;
    expr_item = fld ? fld->m_default_val_expr->expr_item : field->m_default_val_expr->expr_item;
    if (expr_item->type() == Item::VARBIN_ITEM) {
      longlong num = expr_item->val_int();
      uint32_t num_len = to_string(num).length();
      column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, num_len + 1);
      if (column->default_text == nullptr) {
        ctc_log_error("alloc mem for set default text failed, mem_start is null");
        return false;
      }
      sprintf(column->default_text, "%llu", num);
      column->default_text[num_len] = '\0';
    } else {
      field_default_string = expr_item->val_str(&tmp_string)->c_ptr();
      int default_text_len = strlen(field_default_string);
      column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, default_text_len + 1);
      if (column->default_text == nullptr) {
        ctc_log_error("alloc mem for set default text failed, mem_start is null");
        return false;
      }
      strncpy(column->default_text, field_default_string, default_text_len + 1);
    }
  } else {
    char *default_value = const_cast<char *>(col_obj->default_value_utf8().data());
    int len = strlen(default_value);
    column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, len + 1);
    if (column->default_text == nullptr) {
      ctc_log_error("alloc mem for numeric default text failed, mem_start is null");
      return false;
    }
    strncpy(column->default_text, default_value, len + 1);
  }
  return true;
}

static bool ctc_check_expression_default_value(TcDb__CtcDDLColumnDef *column, Field *field, const Create_field *fld,
  ctc_column_option_set_bit *option_set, bool* is_expr_value) {
  if ((field != nullptr && !field->m_default_val_expr) ||
     (fld != nullptr && !fld->m_default_val_expr)) {
      //排除普通表达式
      option_set->is_default_func = 0;
      return true;
  }
  Item *constant_default = fld == nullptr ? field->m_default_val_expr->expr_item :
                                            fld->m_default_val_expr->expr_item;
  //default expression
  if (!constant_default->const_for_execution() && constant_default->type() != Item::FUNC_ITEM) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                    "Current storage engine only support default expression without variable parameters");
    return false;
  } else if (constant_default->type() == Item::FUNC_ITEM) {
    // default function
    Item_func *item_func = dynamic_cast<Item_func *>(constant_default);
    column->default_func_name = const_cast<char *>(item_func->func_name());
    for (uint i = 0; i < item_func->arg_count; i++) {
      if (item_func->get_arg(i)->type() == Item::FIELD_ITEM) {
        my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                    "Current storage engine only support default function without variable parameters");
        return false;
      }
    }
  } else {
    option_set->is_default_func = 0;
    *is_expr_value = true;
  }
  return true;
}


static bool ctc_ddl_fill_column_default_value(
  THD *thd, TcDb__CtcDDLColumnDef *column, Field *field, const Create_field *fld, const dd::Column *col_obj,
  ctc_column_option_set_bit *option_set, char **mem_start, char *mem_end, const CHARSET_INFO *field_cs) {
  option_set->is_default = true;
  option_set->is_default_null = false;
    
  // function except DEFAULT CURRENT_TIMESTAMP and ON UPDATE CURRENT_TIMESTAMP
  // CURRENT_TIMESTAMP and ON UPDATE CURRENT_TIMESTAMP is described in ctc_get_datetime_default_value
  option_set->is_default_func = field->has_insert_default_general_value_expression() &&
                                !field->has_insert_default_datetime_value_expression() &&
                                !field->has_update_default_datetime_value_expression();
  option_set->is_curr_timestamp = 0;
  bool is_expr_value = false;
  if (option_set->is_default_func) {
    CTC_RETURN_IF_ERROR(ctc_check_expression_default_value(column, field, fld, option_set, &is_expr_value), false);
    if (!is_expr_value) {
      return true;
    }
  }

  bool is_blob_type = false;
  switch (field->real_type()) {
    case MYSQL_TYPE_BIT:
      CTC_RETURN_IF_ERROR(ctc_get_bit_default_value(thd, column, field, fld, col_obj, mem_start, mem_end, is_expr_value), false);
      break;
    case MYSQL_TYPE_BLOB:
      is_blob_type = (column->datatype->datatype != CTC_DDL_TYPE_CLOB);
    case MYSQL_TYPE_JSON:
      if (!is_expr_value) {
        char *default_value = const_cast<char *>(col_obj->default_value_utf8().data());
        int len = strlen(default_value);
        column->default_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, len + 1);
        if (column->default_text == nullptr) {
          ctc_log_error("alloc mem for json default text failed, mem_start is null");
          return false;
        }
        strncpy(column->default_text, default_value, len + 1);
        break;
      }
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
      CTC_RETURN_IF_ERROR(ctc_get_string_default_value(column, field, col_obj, fld, mem_start, mem_end, is_blob_type), false);
      break;
    case MYSQL_TYPE_ENUM:
      CTC_RETURN_IF_ERROR(ctc_get_enum_default_value(thd, column, col_obj, field, fld, mem_start, mem_end, field_cs), false);
      break;
    case MYSQL_TYPE_SET:
      CTC_RETURN_IF_ERROR(ctc_get_set_default_value(thd, column, field, fld, mem_start, mem_end, field_cs), false);
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_TIME2:
      CTC_RETURN_IF_ERROR(ctc_get_datetime_default_value(column, field, fld, col_obj, mem_start, mem_end, option_set, is_expr_value), false);
      break;
    default:
      CTC_RETURN_IF_ERROR(ctc_get_numeric_default_value(column, field, col_obj, fld, mem_start, mem_end, is_expr_value), false);
      break;
  }
  return true;
}

static void ctc_fill_column_option_set(TcDb__CtcDDLColumnDef *column, Field *field,
                                       TABLE *form, ctc_column_option_set_bit *option_set) {
  column->is_option_set = 0;
  option_set->is_option_set= 0;
  option_set->primary = false;
  option_set->is_default_null = false;
  option_set->has_null = true; // 保证nullable的值是准确的
  option_set->nullable = (field->is_flag_set(NOT_NULL_FLAG)) ? 0 : 1;
  /*
    For virtual generated columns, set flag is_virtual and no store.
    For virtual stored columns, as regular stored columns.
  */
  option_set->is_virtual = field->is_virtual_gcol() ? 1 : 0;

  if (field->is_flag_set(PRI_KEY_FLAG)) {
    option_set->primary = true;
    option_set->has_null = true;
    option_set->nullable = false;
  }
  option_set->unique = field->is_flag_set(UNIQUE_KEY_FLAG);
  column->cons_name = option_set->primary ? const_cast<char *>("PRIMARY") :
        (option_set->unique ? column->name : nullptr);
  option_set->is_serial = field->is_flag_set(AUTO_INCREMENT_FLAG);
  option_set->is_comment = field->comment.length > 0;
  column->comment = option_set->is_comment ? const_cast<char *>(field->comment.str) : nullptr;

  /*处理自增列为unique key的情况*/
  if(option_set->is_serial) {
    for (uint i = 0; i < form->s->keys; i++) {
      if (strcmp(column->name, form->s->keynames.type_names[i]) == 0) {
        option_set->unique = true;
        option_set->primary = false;
        column->cons_name = column->name;
      }
    }
  }
  const field_cnvrt_aux_t *mysql_info = get_auxiliary_for_field_convert(field, field->real_type());
  if (mysql_info->sql_data_type == STRING_DATA || mysql_info->sql_data_type == LOB_DATA) {
    column->collate = ctc_get_column_cs(field->charset());
    option_set->is_collate = 1;
  }
}

static bool ctc_ddl_fill_column_by_field(
    THD *thd, TcDb__CtcDDLColumnDef *column, Field *field,
    const dd::Table *table_def, TABLE *form, const Create_field *fld,
    ctc_alter_column_alter_mode alter_mode, char **mem_start, char *mem_end, const CHARSET_INFO *field_cs) {
  const dd::Column *col_obj = NULL;
  col_obj = table_def ? table_def->get_column(field->field_name) : nullptr; // create view中创临时表，table_def为空
  /*
    We need this to get default values from the table
    We have to restore the read_set if we are called from insert in case
    of row based replication.
  */
  my_bitmap_map *old_map = tmp_use_all_columns(form, form->read_set);
  auto grd = create_scope_guard(
      [&]() { tmp_restore_column_map(form->read_set, old_map); });
  if (fld != NULL && fld->change != NULL) {
      column->name = const_cast<char *>(fld->change);
      if (strcmp(fld->change, field->field_name) != 0) {
        column->new_name = const_cast<char *>(field->field_name);
      }
  } else {
      column->name = const_cast<char *>(field->field_name);
  }
  
  CTC_RETURN_IF_ERROR(ctc_ddl_fill_column_by_field_fill_type(column, field), false);

  ctc_column_option_set_bit option_set;
  ctc_fill_column_option_set(column, field, form, &option_set);

  if (option_set.is_virtual) {
    /*
    The expression of virtual generated columns is unused in engine,
    it is used to store the location in SYS_COLUMNS.
    The default value of virtual column depended on expression.
    */
    option_set.is_default = 0;
    option_set.is_default_null = 0;
  } else if (ctc_is_with_default_value(field, col_obj)) {
    CTC_RETURN_IF_ERROR(ctc_ddl_fill_column_default_value(thd, column, field, fld, col_obj,
                                                          &option_set, mem_start, mem_end, field_cs), false);
  } else {
    option_set.is_default = 0;
    option_set.is_default_func = 0;
    option_set.is_curr_timestamp = 0;
    option_set.is_default_null = 1;
  }
  // 这句代码要放在所有设置option_set的后面
  column->is_option_set = option_set.is_option_set;
  column->alter_mode = alter_mode; // ctc_alter_column_alter_mode
  return true;
}

static int ctc_ddl_alter_table_fill_foreign_key_info(TcDb__CtcDDLForeignKeyDef *fk_def, const Foreign_key_spec *fk,
                                                     char **mem_start, char *mem_end)
{
  CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(fk->name.str));
  fk_def->name = const_cast<char *>(fk->name.str);
  fk_def->delete_opt = ctc_ddl_get_foreign_key_rule(fk->delete_opt);
  fk_def->update_opt = ctc_ddl_get_foreign_key_rule(fk->update_opt);
  size_t buf_len = fk->ref_db.length + 1;
  fk_def->referenced_table_schema_name = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, buf_len);
  if (fk_def->referenced_table_schema_name == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  ctc_copy_name(fk_def->referenced_table_schema_name, fk->ref_db.str, buf_len);
  fk_def->referenced_table_name =
      const_cast<char *>(fk->ref_table.str);
  for (uint j = 0; j < fk_def->n_elements; j++) {
    TcDb__CtcDDLForeignKeyElementDef *fk_ele = fk_def->elements[j];
    fk_ele->src_column_name =
        const_cast<char *>(fk->columns[j]->get_field_name());
    fk_ele->ref_column_name =
        const_cast<char *>(fk->ref_columns[j]->get_field_name());
  }
  return CT_SUCCESS;
}

static int ctc_ddl_create_table_fill_foreign_key_info(TcDb__CtcDDLCreateTableDef *req,
  const dd::Table *table_def, char **mem_start, char *mem_end) {
  if (req->n_fk_list == 0) {
    return CT_SUCCESS;
  }
  for (uint i = 0; i < req->n_fk_list; i++) {
    TcDb__CtcDDLForeignKeyDef *fk_def = req->fk_list[i];
    assert(table_def != nullptr);
    const dd::Foreign_key *fk = table_def->foreign_keys().at(i);
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(fk->name().data()));
    fk_def->name = const_cast<char *>(fk->name().data());

    fk_def->delete_opt = ctc_ddl_get_foreign_key_rule(fk->delete_rule());
    fk_def->update_opt = ctc_ddl_get_foreign_key_rule(fk->update_rule());
    size_t buf_len = fk->referenced_table_schema_name().length() + 1;
    fk_def->referenced_table_schema_name = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, buf_len);
    if (fk_def->referenced_table_schema_name == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }
    ctc_copy_name(fk_def->referenced_table_schema_name, fk->referenced_table_schema_name().data(), buf_len);
    fk_def->referenced_table_name =
        const_cast<char *>(fk->referenced_table_name().data());
    for (uint j = 0; j < fk_def->n_elements; j++) {
      TcDb__CtcDDLForeignKeyElementDef *fk_ele = fk_def->elements[j];
      const dd::Foreign_key_element *fk_col_obj = fk->elements().at(j);
      fk_ele->src_column_name =
          const_cast<char *>(fk_col_obj->column().name().data());
      fk_ele->ref_column_name =
          const_cast<char *>(fk_col_obj->referenced_column_name().data());
    }
  }
  return CT_SUCCESS;
}

static void ctc_fill_prefix_func_key_part(TcDb__CtcDDLTableKeyPart *req_key_part,
                                          const Field *field, uint16 prefix_len) {
  req_key_part->is_func = true;
  if (field->charset() == &my_charset_bin && field->is_flag_set(BINARY_FLAG)) {
    req_key_part->func_name = const_cast<char *>("substrb");
    snprintf(req_key_part->func_text, FUNC_TEXT_MAX_LEN - 1, "substrb(%s,1,%d)",
            field->field_name, prefix_len);
  } else {
    req_key_part->func_name = const_cast<char *>("substr");
    snprintf(req_key_part->func_text, FUNC_TEXT_MAX_LEN - 1, "substr(%s,1,%d)",
            field->field_name, prefix_len);
  }
  return;
}

struct FuncMapping {
    std::string mysql_func_name;
    std::string ctc_func_name;
};

static const FuncMapping mysql_func_name_to_ctc_map[] = {
    {"%", "mod"},
    {"cast_as", "cast"},
    {"ceiling", "ceil"},
    {"locate", "position"}
};

static const size_t func_map_size = sizeof(mysql_func_name_to_ctc_map) / sizeof(mysql_func_name_to_ctc_map[0]);

static int ctc_check_func_name(Item_func *func_expr_item)
{
    std::string func_name = func_expr_item->func_name();
    size_t func_name_len = func_name.length();

    for (size_t i = 0; i < func_map_size; i++) {
        const std::string mysql_func_name = mysql_func_name_to_ctc_map[i].mysql_func_name;
        if (func_name_len >= mysql_func_name.length() &&
            func_name.compare(0, mysql_func_name.length(), mysql_func_name) == 0) {
            my_printf_error(ER_DISALLOWED_OPERATION, "Function %s is not indexable", MYF(0),
                            mysql_func_name_to_ctc_map[i].ctc_func_name.c_str());
            return CT_ERROR;
        }
    }

    for (uint32_t i = 0; i < func_expr_item->arg_count; i++) {
        if (func_expr_item->get_arg(i)->type() == Item::FUNC_ITEM) {
            // nested function, need to do check recursively, error emit has been done on-the-spot
            CTC_RETURN_IF_NOT_ZERO(ctc_check_func_name((Item_func *) func_expr_item->get_arg(i)));
        }
    }
    return CT_SUCCESS;
}

static int recursively_get_dependency_item(TABLE *form, Item_func *item_func, TcDb__CtcDDLTableKeyPart *req_key_part,
                                           uint32_t *col_item_count)
{
    for (uint32_t i = 0; i < item_func->arg_count; i++) {
        if (item_func->get_arg(i)->type() == Item::FUNC_ITEM) {
            // nested function, would go recursively to do detection
            int result = recursively_get_dependency_item(form, (Item_func *) item_func->get_arg(i),
                                                         req_key_part, col_item_count);
            if (result != CT_SUCCESS) {
                return result;
            }
        }

        if (item_func->get_arg(i)->type() == Item::FIELD_ITEM) {
            // the field* args[i] contains don't have proper m_field_index when it's a alter table scenario.
            //  thus we have to look up by name through metadata on the new TABLE*
            Item_field *arg_item_field = (Item_field*) item_func->get_arg(i);
            Field *field = ctc_get_field_by_name(form, arg_item_field->field->field_name);
            if (field && field->is_virtual_gcol()) {
                my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                                "Cantian does not support index on virtual generated column.");
                return CT_ERROR;
            }

            if (*col_item_count >= 1) {
                my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                                "Cantian does not support function indexes with multiple columns of arguments.");
                return CT_ERROR;
            }
            req_key_part->name = const_cast<char *>(item_func->get_arg(i)->item_name.ptr());
            (*col_item_count)++;
        }
    }
    return CT_SUCCESS;
}

void ctc_rewrite_expression_clause(TABLE *form, const THD *thd, Item *item, String *out);

void ctc_print_on_empty_or_error(TABLE *form, const THD *thd, String *out, bool on_empty,
                                 Json_on_response_type response_type, Item *default_string)
{
    switch (response_type) {
        case Json_on_response_type::ERROR:
            out->append(STRING_WITH_LEN(" error"));
            break;
        case Json_on_response_type::NULL_VALUE:
            out->append(STRING_WITH_LEN(" null"));
            break;
        case Json_on_response_type::DEFAULT:
            out->append(STRING_WITH_LEN(" default "));
            ctc_rewrite_expression_clause(form, thd, default_string, out);
            break;
        case Json_on_response_type::IMPLICIT:
            // Nothing to print when the clause was implicit.
            return;
    };

    if (on_empty) {
        out->append(STRING_WITH_LEN(" on empty"));
    } else {
        out->append(STRING_WITH_LEN(" on error"));
    }
}

#ifdef METADATA_NORMALIZED
#define JSON_VALUE_ON_EMPTY_ARG_IDX 2
#define JSON_VALUE_ON_ERROR_ARG_IDX 3

// equivalent to Item_func_json_value::print, but stripped charset info
void ctc_item_print_json_value(TABLE *form, const THD *thd, Item* item, String *out)
{
    Item_func_json_value *item_func = (Item_func_json_value *) item;
    out->append(STRING_WITH_LEN("json_value("));
    ctc_rewrite_expression_clause(form, thd, item_func->get_arg(0), out);
    out->append(STRING_WITH_LEN(", "));
    ctc_rewrite_expression_clause(form, thd, item_func->get_arg(1), out);
    out->append(STRING_WITH_LEN(" returning "));
    if (item_func->m_cast_target == ITEM_CAST_CHAR && item_func->collation.collation != &my_charset_bin) {
        // don't add char, use CLOB instead
        out->append(STRING_WITH_LEN("CLOB"));
    } else {
        print_cast_type(item_func->m_cast_target, item_func, out);
    }
    ctc_print_on_empty_or_error(form, thd, out, true, item_func->m_on_empty,
                                item_func->get_arg(JSON_VALUE_ON_EMPTY_ARG_IDX));
    ctc_print_on_empty_or_error(form, thd, out, false, item_func->m_on_error,
                                item_func->get_arg(JSON_VALUE_ON_ERROR_ARG_IDX));
    out->append(')');
};
#else
// with not normalized mode, we don't have patch on mysql source code, so fallback to the old implmentation route
// based on regular expression replacement. the conversion for parameter of json_value won't be precise as normal path
void ctc_item_print_json_value(TABLE *form MY_ATTRIBUTE((unused)), const THD *thd, Item* item, String *out)
{
    Item_func_json_value *item_func = (Item_func_json_value *) item;
    char buffer[FUNC_TEXT_MAX_LEN] = {0};
    String gc_expr(buffer, sizeof(buffer), &my_charset_bin);
    item_func->print(thd, &gc_expr, enum_query_type(QT_WITHOUT_INTRODUCERS | QT_NO_DB | QT_NO_TABLE));
    string expr_str(buffer);
    expr_str.erase(remove(expr_str.begin(), expr_str.end(), '`'), expr_str.end());
    // 处理json_value建索引，只允许returning char
    // 不带returning默认char512
    std::regex reg_char("returning[ ]char[(]\\d+[)]");
    std::regex reg_charset("[_][a-z]+[0-9]*[a-z]*[0-9]*['$]");
    std::regex reg_charset2("[ ]character[ ]set[ ][a-z]+[0-9]*[a-z]*[0-9]");
    expr_str = std::regex_replace(expr_str, reg_char, "returning CLOB");
    expr_str = std::regex_replace(expr_str, reg_charset, "'");
    // 处理char带charset设置
    expr_str = std::regex_replace(expr_str, reg_charset2, "");
    out->append(expr_str.c_str());
    return ;
};
#endif

std::map<std::string, ctc_item_print_t> item_func_printer = {
    {"json_value", (ctc_item_print_t) ctc_item_print_json_value}
};

static void ctc_print_op(TABLE *form, const THD *thd, Item_func *item_func, String *out)
{
    out->append('(');
    for (uint i = 0; i < item_func->arg_count - 1; i++) {
        ctc_rewrite_expression_clause(form, thd, item_func->get_arg(i), out);
        out->append(' ');
        out->append(item_func->func_name());
        out->append(' ');
    }
    ctc_rewrite_expression_clause(form, thd, item_func->get_arg(item_func->arg_count - 1), out);
    out->append(')');
}

static void ctc_print_func(TABLE *form, const THD *thd, Item_func *item_func, String *out)
{
    if (item_func->collation.collation == &my_charset_bin && strcmp(item_func->func_name(), "substr") == 0) {
        out->append("substrb");
    } else {
        out->append(item_func->func_name());
    }
    out->append('(');
    for (uint i = 0; i < item_func->arg_count; i++) {
        if (i != 0) out->append(',');
        ctc_rewrite_expression_clause(form, thd, item_func->get_arg(i), out);
    }
    out->append(')');
}

// the origin implementation of Item_field::print would add extra back quote (`) when it's called,
// due to append_identifier() would explicitly use get_quote_char_for_identifier() to get quote char.
// meanwhile ctsql don't have support for that, ctc_print_field shall be used instead.

static void ctc_print_field(TABLE *form MY_ATTRIBUTE((unused)), const THD *thd MY_ATTRIBUTE((unused)),
                            Item_ident *item_ident, String *out)
{
    // equivalent to append_identifier(thd, out, item_ident->field_name, strlen(item_ident->field_name), NULL, NULL)
    // but without quote_char support
    out->append(item_ident->field_name, strlen(item_ident->field_name), system_charset_info);
}

// this expression rewrite process may only happen once during DDL, performance is not major concern
void ctc_rewrite_expression_clause(TABLE *form, const THD *thd, Item *item, String *out)
{
    if (item == nullptr) {
        return;
    }
    Item::Type item_type = item->type();
    if (item_type == Item::Type::FIELD_ITEM) {
        ctc_print_field(form, thd, (Item_ident*) item, out);
        return;
    }

    // for function item, in order to intercept all print() call to its args, a ctc_print_func is used instead of
    //  the built-in print()
    if (item_type == Item::Type::FUNC_ITEM) {
        std::string func_name_string = std::string(((Item_func *)item)->func_name());
        if (print_op_func_name.find(func_name_string) != print_op_func_name.end()) {
            ctc_print_op(form, thd, (Item_func*) item, out);
        } else if (item_func_printer.find(func_name_string) != item_func_printer.end()) {
            ctc_item_print_t printer = item_func_printer[func_name_string];
            printer(form, thd, (Item_func *)item, out);
        } else {
            ctc_print_func(form, thd, (Item_func *)item, out);
        }
        return;
    }

    // otherwise, it should be int literals / string literals etc, use the built-in print
    // but with QT_WITHOUT_INTRODUCERS so we don't have introducers like _utf8mb4, which is not supported by ctsql
    item->print(thd, out, enum_query_type(QT_WITHOUT_INTRODUCERS | QT_NO_DB | QT_NO_TABLE));
}

static int ctc_fill_func_key_part(TABLE *form, THD *thd,
                                  TcDb__CtcDDLTableKeyPart *req_key_part, Value_generator *gcol_info)
{
    Item_func *func_expr_item = dynamic_cast<Item_func *>(gcol_info->expr_item);
    if (func_expr_item == nullptr) {
        my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                        "[CTC_CREATE_TABLE]: CTC do not support this functional index.");
        return CT_ERROR;
    }

    uint32_t arg_count = func_expr_item->arg_count;
    if (arg_count == 0) {
        my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                        "[CTC_CREATE_TABLE]: There is no functional index.");
        return CT_ERROR;
    }

    if (ctc_check_func_name(func_expr_item) != CT_SUCCESS) {
        return CT_ERROR;
    }

    req_key_part->is_func = true;
    req_key_part->func_name = const_cast<char *>(func_expr_item->func_name());

    uint32_t col_item_count = 0;
    // recursively_get_dependency_item fill req_key_part->name
    int result = recursively_get_dependency_item(form, func_expr_item, req_key_part, &col_item_count);
    if (result != CT_SUCCESS || col_item_count != 1) {
        my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                        "[CTC_CREATE_TABLE]:  CTC do not support this functional index.");
        return CT_ERROR;
    }

    char buffer[FUNC_TEXT_MAX_LEN] = {0};
    String gc_expr(buffer, sizeof(buffer), &my_charset_bin);

    gc_expr.length(0);
    ctc_rewrite_expression_clause(form, thd, gcol_info->expr_item, &gc_expr);
    strncpy(req_key_part->func_text, gc_expr.c_ptr(), FUNC_TEXT_MAX_LEN - 1);
    return CT_SUCCESS;
}

static inline longlong get_session_level_create_index_parallelism(THD *thd)
{
  return get_session_variable_int_value_with_range(
      thd, SESSION_VARIABLE_NAME_CREATE_INDEX_PARALLELISM,
      SESSION_VARIABLE_VALUE_MIN_CREATE_INDEX_PARALLELISM,
      SESSION_VARIABLE_VALUE_MAX_CREATE_INDEX_PARALLELISM,
      SESSION_VARIABLE_VALUE_DEFAULT_CREATE_INDEX_PARALLELISM);
}

static inline int initialize_ddl_table_key_req_key_def(TcDb__CtcDDLTableKey *req_key_def, char* user,
                                                       TcDb__CtcDDLCreateTableDef *req, const char *name)
{
  req_key_def->user = user;
  req_key_def->table = req->name;
  CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(name));
  req_key_def->name = const_cast<char *>(name);
  req_key_def->space = NULL;
  return 0;
}

static bool ctc_ddl_create_table_fill_add_key_table_def_null_fill_key_part(TcDb__CtcDDLTableKey *req_key_def,
    THD *thd, TABLE *form, TcDb__CtcDDLTableKeyPart *req_key_part, KEY_PART_INFO *key_part)
{
  if (key_part->key_part_flag & HA_REVERSE_SORT) {
    req_key_def->is_dsc = true;
  }
  bool is_prefix_key = false;
  cm_assert(key_part != NULL);
  Field *fld = form->field[key_part->field->field_index()];
  cm_assert(fld != nullptr);
  if (fld->is_field_for_functional_index()) {
    req_key_def->is_func = true;
    CTC_RETURN_IF_ERROR(ctc_fill_func_key_part(form, thd, req_key_part, fld->gcol_info) == CT_SUCCESS, false);
  } else {
    if (fld->is_virtual_gcol()) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                      "Cantian does not support index on virtual generated column.");
      return false;
    }

    uint prefix_len = get_prefix_index_len(fld, key_part->length);
    if (prefix_len) {
      ctc_fill_prefix_func_key_part(req_key_part, fld, prefix_len);
      is_prefix_key = true;
    } else {
      req_key_part->is_func = false;
      req_key_part->func_text = nullptr;
    }
    req_key_part->name = const_cast<char *>(key_part->field->field_name);
  }
  req_key_part->length = key_part->length;
  ctc_ddl_get_data_type_from_mysql_type(fld, fld->type(), &req_key_part->datatype);
  ctc_set_unsigned_column(fld, &req_key_part->is_unsigned);
  if (is_prefix_key && fld->is_flag_set(BLOB_FLAG)) {
    req_key_part->datatype = CTC_DDL_TYPE_VARCHAR;
  }
  return true;
}

static bool ctc_ddl_create_table_fill_add_key_table_def_null(TcDb__CtcDDLCreateTableDef *req, THD *thd,
                                                             TABLE *form, char *user)
{
  for (uint i = 0; i < req->n_key_list; i++) {
    TcDb__CtcDDLTableKey *req_key_def = req->key_list[i];
    const KEY *key = form->key_info + i;
    if (key->key_length == 0) {
      my_error(ER_WRONG_KEY_COLUMN, MYF(0), key->key_part->field->field_name);
      return ER_WRONG_KEY_COLUMN;
    }
    CTC_RETURN_IF_NOT_ZERO(initialize_ddl_table_key_req_key_def(req_key_def, user, req, key->name));
    CTC_RETURN_IF_ERROR(get_ctc_key_type(key, &req_key_def->key_type), false);
    CTC_RETURN_IF_ERROR(get_ctc_key_algorithm(key->algorithm, &req_key_def->algorithm), false);
    if (req_key_def->key_type == CTC_KEYTYPE_PRIMARY || req_key_def->key_type == CTC_KEYTYPE_UNIQUE) {
      req_key_def->is_constraint = true;
    }
    for (uint j = 0; j < req_key_def->n_columns; j++) {
      TcDb__CtcDDLTableKeyPart *req_key_part = req_key_def->columns[j];
      KEY_PART_INFO *key_part = key->key_part + j;
      if (!ctc_ddl_create_table_fill_add_key_table_def_null_fill_key_part(
          req_key_def, thd, form, req_key_part, key_part)) {
        return false;
      }
    }
    req_key_def->parallelism = get_session_level_create_index_parallelism(thd);
  }
  return true;
}

static inline bool ctc_ddl_create_table_fill_add_key_table_def_not_null_fill_key_part(TcDb__CtcDDLTableKey *req_key_def,
    THD *thd, TABLE *form, TcDb__CtcDDLTableKeyPart *req_key_part, const dd::Index_element *key_part)
{
  if (key_part->order() == dd::Index_element::ORDER_DESC) {
    req_key_def->is_dsc = true;
  }
  bool is_prefix_key = false;
  cm_assert(key_part != NULL);
  Field *fld = ctc_get_field_by_name(form, key_part->column().name().data());
  cm_assert(fld != nullptr);

  if (fld->is_field_for_functional_index()) {
    req_key_def->is_func = true;
    CTC_RETURN_IF_ERROR(ctc_fill_func_key_part(form, thd, req_key_part, fld->gcol_info) == CT_SUCCESS, false);
  } else {
    if (fld->is_virtual_gcol()) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                      "Cantian does not support index on virtual generated column.");
      return false;
    }

    uint prefix_len = get_prefix_index_len(fld, key_part->length());
    if (prefix_len) {
      ctc_fill_prefix_func_key_part(req_key_part, fld, prefix_len);
      is_prefix_key = true;
    } else {
      req_key_part->is_func = false;
      req_key_part->func_text = nullptr;
    }
    req_key_part->name = const_cast<char *>(key_part->column().name().data());
  }
  req_key_part->length = key_part->length();
  ctc_ddl_get_data_type_from_mysql_type(fld, fld->type(), &req_key_part->datatype);
  ctc_set_unsigned_column(fld, &req_key_part->is_unsigned);
  if (is_prefix_key && fld->is_flag_set(BLOB_FLAG)) {
    req_key_part->datatype = CTC_DDL_TYPE_VARCHAR;
  }
  return true;
}

static inline bool ctc_ddl_create_table_fill_add_key_table_def_not_null(TcDb__CtcDDLCreateTableDef *req, THD *thd,
                                                                        const dd::Table *table_def, TABLE *form,
                                                                        char *user)
{
  for (uint i = 0; i < req->n_key_list; i++) {
    TcDb__CtcDDLTableKey *req_key_def = req->key_list[i];
    assert(table_def != nullptr);
    const dd::Index *idx = table_def->indexes().at(i);
    CTC_RETURN_IF_NOT_ZERO(initialize_ddl_table_key_req_key_def(req_key_def, user, req, idx->name().data()));
    CTC_RETURN_IF_ERROR(ctc_ddl_get_create_key_type(idx->type(), &req_key_def->key_type), false);
    CTC_RETURN_IF_ERROR(ctc_ddl_get_create_key_algorithm(idx->algorithm(), &req_key_def->algorithm), false);

    for (uint j = 0; j < req_key_def->n_columns; j++) {
      TcDb__CtcDDLTableKeyPart *req_key_part = req_key_def->columns[j];
      const dd::Index_element *key_part = idx->elements().at(j);
      if (!ctc_ddl_create_table_fill_add_key_table_def_not_null_fill_key_part(
          req_key_def, thd, form, req_key_part, key_part)) {
        return false;
      }
    }
    req_key_def->parallelism = get_session_level_create_index_parallelism(thd);
  }
  return true;
}

static bool ctc_ddl_create_table_fill_add_key(TcDb__CtcDDLCreateTableDef *req, THD *thd,
                                              const dd::Table *table_def, TABLE *form, char *user)
{
  if (req->n_key_list == 0) {
    return true;
  }

  if (table_def == nullptr) {
    return ctc_ddl_create_table_fill_add_key_table_def_null(req, thd, form, user);
  }
  return ctc_ddl_create_table_fill_add_key_table_def_not_null(req, thd, table_def, form, user);
}

static int ctc_ddl_fill_partition_table_info(const dd::Partition *pt, char **mem_start, char *mem_end,
  TcDb__CtcDDLPartitionDef *part_def, uint32_t part_id)
{
  TcDb__CtcDDLPartitionTableDef *part_table = part_def->part_table_list[part_id];
  part_table->name = const_cast<char *>(pt->name().data());
  part_table->n_subpart_table_list = pt->subpartitions().size();
  if (part_table->n_subpart_table_list > 0) {
    part_table->subpart_table_list =
      (TcDb__CtcDDLPartitionTableDef **)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionTableDef *) * pt->subpartitions().size());
    if (part_table->subpart_table_list == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    TcDb__CtcDDLPartitionTableDef **subpart_table_list = part_table->subpart_table_list;
    uint32_t i = 0;
    for (const dd::Partition *sub_part_obj : pt->subpartitions()) {
      subpart_table_list[i] = (TcDb__CtcDDLPartitionTableDef *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionTableDef));
      if (subpart_table_list[i] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddlpartition_table_def__init(subpart_table_list[i]);
      TcDb__CtcDDLPartitionTableDef *sub_part_table = part_table->subpart_table_list[i];
      sub_part_table->name = const_cast<char *>(sub_part_obj->name().data());
      i++;
    }
  }
  return 0;
}
 
static int ctc_ddl_prepare_create_partition_info(TcDb__CtcDDLCreateTableDef *req, const dd::Table *table_def,
  char **mem_start, char *mem_end)
{
  req->partition_def = (TcDb__CtcDDLPartitionDef *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionDef));
  if (req->partition_def == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  tc_db__ctc_ddlpartition_def__init(req->partition_def);
  int ret = convert_ctc_part_type(table_def->partition_type(), &req->partition_def->part_type);
  if (ret != 0) {
    return ret;
  }

  ret = convert_ctc_subpart_type(table_def->subpartition_type(), &req->partition_def->subpart_type);
  if (ret != 0) {
    return ret;
  }

  req->partition_def->n_part_table_list = table_def->partitions().size();

  req->partition_def->part_table_list =
    (TcDb__CtcDDLPartitionTableDef **)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionTableDef *) * req->partition_def->n_part_table_list);
  if (req->partition_def->part_table_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  TcDb__CtcDDLPartitionTableDef **part_table_list = req->partition_def->part_table_list;
  uint32_t i = 0;
  for (const dd::Partition *pt : table_def->partitions()) {
    part_table_list[i] = (TcDb__CtcDDLPartitionTableDef *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionTableDef));
    if (part_table_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }

    tc_db__ctc_ddlpartition_table_def__init(part_table_list[i]);
    CTC_RETURN_IF_NOT_ZERO(ctc_ddl_fill_partition_table_info(pt, mem_start, mem_end, req->partition_def, i));
    i++;
  }
 
  return 0;
}

static int ctc_ddl_init_column_def(TcDb__CtcDDLCreateTableDef *req, char **mem_start, char *mem_end) {
  req->columns = (TcDb__CtcDDLColumnDef **)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLColumnDef*) * req->n_columns);
  if (req->columns == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_columns; i++) {
    req->columns[i] = (TcDb__CtcDDLColumnDef *)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLColumnDef));
    if (req->columns[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlcolumn_def__init(req->columns[i]);
    TcDb__CtcDDLColumnDef *column = req->columns[i];
    column->datatype = (TcDb__CtcDDLColumnDataTypeDef *)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLColumnDataTypeDef));
    if (column->datatype == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }

    tc_db__ctc_ddlcolumn_data_type_def__init(column->datatype);
  }

  return 0;
}

static int ctc_ddl_init_foreign_key_def(TcDb__CtcDDLCreateTableDef *req, const dd::Table *table_def,
                                        char **mem_start, char *mem_end) {
  req->fk_list = (TcDb__CtcDDLForeignKeyDef **)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyDef*) * req->n_fk_list);
  if (req->fk_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_fk_list; i++) {
    req->fk_list[i] = (TcDb__CtcDDLForeignKeyDef *)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyDef));
    if (req->fk_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlforeign_key_def__init(req->fk_list[i]);
    TcDb__CtcDDLForeignKeyDef *fk_def = req->fk_list[i];
    const dd::Foreign_key *fk = table_def->foreign_keys().at(i);
    fk_def->n_elements = fk->elements().size();
    fk_def->elements = (TcDb__CtcDDLForeignKeyElementDef **)ctc_ddl_alloc_mem(
        mem_start, mem_end,
        sizeof(TcDb__CtcDDLForeignKeyElementDef*) * fk_def->n_elements);
    if (fk_def->elements == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    for (uint j = 0; j < fk_def->n_elements; j++) {
      fk_def->elements[j] = (TcDb__CtcDDLForeignKeyElementDef *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyElementDef));
      if (fk_def->elements[j] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddlforeign_key_element_def__init(fk_def->elements[j]);
    }
  }
  return 0;
}

static int ctc_ddl_init_index_def(TcDb__CtcDDLCreateTableDef *req, THD* thd, const dd::Table *table_def,
                                  char **mem_start, char *mem_end) {
  req->key_list = (TcDb__CtcDDLTableKey **)ctc_ddl_alloc_mem(
      mem_start, mem_end, sizeof(TcDb__CtcDDLTableKey*) * req->n_key_list);
  if (req->key_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_key_list; i++) {
    req->key_list[i] = (TcDb__CtcDDLTableKey *)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLTableKey));
    if (req->key_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    TcDb__CtcDDLTableKey *req_key = req->key_list[i];
    tc_db__ctc_ddltable_key__init(req_key);
    const dd::Index *idx = table_def->indexes().at(i);
    assert(idx != NULL);
    req_key->n_columns = idx->elements().size();
    req_key->columns = (TcDb__CtcDDLTableKeyPart **)ctc_ddl_alloc_mem(
        mem_start, mem_end, sizeof(TcDb__CtcDDLTableKeyPart*) * req_key->n_columns);
    if (req_key->columns == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    for (uint j = 0; j < req_key->n_columns; j++) {
      req_key->columns[j] = (TcDb__CtcDDLTableKeyPart *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLTableKeyPart));
      if (req_key->columns[j] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddltable_key_part__init(req_key->columns[j]);
      req_key->columns[j]->func_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, FUNC_TEXT_MAX_LEN);
      assert(req_key->columns[j]->func_text != NULL);
      memset(req_key->columns[j]->func_text, 0, FUNC_TEXT_MAX_LEN);
      req_key->parallelism = get_session_level_create_index_parallelism(thd);
    }
  }
  return 0;
}

static int ctc_ddl_init_index_form(TcDb__CtcDDLCreateTableDef *req, THD *thd, TABLE *form,
                                  char **mem_start, char *mem_end) {

  req->n_key_list = form->s->keys;
  if (req->n_key_list > 0) {
    req->key_list = (TcDb__CtcDDLTableKey **)ctc_ddl_alloc_mem(
      mem_start, mem_end,
      sizeof(TcDb__CtcDDLTableKey*) * req->n_key_list);
    if (req->key_list == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    for (uint i = 0; i < req->n_key_list; i++) {
      const KEY *key = form->key_info + i;
      if (key->key_length == 0) {
        my_error(ER_WRONG_KEY_COLUMN, MYF(0), key->key_part->field->field_name);
        return ER_WRONG_KEY_COLUMN;
      }
      req->key_list[i] = (TcDb__CtcDDLTableKey *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLTableKey));
      if (req->key_list[i] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      TcDb__CtcDDLTableKey *req_key = req->key_list[i];
      tc_db__ctc_ddltable_key__init(req_key);
      req_key->n_columns = key->user_defined_key_parts;
      req_key->columns = (TcDb__CtcDDLTableKeyPart **)ctc_ddl_alloc_mem(
          mem_start, mem_end,
          sizeof(TcDb__CtcDDLTableKeyPart*) * req_key->n_columns);
      if (req_key->columns == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      for (uint j = 0; j < req_key->n_columns; j++) {
        req_key->columns[j] = (TcDb__CtcDDLTableKeyPart *)ctc_ddl_alloc_mem(
            mem_start, mem_end, sizeof(TcDb__CtcDDLTableKeyPart));
        if (req_key->columns[j] == NULL) {
          return HA_ERR_OUT_OF_MEM;
        }
        tc_db__ctc_ddltable_key_part__init(req_key->columns[j]);
        req_key->columns[j]->func_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, FUNC_TEXT_MAX_LEN);
        assert(req_key->columns[j]->func_text != NULL);
        memset(req_key->columns[j]->func_text, 0, FUNC_TEXT_MAX_LEN);
      }
      req_key->parallelism = get_session_level_create_index_parallelism(thd);
    }
  }
  return 0;
}

static int ctc_ddl_init_create_table_def(TcDb__CtcDDLCreateTableDef *req,
                                 TABLE *form, THD *thd,
                                 const dd::Table *table_def, char **mem_start,
                                 char *mem_end) {
  uint fields = form->s->fields;
  tc_db__ctc_ddlcreate_table_def__init(req);
  DBUG_EXECUTE_IF("ctc_create_table_max_column", { fields = REC_MAX_N_USER_FIELDS + 1; });
  if (fields > REC_MAX_N_USER_FIELDS) {
    ctc_log_system("Max filed %d > %d, sql:%s", fields, REC_MAX_N_USER_FIELDS,
              thd->query().str);
    return HA_ERR_TOO_MANY_FIELDS;
  }
  req->n_columns = fields;
  if (req->n_columns > 0) {
    CTC_RETURN_IF_NOT_ZERO(ctc_ddl_init_column_def(req, mem_start, mem_end));
  }

  if (table_def == nullptr) {
    return ctc_ddl_init_index_form(req, thd, form, mem_start, mem_end);
  }

  req->n_key_list = table_def->indexes().size();
  if (req->n_key_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(ctc_ddl_init_index_def(req, thd, table_def, mem_start, mem_end));
  }

  req->n_fk_list = table_def->foreign_keys().size();
  if (req->n_fk_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(ctc_ddl_init_foreign_key_def(req, table_def, mem_start, mem_end));
  }

  if (table_def->partitions().size() > 0) {
    uint subpart_num_per_part = table_def->leaf_partitions().size() / table_def->partitions().size();
    if (subpart_num_per_part > MAX_SUBPART_NUM) {
      my_printf_error(ER_TOO_MANY_PARTITIONS_ERROR,
        "The number of subpartitions of one parent partition exceeds the maximum %d.", MYF(0), MAX_SUBPART_NUM);
      return -1;
    }
    int ret = ctc_ddl_prepare_create_partition_info(req, table_def, mem_start, mem_end);
    if (ret != 0) {
      ctc_log_system("ctc_ddl_prepare_create_partition_info failed , ret = %d.", ret);
      return ret;
    }
  }
  return 0;
}

int ha_ctc_truncate_table(ctc_handler_t *tch, THD *thd, const char *db_name, const char *table_name, bool is_tmp_table) {
  assert(thd->lex->sql_command == SQLCOM_TRUNCATE);

  ct_errno_t ret = CT_SUCCESS;
  size_t msg_len = 0;
  ctc_ddl_stack_mem stack_mem(0);
  void *ctc_ddl_req_msg_mem = nullptr;
  {
    TcDb__CtcDDLTruncateTableDef req;
    tc_db__ctc_ddltruncate_table_def__init(&req);
    char user_name[SMALL_RECORD_SIZE] = { 0 };
    ctc_copy_name(user_name, db_name, SMALL_RECORD_SIZE);
    req.schema = const_cast<char *>(user_name);
    req.name = const_cast<char *>(table_name);
    req.db_name = CTC_GET_THD_DB_NAME(thd);
    string sql;
    if (is_tmp_table) { // truncate临时表不需要广播
      req.sql_str = nullptr;
    } else {
      sql = string(thd->query().str).substr(0, thd->query().length);
      req.sql_str = const_cast<char *>(sql.c_str());
    }
    if (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS) {
      req.no_check_fk = 1;
    }
    msg_len = tc_db__ctc_ddltruncate_table_def__get_packed_size(&req);
    stack_mem.set_mem_size(msg_len + sizeof(ddl_ctrl_t));
    ctc_ddl_req_msg_mem = stack_mem.get_buf();
    if (ctc_ddl_req_msg_mem == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }

    if (tc_db__ctc_ddltruncate_table_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem) != msg_len) {
      assert(false);
    }
  }
  update_member_tch(*tch, ctc_hton, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, *tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ddl_ctrl.msg_len = msg_len;
  ret = (ct_errno_t)ctc_truncate_table(ctc_ddl_req_msg_mem, &ddl_ctrl);
  ctc_ddl_hook_cantian_error("ctc_truncate_table_cantian_error", thd, &ddl_ctrl, &ret);
  *tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(*tch, ctc_hton, thd);
  return ctc_ddl_handle_fault("ctc_truncate_table", thd, &ddl_ctrl, ret);
}

static int fill_create_table_req_base_info(HA_CREATE_INFO *create_info, char *db_name, char *table_name, THD *thd,
           TcDb__CtcDDLCreateTableDef *req, bool is_alter_copy, char **mem_start, char *mem_end) {
  req->schema = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, SMALL_RECORD_SIZE);
  if (req->schema == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  req->alter_db_name = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, SMALL_RECORD_SIZE);
  if (req->alter_db_name == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  ctc_copy_name(req->schema, db_name, SMALL_RECORD_SIZE);
  if (thd->lex->query_tables != nullptr) {
    ctc_copy_name(req->alter_db_name, thd->lex->query_tables->get_db_name(), SMALL_RECORD_SIZE);
  } else {
    ctc_copy_name(req->alter_db_name, db_name, SMALL_RECORD_SIZE);
  }
  req->name = table_name;
  CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(create_info->tablespace));
  if (create_info->tablespace && strcmp(req->schema, create_info->tablespace) != 0) {
    req->space = const_cast<char *>(create_info->tablespace);
  }

  if (is_alter_copy) {
    req->alter_table_name = const_cast<char *>(thd->lex->query_tables->table_name);
  } else {
    req->alter_table_name = table_name;
  }

  req->auto_increment_value = create_info->auto_increment_value;
  if (create_info->options & HA_LEX_CREATE_IF_NOT_EXISTS) {
    req->options |= CTC_CREATE_IF_NOT_EXISTS;
  }

  if (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS) {
    req->options |= CTC_CREATE_TYPE_NO_CHECK_CONS;
  }

  req->db_name = CTC_GET_THD_DB_NAME(thd);
  req->is_create_as_select = !thd->lex->query_block->field_list_is_empty();
  return 0;
}

static int fill_create_table_req_columns_info(HA_CREATE_INFO *create_info, dd::Table *table_def, TABLE *form,
           THD *thd, ddl_ctrl_t *ddl_ctrl, TcDb__CtcDDLCreateTableDef *req, char **mem_start, char *mem_end) {
  uint32_t ctc_col_idx = 0;
  uint32_t mysql_col_idx = 0;
  uint32_t virtual_cols = 0;
  while (ctc_col_idx < req->n_columns) {
    // When create table or alter copy algorithm, the vir_cols based on func_index at the last in the req
    Field *field = form->field[mysql_col_idx];
    // The vir_cols based on func_indexs are not pushed to engine
    if (field->is_field_for_functional_index()) {
      mysql_col_idx++;
      req->n_columns--;
      continue;
    }
    // Virtual Generated Columns Processed
    if (field->is_virtual_gcol()) {
      ddl_ctrl->table_flags |= CTC_FLAG_TABLE_CONTAINS_VIRCOL;
      virtual_cols++;
    }

    TcDb__CtcDDLColumnDef *column = req->columns[ctc_col_idx];
    const Create_field *fld_charset = ctc_get_create_field_by_column_name(thd, field->field_name);
    const CHARSET_INFO *field_cs = fld_charset != nullptr ? get_sql_field_charset(fld_charset, create_info) : nullptr;
    CTC_RETURN_IF_ERROR(
        ctc_ddl_fill_column_by_field(thd, column, field, table_def, form, NULL, CTC_ALTER_COLUMN_ALTER_ADD_COLUMN,
                                     mem_start, mem_end, field_cs), HA_ERR_WRONG_COMMAND);
    ctc_col_idx++;
    mysql_col_idx++;
  }

  // Prevent only virtual columns
  if (req->n_columns == virtual_cols) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "Cantian does not support all columns are generated.");
    return HA_ERR_WRONG_COMMAND;
  }

  return 0;
}

int fill_create_table_req(HA_CREATE_INFO *create_info, dd::Table *table_def, char *db_name, char *table_name,
                          TABLE *form, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem) {
  lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
  char *req_mem_start = ctc_ddl_req_mem;
  char *req_mem_end = req_mem_start + CTC_DDL_PROTOBUF_MEM_SIZE;
  TcDb__CtcDDLCreateTableDef req;
  int ret = ctc_ddl_init_create_table_def(&req, form, thd, table_def, &req_mem_start, req_mem_end);
  assert(req_mem_start <= req_mem_end);
  if (ret != 0) {
    return ret;
  }

  string sql;
  if (create_info->options & HA_LEX_CREATE_TMP_TABLE) { // 创建临时表不需要广播
    req.sql_str = nullptr;
  } else {
    sql = string(thd->query().str).substr(0, thd->query().length);
    req.sql_str = const_cast<char *>(sql.c_str());
  }
  ret = fill_create_table_req_base_info(create_info, db_name, table_name, thd, &req,
                                        ddl_ctrl->is_alter_copy, &req_mem_start, req_mem_end);
  if (ret != 0) {
    return ret;
  }
  
  assert(form->s->row_type == create_info->row_type);

  ret = fill_create_table_req_columns_info(create_info, table_def, form, thd, ddl_ctrl, &req, &req_mem_start, req_mem_end);
  if (ret != 0) {
    return ret;
  }

  CTC_RETURN_IF_NOT_ZERO(ctc_ddl_create_table_fill_foreign_key_info(&req, table_def, &req_mem_start, req_mem_end));
  CTC_RETURN_IF_ERROR(ctc_ddl_create_table_fill_add_key(&req, thd, table_def, form, req.schema), HA_ERR_WRONG_COMMAND);

  size_t msg_len = tc_db__ctc_ddlcreate_table_def__get_packed_size(&req);
  stack_mem->set_mem_size(msg_len + sizeof(ddl_ctrl_t));
  void *ctc_ddl_req_msg_mem = stack_mem->get_buf();
  if (ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  if (tc_db__ctc_ddlcreate_table_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem + sizeof(ddl_ctrl_t)) != msg_len) {
    assert(false);
  }

  ddl_ctrl->msg_len = msg_len + sizeof(ddl_ctrl_t);
  memcpy(ctc_ddl_req_msg_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  return 0;
}


static int ctc_ddl_fill_add_part_table_info(TcDb__CtcDDLPartitionTableDef *add_part, partition_element &part,
  char **mem_start, char *mem_end)
{
  int name_len = strlen(part.partition_name);
  add_part->name = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(char) * (name_len + 1));
  if (add_part->name == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  strncpy(add_part->name, part.partition_name, name_len + 1);
  add_part->n_subpart_table_list = part.subpartitions.size();
  if (part.subpartitions.size() > 0) {
    add_part->subpart_table_list =
    (TcDb__CtcDDLPartitionTableDef **)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionTableDef *) * part.subpartitions.size());
    if (add_part->subpart_table_list == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    TcDb__CtcDDLPartitionTableDef **subpart_table_list = add_part->subpart_table_list;
    uint32_t i = 0;
    for (partition_element sub_part_obj : part.subpartitions) {
      subpart_table_list[i] = (TcDb__CtcDDLPartitionTableDef *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(TcDb__CtcDDLPartitionTableDef));
      if (subpart_table_list[i] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddlpartition_table_def__init(subpart_table_list[i]);
      int name_len = strlen(sub_part_obj.partition_name);
      subpart_table_list[i]->name = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(char) * (name_len + 1));
      if (subpart_table_list[i]->name == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      strncpy(subpart_table_list[i]->name, sub_part_obj.partition_name, name_len + 1);
      i++;
    }
  }
  return 0;
}

static int ctc_prepare_alter_partition_init_part_list(TcDb__CtcDDLAlterTableDef *req, Alter_inplace_info *alter_info,
                                                      char **mem_start, char *mem_end) {
  List<partition_element> part_list = alter_info->modified_part_info->partitions;
  if (part_list.size() > PART_CURSOR_NUM) {
    my_error(ER_TOO_MANY_PARTITIONS_ERROR, MYF(0));
    return -1;
  }
  req->drop_partition_names = (char **)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(char *) * part_list.size());
  if (req->drop_partition_names == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  req->add_part_list = (TcDb__CtcDDLPartitionTableDef **)ctc_ddl_alloc_mem(mem_start, mem_end,
                        sizeof(TcDb__CtcDDLPartitionTableDef *) * part_list.size());
  if (req->add_part_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  return 0;
}

static int ctc_ddl_prepare_alter_partition_info(TcDb__CtcDDLAlterTableDef *req,
                                                Alter_inplace_info *alter_info, char **mem_start, char *mem_end)
{
  req->n_drop_partition_names = 0;
  req->n_add_part_list = 0;
  req->hash_coalesce_count = 0;
  if (alter_info->modified_part_info->partitions.size() <= 0) {
    return 0;
  }
  CTC_RETURN_IF_NOT_ZERO(ctc_prepare_alter_partition_init_part_list(req, alter_info, mem_start, mem_end));
  for (auto part : alter_info->modified_part_info->partitions) {
    switch (part.part_state) {
      case PART_TO_BE_DROPPED: {
        int name_len = strlen(part.partition_name);
        req->drop_partition_names[req->n_drop_partition_names] = (char *)ctc_ddl_alloc_mem(mem_start, mem_end,
                                                                  sizeof(char) * (name_len + 1));
        if (req->drop_partition_names[req->n_drop_partition_names] == NULL) {
          return HA_ERR_OUT_OF_MEM;
        }
        strncpy(req->drop_partition_names[req->n_drop_partition_names], part.partition_name, name_len + 1);
        req->n_drop_partition_names++;
        break;
      }
      case PART_TO_BE_ADDED: {
        req->add_part_list[req->n_add_part_list] = (TcDb__CtcDDLPartitionTableDef *)ctc_ddl_alloc_mem(mem_start, mem_end,
                                                    sizeof(TcDb__CtcDDLPartitionTableDef));
        if (req->add_part_list[req->n_add_part_list] == NULL) {
          return HA_ERR_OUT_OF_MEM;
        }
        TcDb__CtcDDLPartitionTableDef *add_part = req->add_part_list[req->n_add_part_list];
        tc_db__ctc_ddlpartition_table_def__init(add_part);
        CTC_RETURN_IF_NOT_ZERO(ctc_ddl_fill_add_part_table_info(add_part, part, mem_start, mem_end));
        req->n_add_part_list++;
        break;
      }
      case PART_REORGED_DROPPED: {
        req->hash_coalesce_count++;
        break;
      }
      case PART_CHANGED:
      case PART_NORMAL:
        break;
      default:
        my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "The current operation on a partitioned table is not supported.");
        return 1;
    }
  }
  return 0;
}

static int init_drop_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, char **mem_start, char *mem_end) {
  req->drop_list = (TcDb__CtcDDLAlterTableDrop **)ctc_ddl_alloc_mem(
                    mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTableDrop*) * req->n_drop_list);
  if (req->drop_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_drop_list; i++) {
    req->drop_list[i] = (TcDb__CtcDDLAlterTableDrop *)ctc_ddl_alloc_mem(
                         mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTableDrop));
    if (req->drop_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlalter_table_drop__init(req->drop_list[i]);
  }
  return 0;
}

static int init_alter_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, char **mem_start, char *mem_end) {
  req->alter_list = (TcDb__CtcDDLAlterTableAlterColumn **)ctc_ddl_alloc_mem(
                     mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTableAlterColumn*) * req->n_alter_list);
  if (req->alter_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_alter_list; i++) {
    req->alter_list[i] = (TcDb__CtcDDLAlterTableAlterColumn *)ctc_ddl_alloc_mem(
                          mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTableAlterColumn));
    if (req->alter_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlalter_table_alter_column__init(req->alter_list[i]);
  }
  return 0;
}

static int init_create_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, char **mem_start, char *mem_end) {
  req->create_list = (TcDb__CtcDDLColumnDef **)ctc_ddl_alloc_mem(
                      mem_start, mem_end, sizeof(TcDb__CtcDDLColumnDef*) * req->n_create_list);
  if (req->create_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_create_list; i++) {
    req->create_list[i] = (TcDb__CtcDDLColumnDef *)ctc_ddl_alloc_mem(
                           mem_start, mem_end, sizeof(TcDb__CtcDDLColumnDef));
    if (req->create_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlcolumn_def__init(req->create_list[i]);
    TcDb__CtcDDLColumnDef *column = req->create_list[i];
    column->datatype = (TcDb__CtcDDLColumnDataTypeDef *)ctc_ddl_alloc_mem(
                        mem_start, mem_end, sizeof(TcDb__CtcDDLColumnDataTypeDef));
    if (column->datatype == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }

    tc_db__ctc_ddlcolumn_data_type_def__init(column->datatype);
  }
  return 0;
}

static int init_add_key_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, THD *thd, Alter_inplace_info *ha_alter_info,
                                          char **mem_start, char *mem_end) {
  req->add_key_list = (TcDb__CtcDDLTableKey **)ctc_ddl_alloc_mem(
                       mem_start, mem_end, sizeof(TcDb__CtcDDLTableKey*) * req->n_add_key_list);
  if (req->add_key_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_add_key_list; i++) {
    req->add_key_list[i] = (TcDb__CtcDDLTableKey *)ctc_ddl_alloc_mem(
                            mem_start, mem_end, sizeof(TcDb__CtcDDLTableKey));
    if (req->add_key_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    TcDb__CtcDDLTableKey *req_key = req->add_key_list[i];
    tc_db__ctc_ddltable_key__init(req_key);
    const KEY *key = &ha_alter_info->key_info_buffer[ha_alter_info->index_add_buffer[i]];
    assert(key != NULL);
    req_key->n_columns = key->user_defined_key_parts;
    req_key->columns = (TcDb__CtcDDLTableKeyPart **)ctc_ddl_alloc_mem(
                        mem_start, mem_end, sizeof(TcDb__CtcDDLTableKeyPart*) * req_key->n_columns);
    if (req_key->columns == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    for (uint j = 0; j < req_key->n_columns; j++) {
      req_key->columns[j] = (TcDb__CtcDDLTableKeyPart *)ctc_ddl_alloc_mem(
                             mem_start, mem_end, sizeof(TcDb__CtcDDLTableKeyPart));
      if (req_key->columns[j] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddltable_key_part__init(req_key->columns[j]);
      req_key->columns[j]->func_text = (char *)ctc_ddl_alloc_mem(mem_start, mem_end, FUNC_TEXT_MAX_LEN);
      if (req_key->columns[j]->func_text == nullptr) {
        return HA_ERR_OUT_OF_MEM;
      }
      memset(req_key->columns[j]->func_text, 0, FUNC_TEXT_MAX_LEN);
    }
    req_key->parallelism = get_session_level_create_index_parallelism(thd);
  }
  return 0;
}

static int init_drop_key_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, char **mem_start, char *mem_end) {
  req->drop_key_list = (TcDb__CtcDDLAlterTableDropKey **)ctc_ddl_alloc_mem(
                        mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTableDropKey*) * req->n_drop_key_list);
  if (req->drop_key_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_drop_key_list; i++) {
    req->drop_key_list[i] = (TcDb__CtcDDLAlterTableDropKey *)ctc_ddl_alloc_mem(
                             mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTableDropKey));
    if (req->drop_key_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlalter_table_drop_key__init(req->drop_key_list[i]);
  }
  return 0;
}

static int init_foreign_key_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, Alter_inplace_info *ha_alter_info,
                                              char **mem_start, char *mem_end) {
  req->add_foreign_key_list = (TcDb__CtcDDLForeignKeyDef **)ctc_ddl_alloc_mem(
       mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyDef*) * ha_alter_info->alter_info->key_list.size());
  if (req->add_foreign_key_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  req->n_add_foreign_key_list = 0;
  for (const Key_spec *key : ha_alter_info->alter_info->key_list) {
    if (key->type != KEYTYPE_FOREIGN) {
      continue;
    }
    req->add_foreign_key_list[req->n_add_foreign_key_list] = (TcDb__CtcDDLForeignKeyDef *)ctc_ddl_alloc_mem(
         mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyDef));
    if (req->add_foreign_key_list[req->n_add_foreign_key_list] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlforeign_key_def__init(req->add_foreign_key_list[req->n_add_foreign_key_list]);
    TcDb__CtcDDLForeignKeyDef *fk_def = req->add_foreign_key_list[req->n_add_foreign_key_list];
    const Foreign_key_spec *fk = down_cast<const Foreign_key_spec *>(key);
    fk_def->n_elements = fk->columns.size();
    fk_def->elements = (TcDb__CtcDDLForeignKeyElementDef **)ctc_ddl_alloc_mem(
                        mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyElementDef*) * fk_def->n_elements);
    if (fk_def->elements == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    for (uint j = 0; j < fk_def->n_elements; j++) {
      fk_def->elements[j] = (TcDb__CtcDDLForeignKeyElementDef *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLForeignKeyElementDef));
      if (fk_def->elements[j] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddlforeign_key_element_def__init(fk_def->elements[j]);
    }
    CTC_RETURN_IF_NOT_ZERO(ctc_ddl_alter_table_fill_foreign_key_info(fk_def, fk, mem_start, mem_end));
    req->n_add_foreign_key_list++;
  }
  return 0;
}

static int init_alter_index_list_4alter_table(TcDb__CtcDDLAlterTableDef *req, char **mem_start, char *mem_end) {
  req->alter_index_list = (TcDb__CtcDDLAlterIndexDef **)ctc_ddl_alloc_mem(
       mem_start, mem_end, sizeof(TcDb__CtcDDLAlterIndexDef) * req->n_alter_index_list);
  if (req->alter_index_list == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  for (uint i = 0; i < req->n_alter_index_list; i++) {
    req->alter_index_list[i] = (TcDb__CtcDDLAlterIndexDef *)ctc_ddl_alloc_mem(
         mem_start, mem_end, sizeof(TcDb__CtcDDLAlterIndexDef));
    if (req->alter_index_list[i] == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    tc_db__ctc_ddlalter_index_def__init(req->alter_index_list[i]);
  }
  return 0;
}

static int init_ctc_ddl_alter_table_def(TcDb__CtcDDLAlterTableDef *req, Alter_inplace_info *ha_alter_info,
  THD *thd, TABLE *altered_table, char **mem_start, char *mem_end, size_t *rename_cols) {
  tc_db__ctc_ddlalter_table_def__init(req);
  uint32_t create_fields = (uint32_t)ha_alter_info->alter_info->create_list.size();
  DBUG_EXECUTE_IF("ctc_alter_table_max_column", { create_fields = REC_MAX_N_USER_FIELDS + 1; });
  if (create_fields > REC_MAX_N_USER_FIELDS) {
    ctc_log_system("Max filed %d > %u, sql:%s", (uint32_t)create_fields,
              REC_MAX_N_USER_FIELDS, thd->query().str);
    my_error(ER_TOO_MANY_FIELDS, MYF(0));
    return HA_ERR_TOO_MANY_FIELDS;
  }
  req->n_drop_list = (uint32_t)ha_alter_info->alter_info->drop_list.size();
  for (size_t i = 0; i < ha_alter_info->alter_info->alter_list.size(); i++) {
    const Alter_column *alter_column = ha_alter_info->alter_info->alter_list.at((size_t)i);
    if (alter_column->change_type() == Alter_column::Type::RENAME_COLUMN) {
      (*rename_cols)++;
    }
  }
  req->n_alter_list = (uint32_t)ha_alter_info->alter_info->alter_list.size() + *rename_cols;
  req->n_create_list = create_fields;
  req->n_add_key_list = ha_alter_info->index_add_count;
  req->n_drop_key_list = ha_alter_info->index_drop_count;
  req->n_alter_index_list = ha_alter_info->index_rename_count;

  // 分区表
  if (ha_alter_info->modified_part_info != NULL) {
    CTC_RETURN_IF_NOT_ZERO(ctc_ddl_prepare_alter_partition_info(req, ha_alter_info, mem_start, mem_end));
  }
  
  if (req->n_drop_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(init_drop_list_4alter_table(req, mem_start, mem_end));
  }

  
  if (req->n_alter_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(init_alter_list_4alter_table(req, mem_start, mem_end));
  }

  if (req->n_create_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(init_create_list_4alter_table(req, mem_start, mem_end));
  }

  req->table_def = (TcDb__CtcDDLAlterTablePorp *)ctc_ddl_alloc_mem(
                    mem_start, mem_end, sizeof(TcDb__CtcDDLAlterTablePorp));
  if (req->table_def == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }
  tc_db__ctc_ddlalter_table_porp__init(req->table_def);

  // 添加索引
  if ((ha_alter_info->handler_flags & Alter_inplace_info::ADD_STORED_BASE_COLUMN) &&
       thd->lex->sql_command == SQLCOM_ALTER_TABLE) {
    req->n_add_key_list = 0;
  }
  if (req->n_add_key_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(init_add_key_list_4alter_table(req, thd, ha_alter_info, mem_start, mem_end));
  }

  // 删除索引
  if (req->n_drop_key_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(init_drop_key_list_4alter_table(req, mem_start, mem_end));
  }

  // 增加外键
  if (ha_alter_info->handler_flags & Alter_inplace_info::ADD_FOREIGN_KEY) {
    CTC_RETURN_IF_NOT_ZERO(init_foreign_key_list_4alter_table(req, ha_alter_info, mem_start, mem_end));
  }

  // 自增
  if ((altered_table->found_next_number_field != nullptr)
    && (ha_alter_info->handler_flags & Alter_inplace_info::CHANGE_CREATE_OPTION)
    && (ha_alter_info->create_info->used_fields & HA_CREATE_USED_AUTO)) {
    req->new_auto_increment_value = ha_alter_info->create_info->auto_increment_value;
  }

  // alter index
  if (req->n_alter_index_list > 0) {
    CTC_RETURN_IF_NOT_ZERO(init_alter_index_list_4alter_table(req, mem_start, mem_end));
  }
  return 0;
}

/**
  Get Create_field object for newly created table by field index.

  @param alter_info  Alter_info describing newly created table.
  @param idx         Field index.
*/

static const Create_field *get_field_by_index(Alter_info *alter_info,
                                              uint idx) {
  List_iterator_fast<Create_field> field_it(alter_info->create_list);
  uint field_idx = 0;
  const Create_field *field = NULL;

  while ((field = field_it++) && field_idx < idx) {
    field_idx++;
  }

  return field;
}

static uint32_t ctc_fill_key_part(THD *thd,
                                  TcDb__CtcDDLTableKeyPart *req_key_part,
                                  TcDb__CtcDDLTableKey *req_key_def,
                                  const Create_field *create_field,
                                  TABLE *form,
                                  const KEY_PART_INFO *key_part) {
  Field *field = ctc_get_field_by_name(form, create_field->field_name);
  assert(field != nullptr);
  bool is_prefix_key = false;

  if (field->is_field_for_functional_index()) {
    req_key_def->is_func = true;
    CTC_RETURN_IF_ERROR(ctc_fill_func_key_part(form, thd, req_key_part, create_field->gcol_info) == CT_SUCCESS, CT_ERROR);
  } else {
    if (field->is_virtual_gcol()) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
        "Cantian does not support index on virtual generated column.");
      return CT_ERROR;
    }

    uint prefix_len = get_prefix_index_len(create_field->field, key_part->length);
    if (prefix_len) {
      ctc_fill_prefix_func_key_part(req_key_part, create_field->field, prefix_len);
      is_prefix_key = true;
    } else {
      req_key_part->is_func = false;
      req_key_part->func_text = nullptr;
    }
    req_key_part->name = const_cast<char *>(create_field->field_name);
  }

  ctc_ddl_get_data_type_from_mysql_type(field, create_field->sql_type, &req_key_part->datatype);
  ctc_set_unsigned_column(field, &req_key_part->is_unsigned);
  if (is_prefix_key && field->is_flag_set(BLOB_FLAG)) {
    req_key_part->datatype = CTC_DDL_TYPE_VARCHAR;
  }
  req_key_part->length = (uint32_t)create_field->key_length();
  
  return CT_SUCCESS;
}

bool ctc_ddl_fill_add_key(THD *thd, TABLE *form, TcDb__CtcDDLAlterTableDef *req,
  Alter_inplace_info *ha_alter_info, char *user)
{
  for (uint i = 0; i < req->n_add_key_list; i++) {
    TcDb__CtcDDLTableKey *req_key_def = req->add_key_list[i];
    const KEY *key = &ha_alter_info->key_info_buffer[ha_alter_info->index_add_buffer[i]];
    assert(key != nullptr);
    req_key_def->user = user;
    req_key_def->table = const_cast<char *>(thd->lex->query_tables->table_name);
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(key->name));
    req_key_def->name = const_cast<char *>(key->name);
    req_key_def->space = NULL;
    CTC_RETURN_IF_ERROR(get_ctc_key_type(key, &req_key_def->key_type), false);
    if (req_key_def->key_type == CTC_KEYTYPE_PRIMARY || req_key_def->key_type == CTC_KEYTYPE_UNIQUE) {
        req_key_def->is_constraint = true;
    }
    CTC_RETURN_IF_ERROR(get_ctc_key_algorithm(key->algorithm, &req_key_def->algorithm), false);
    for (uint j = 0; j < req_key_def->n_columns; j++) {
      TcDb__CtcDDLTableKeyPart *req_key_part = req_key_def->columns[j];
      const KEY_PART_INFO *key_part = key->key_part + j;
      const Create_field *create_field = get_field_by_index(ha_alter_info->alter_info, key_part->fieldnr);
      assert(create_field != NULL);
      CTC_RETURN_IF_ERROR(
          (ctc_fill_key_part(thd, req_key_part, req_key_def, create_field, form,
                             key_part) == CT_SUCCESS),
          false);
    }
    req_key_def->parallelism = get_session_level_create_index_parallelism(thd);
  }
  return true;
}

static int ctc_ddl_fill_drop_key(Alter_inplace_info *ha_alter_info, const dd::Table *old_table_def,
  TcDb__CtcDDLAlterTableDef *req)
{
  for (uint ctc_drop_key_idx = 0; ctc_drop_key_idx < req->n_drop_key_list; ctc_drop_key_idx++) {
    TcDb__CtcDDLAlterTableDropKey *req_drop = req->drop_key_list[ctc_drop_key_idx];
    const KEY *key = ha_alter_info->index_drop_buffer[ctc_drop_key_idx];
    req_drop->name = const_cast<char *>(key->name);
    req_drop->drop_type = CTC_ALTER_TABLE_DROP_KEY;

    const dd::Index *idx = ctc_ddl_get_index_by_name(old_table_def, req_drop->name);
    assert(idx != nullptr);
    CTC_RETURN_IF_ERROR(ctc_ddl_get_create_key_type(idx->type(), &req_drop->key_type), CT_ERROR);
  }
  
  return CT_SUCCESS;
}

static int fill_ctc_alter_drop_list(Alter_inplace_info *ha_alter_info, const dd::Table *old_table_def,
  TcDb__CtcDDLAlterTableDef *req)
{
  uint32_t ctc_drop_idx = 0;
  uint32_t mysql_drop_idx = 0;
  while (ctc_drop_idx < req->n_drop_list) {
    TcDb__CtcDDLAlterTableDrop *req_drop = req->drop_list[ctc_drop_idx];
    const Alter_drop *drop = ha_alter_info->alter_info->drop_list.at((size_t)mysql_drop_idx);
    req_drop->name = const_cast<char *>(drop->name);
    req_drop->drop_type = ctc_ddl_get_drop_type_from_mysql_type(drop->type);
    if (req_drop->drop_type == CTC_ALTER_TABLE_DROP_COLUMN) {
      const dd::Column *col = ctc_ddl_get_column_by_name(old_table_def, drop->name);
      assert(col != nullptr);
      if (col->is_virtual()) {
        req->n_drop_list--;
        mysql_drop_idx++;
        continue;
      }
    } else if (req_drop->drop_type == CTC_ALTER_TABLE_DROP_FOREIGN_KEY) {
      req_drop->key_type = CTC_KEYTYPE_FOREIGN;
    } else if (req_drop->drop_type == CTC_ALTER_TABLE_DROP_KEY) {
      // drop 索引通过index_drop_count实现
      req->n_drop_list--;
      mysql_drop_idx++;
      continue;
    }

    ctc_drop_idx++;
    mysql_drop_idx++;
  }
  return CT_SUCCESS;
}

static int fill_ctc_alter_create_list(THD *thd, TABLE *altered_table, Alter_inplace_info *ha_alter_info,
  dd::Table *new_table_def, TcDb__CtcDDLAlterTableDef *req, ddl_ctrl_t *ddl_ctrl, char **mem_start, char *mem_end)
{
  uint32_t ctc_col_idx = 0;
  uint32_t mysql_col_idx = 0;
  uint32_t virtual_cols = 0;
  while (ctc_col_idx < req->n_create_list) {
    // When alter table by inplace, the vir_cols based on func_indexs at the last req->n_create_list
    TcDb__CtcDDLColumnDef *req_create_column = req->create_list[ctc_col_idx];
    const Create_field *fld = ha_alter_info->alter_info->create_list[mysql_col_idx];

    // The vir_cols based on func_indexs are not pushed to engine
    if (is_field_for_functional_index(fld)) {
      mysql_col_idx++;
      req->n_create_list--;
      continue;
    } 

    // Virtual Generated Columns Processed
    if (fld->is_virtual_gcol()) {
      ddl_ctrl->table_flags |= CTC_FLAG_TABLE_CONTAINS_VIRCOL;
      virtual_cols++;
    }

    ctc_alter_column_alter_mode alter_mode = CTC_ALTER_COLUMN_ALTER_MODE_NONE;
    if (fld->field == NULL) {
      alter_mode = CTC_ALTER_COLUMN_ALTER_ADD_COLUMN;
    }
    if (fld->change != NULL) {
      alter_mode = CTC_ALTER_COLUMN_ALTER_MODIFY_COLUMN;
    }
    const CHARSET_INFO *field_cs = get_sql_field_charset(fld, ha_alter_info->create_info);
    CTC_RETURN_IF_ERROR(ctc_ddl_fill_column_by_field(thd, req_create_column, altered_table->s->field[mysql_col_idx],
                        ((const dd::Table *)new_table_def), altered_table, fld, alter_mode, mem_start, mem_end, field_cs),
                        CT_ERROR);
    ctc_col_idx++;
    mysql_col_idx++;
  }

  // Prevent only virtual columns
  if (req->n_create_list == virtual_cols) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "Cantian does not support all columns are generated.");
    return HA_ERR_WRONG_COMMAND;
  }

  return CT_SUCCESS;
}

static void fill_sys_cur_timestamp(THD *thd, TcDb__CtcDDLAlterTableDef *req) {
  date_detail_t date_detail;
  MYSQL_TIME ltime;
#ifdef FEATURE_X_FOR_MYSQL_32
  my_timeval tm = thd->query_start_timeval_trunc(0);
#elif defined(FEATURE_X_FOR_MYSQL_26)
  timeval tm = thd->query_start_timeval_trunc(0);
#endif
  my_tz_UTC->gmt_sec_to_TIME(&ltime, tm);
  assign_mysql_date_detail(MYSQL_TYPE_TIMESTAMP, ltime, &date_detail);
  cm_encode_date(&date_detail, &req->systimestamp);

  MYSQL_TIME ltime2;
  thd->time_zone()->gmt_sec_to_TIME(&ltime2, tm);
  longlong seconds_diff;
  long microsec_diff;
  bool negative = calc_time_diff(ltime2, ltime, 1, &seconds_diff, &microsec_diff);
  req->tz_offset_utc = negative ? -(seconds_diff / 60) : seconds_diff / 60;
  return;
}

static void fill_alter_list_4alter_table(dd::Table *new_table_def, Alter_inplace_info *ha_alter_info,
            TcDb__CtcDDLAlterTableDef *req, size_t rename_cols, uint32_t thd_id, char **req_mem_start, char *req_mem_end) {
  TcDb__CtcDDLAlterTableAlterColumn *req_alter = NULL;
  uint32_t copy_rm_num = 0;
  for (uint32_t i = 0; i < req->n_alter_list - rename_cols; i++) {
    req_alter = req->alter_list[i];
    
    const Alter_column *alter_column = ha_alter_info->alter_info->alter_list.at((size_t)i);
    if (alter_column->change_type() == Alter_column::Type::SET_DEFAULT ||
        alter_column->change_type() == Alter_column::Type::DROP_DEFAULT ||
        alter_column->change_type() == Alter_column::Type::SET_COLUMN_VISIBLE ||
        alter_column->change_type() == Alter_column::Type::SET_COLUMN_INVISIBLE ||
        alter_column->change_type() == Alter_column::Type::RENAME_COLUMN) {
      req_alter->name = const_cast<char *>(alter_column->name);
      req_alter->new_name = const_cast<char *>(alter_column->m_new_name);
      req_alter->type = ctc_ddl_get_alter_column_type_from_mysql_type(alter_column->change_type());
      if (alter_column->change_type() == Alter_column::Type::RENAME_COLUMN) {
        uint32_t copy_rm_index = req->n_alter_list - rename_cols + copy_rm_num;
        req_alter->new_name = (char *)ctc_ddl_alloc_mem(req_mem_start, req_mem_end, CTC_MAX_COLUMN_LEN);
        sprintf(req_alter->new_name, "TMPCOLUMN4CANTIAN_%d_%d", copy_rm_index, thd_id);
        // for rename swap columns:
        // column a to b, b to c, c to x... or a to b, b to a
        req->alter_list[copy_rm_index]->name = req_alter->new_name;
        req->alter_list[copy_rm_index]->new_name = const_cast<char *>(alter_column->m_new_name);
        req->alter_list[copy_rm_index]->type = req_alter->type;
        copy_rm_num++;
        continue;
      }
      const dd::Column *new_col = new_table_def->get_column(alter_column->name);
      req_alter->has_no_default = new_col->has_no_default() ? true : false;
      req_alter->is_default_null = new_col->is_default_value_null() ? true : false;
    }
  }
}

int fill_alter_table_req(TABLE *altered_table, Alter_inplace_info *ha_alter_info, const dd::Table *old_table_def,
    dd::Table *new_table_def, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem) {
  lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
  TcDb__CtcDDLAlterTableDef req;

  char *req_mem_start = ctc_ddl_req_mem;
  char *req_mem_end = req_mem_start + CTC_DDL_PROTOBUF_MEM_SIZE;

  size_t rename_cols = 0;
  CTC_RETURN_IF_NOT_ZERO(init_ctc_ddl_alter_table_def(&req, ha_alter_info, thd,
                         altered_table, &req_mem_start, req_mem_end, &rename_cols));

  assert(req_mem_start <= req_mem_end);
  char user_name_str[SMALL_RECORD_SIZE];
  if (thd->lex->query_tables != nullptr) {
    ctc_copy_name(user_name_str, thd->lex->query_tables->get_db_name(), SMALL_RECORD_SIZE);
  } else {
    ctc_copy_name(user_name_str, altered_table->s->db.str, SMALL_RECORD_SIZE);
  }
  req.user = user_name_str;
  req.name = const_cast<char *>(thd->lex->query_tables->table_name);
  fill_sys_cur_timestamp(thd, &req);
  CTC_RETURN_IF_ERROR((fill_ctc_alter_drop_list(ha_alter_info, old_table_def, &req) == CT_SUCCESS), CT_ERROR);
  if (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS) {
      req.options |= CTC_CREATE_TYPE_NO_CHECK_CONS;
  }
  // 删除索引相关逻辑填充
  if (ha_alter_info->index_drop_count) {
    assert(ha_alter_info->handler_flags &
          (Alter_inplace_info::DROP_INDEX |
           Alter_inplace_info::DROP_UNIQUE_INDEX |
           Alter_inplace_info::DROP_PK_INDEX));

    CTC_RETURN_IF_ERROR((ctc_ddl_fill_drop_key(ha_alter_info, old_table_def, &req) == CT_SUCCESS), CT_ERROR);
  }
  
  if (req.n_alter_list > 0) {
    fill_alter_list_4alter_table(new_table_def, ha_alter_info, &req,
                                 rename_cols, ddl_ctrl->tch.thd_id, &req_mem_start, req_mem_end);
  }

  CTC_RETURN_IF_ERROR((fill_ctc_alter_create_list(thd, altered_table, ha_alter_info,
                       new_table_def, &req, ddl_ctrl, &req_mem_start, req_mem_end) == CT_SUCCESS), CT_ERROR);

  // 创建索引相关逻辑填充
  CTC_RETURN_IF_ERROR(ctc_ddl_fill_add_key(thd, altered_table, &req, ha_alter_info, req.user), true);
  assert(req_mem_start <= req_mem_end);

  // rename索引
  for (uint32_t i = 0; i < req.n_alter_index_list; ++i) {
      TcDb__CtcDDLAlterIndexDef *req_alter_index_def = req.alter_index_list[i];

      req_alter_index_def->user = user_name_str;
      req_alter_index_def->table = const_cast<char *>(thd->lex->query_tables->table_name);
      req_alter_index_def->name = const_cast<char *>(ha_alter_info->index_rename_buffer[i].old_key->name);
      req_alter_index_def->new_name = const_cast<char *>(ha_alter_info->index_rename_buffer[i].new_key->name);
      CTC_RETURN_IF_ERROR(get_ctc_key_type(ha_alter_info->index_rename_buffer[i].old_key, &req_alter_index_def->key_type), true);
  }
  req.db_name = CTC_GET_THD_DB_NAME(thd);
  string sql = string(thd->query().str).substr(0, thd->query().length);
  req.sql_str = const_cast<char *>(sql.c_str());

  size_t msg_len = tc_db__ctc_ddlalter_table_def__get_packed_size(&req);
  stack_mem->set_mem_size(msg_len + sizeof(ddl_ctrl_t));
  void *ctc_ddl_req_msg_mem = stack_mem->get_buf();
  if(ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  if (tc_db__ctc_ddlalter_table_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem + sizeof(ddl_ctrl_t)) != msg_len) {
    assert(false);
  }

  ddl_ctrl->msg_len = msg_len + sizeof(ddl_ctrl_t);
  memcpy(ctc_ddl_req_msg_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  return 0;
}

static void ctc_fill_rename_constraints(TcDb__CtcDDLRenameTableDef *req, const char *old_cons_name,
  const char *new_cons_name, char **mem_start, char *mem_end) {
  req->old_constraints_name[req->n_old_constraints_name] = (char *)ctc_ddl_alloc_mem(mem_start, mem_end,
                                                               sizeof(char) * (strlen(old_cons_name) + 1));
  req->new_constraints_name[req->n_new_constraints_name] = (char *)ctc_ddl_alloc_mem(mem_start, mem_end,
                                                               sizeof(char) * (strlen(new_cons_name) + 1));
  assert(req->old_constraints_name[req->n_old_constraints_name] != NULL);
  assert(req->new_constraints_name[req->n_new_constraints_name] != NULL);
  strcpy(req->old_constraints_name[req->n_old_constraints_name], old_cons_name);
  strcpy(req->new_constraints_name[req->n_new_constraints_name], new_cons_name);
}

static int init_ctc_ddl_rename_constraints_def(THD *thd, TcDb__CtcDDLRenameTableDef *req,
  const dd::Table *from_table_def, dd::Table *to_table_def, char **mem_start, char *mem_end) {
  size_t constraint_size = from_table_def->foreign_keys().size();
  handlerton *ctc_handlerton = get_ctc_hton();
  size_t generated_cons = 0;  // The number of default constraints
  bool is_alter_copy = is_alter_table_copy(thd);

  if (constraint_size <= 0) {
    return 0;
  }

  const char *from_tbl_name = is_alter_copy ? thd->lex->query_tables->table_name :
                              from_table_def->name().c_str();
  size_t tbl_name_length = is_alter_copy ? strlen(thd->lex->query_tables->table_name) :
                            from_table_def->name().length();
  bool is_rename_table = ((thd->lex->sql_command == SQLCOM_RENAME_TABLE) ||                     // sql_command
                          (from_table_def->tablespace_id() != to_table_def->tablespace_id()) || // copy rename cross db
                          (strcmp(from_tbl_name, to_table_def->name().c_str()) != 0));          // alter copy(如指定)
  for (const dd::Foreign_key *fk : from_table_def->foreign_keys()) {
    bool is_generated_name = dd::is_generated_foreign_key_name(from_tbl_name, tbl_name_length, ctc_handlerton, *fk);
    if (is_generated_name && is_rename_table) {
      generated_cons++;
    }
  }

  req->new_constraints_name = (char **)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(char *) * generated_cons);
  req->old_constraints_name = (char **)ctc_ddl_alloc_mem(mem_start, mem_end, sizeof(char *) * generated_cons);
  if (req->new_constraints_name == NULL || req->old_constraints_name == NULL) {
    return HA_ERR_OUT_OF_MEM;
  }

  for (const dd::Foreign_key *fk : from_table_def->foreign_keys()) {
    bool is_generated_name = dd::is_generated_foreign_key_name(from_tbl_name, tbl_name_length, ctc_handlerton, *fk);
    if (is_generated_name && is_rename_table) {
      char new_fk_name[CTC_MAX_CONS_NAME_LEN  + 1];
      // Construct new name by copying <FK name suffix><number> suffix from the old one.
      strxnmov(new_fk_name, sizeof(new_fk_name) - 1, to_table_def->name().c_str(),
              fk->name().c_str() + tbl_name_length, NullS);
      ctc_fill_rename_constraints(req, fk->name().c_str(), new_fk_name, mem_start, mem_end);
      req->n_old_constraints_name++;
      req->n_new_constraints_name++;
    }
  }

  return 0;
}

int fill_rename_table_req(const char *from, const char *to, const dd::Table *from_table_def,
  dd::Table *to_table_def, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem) {
  char old_db[SMALL_RECORD_SIZE] = { 0 };
  char new_db[SMALL_RECORD_SIZE] = { 0 };
  char user_name[SMALL_RECORD_SIZE] = { 0 };
  char new_user_name[SMALL_RECORD_SIZE] = { 0 };
  ctc_split_normalized_name(from, old_db, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
  ctc_split_normalized_name(to, new_db, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
  ctc_copy_name(user_name, old_db, SMALL_RECORD_SIZE);
  ctc_copy_name(new_user_name, new_db, SMALL_RECORD_SIZE);
  
  char *req_mem_start = ctc_ddl_req_mem;
  char *req_mem_end = req_mem_start + CTC_DDL_PROTOBUF_MEM_SIZE;
  TcDb__CtcDDLRenameTableDef req;
  tc_db__ctc_ddlrename_table_def__init(&req);

  int ret = init_ctc_ddl_rename_constraints_def(thd, &req, from_table_def, to_table_def, &req_mem_start, req_mem_end);
  if (ret != 0) {
    return ret;
  }

  req.user = user_name;
  req.new_user = new_user_name;
  req.old_db_name = old_db;
  req.old_table_name = const_cast<char *>(from_table_def->name().c_str());
  req.new_db_name = new_db;
  req.new_table_name = const_cast<char *>(to_table_def->name().c_str());
  req.current_db_name = const_cast<char *>(CTC_GET_THD_DB_NAME(thd));

  string sql = string(thd->query().str).substr(0, thd->query().length);
  req.sql_str = const_cast<char *>(sql.c_str());

  size_t msg_len = tc_db__ctc_ddlrename_table_def__get_packed_size(&req);
  stack_mem->set_mem_size(msg_len + sizeof(ddl_ctrl_t));
  void *ctc_ddl_req_msg_mem = stack_mem->get_buf();
  if(ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  if (tc_db__ctc_ddlrename_table_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem) != msg_len) {
    assert(false);
  }

  ddl_ctrl->msg_len = msg_len;
  return 0;
}

static int fill_partition_info_4truncate(TcDb__CtcDDLTruncateTablePartitionDef *req, dd::Table *dd_table,
                                         partition_info *part_info, char **mem_start, char *mem_end) {
  uint32_t part_num = 0;
  if (part_info->is_sub_partitioned()) {
    req->n_subpartition_id = 0;
    req->subpartition_id = (uint32_t *)ctc_ddl_alloc_mem(
         mem_start, mem_end, dd_table->leaf_partitions()->size() * sizeof(uint32_t));
    if (req->subpartition_id == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }
    req->n_subpartition_name = 0;
    req->subpartition_name = (char **)ctc_ddl_alloc_mem(
         mem_start, mem_end, dd_table->leaf_partitions()->size() * sizeof(char *));
    if (req->subpartition_name == nullptr) {
      return HA_ERR_OUT_OF_MEM;
    }
    req->is_subpart = 1;
    for (const auto dd_part : *dd_table->leaf_partitions()) {
      if (!part_info->is_partition_used(part_num++)) {
        continue;
      }
      int part_id = (part_num - 1) / part_info->num_subparts;
      int subpart_id = (part_num - 1) % part_info->num_subparts;
      req->partition_id[req->n_partition_id] = part_id;
      req->n_partition_id++;
      req->partition_name[req->n_partition_name] = const_cast<char *>(dd_part->name().c_str());
      req->n_partition_name++;
      req->subpartition_id[req->n_subpartition_id] = subpart_id;
      req->n_subpartition_id++;
      req->subpartition_name[req->n_subpartition_name] = const_cast<char *>(dd_part->name().c_str());
      req->n_subpartition_name++;
    }
  } else {
    for (const auto dd_part : *dd_table->leaf_partitions()) {
      if (!part_info->is_partition_used(part_num++)) {
        continue;
      }
      req->partition_id[req->n_partition_id] = part_num - 1;
      req->n_partition_id++;
      req->partition_name[req->n_partition_name] = const_cast<char *>(dd_part->name().c_str());
      req->n_partition_name++;
    }
  }
  return 0;
}

int fill_truncate_partition_req(const char *full_name, partition_info *part_info,
  dd::Table *dd_table, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem) {
  lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
  char *req_mem_start = ctc_ddl_req_mem;
  char *req_mem_end = req_mem_start + CTC_DDL_PROTOBUF_MEM_SIZE;
  char db_name[SMALL_RECORD_SIZE] = { 0 };
  char user_name[SMALL_RECORD_SIZE] = { 0 };
  const char *table_name_str = dd_table->name().c_str();
  ctc_split_normalized_name(full_name, db_name, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
  ctc_copy_name(user_name, db_name, SMALL_RECORD_SIZE);
  TcDb__CtcDDLTruncateTablePartitionDef req;
  tc_db__ctc_ddltruncate_table_partition_def__init(&req);
  req.user = user_name;
  req.db_name = CTC_GET_THD_DB_NAME(thd);
  req.table_name = const_cast<char *>(table_name_str);
  string sql = string(thd->query().str).substr(0, thd->query().length);
  req.sql_str = const_cast<char *>(sql.c_str());

  req.n_partition_id = 0;
  req.partition_id = (uint32_t *)ctc_ddl_alloc_mem(
      &req_mem_start, req_mem_end, dd_table->leaf_partitions()->size() * sizeof(uint32_t));
  if (req.partition_id == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  req.n_partition_name = 0;
  req.partition_name = (char **)ctc_ddl_alloc_mem(
    &req_mem_start, req_mem_end, dd_table->leaf_partitions()->size() * sizeof(char *));
  if (req.partition_name == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  CTC_RETURN_IF_NOT_ZERO(fill_partition_info_4truncate(&req, dd_table,
                         part_info, &req_mem_start, req_mem_end));
  size_t msg_len = tc_db__ctc_ddltruncate_table_partition_def__get_packed_size(&req);
  stack_mem->set_mem_size(msg_len + sizeof(ddl_ctrl_t));
  void *ctc_ddl_req_msg_mem = stack_mem->get_buf();
  if(ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  if (tc_db__ctc_ddltruncate_table_partition_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem + sizeof(ddl_ctrl_t))
      != msg_len) {
    assert(false);
  }

  ddl_ctrl->msg_len = msg_len + sizeof(ddl_ctrl_t);
  memcpy(ctc_ddl_req_msg_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  return 0;
}

int init_ctc_optimize_table_def(TcDb__CtcDDLAlterTableDef *req, TABLE *table, char **mem_start, char *mem_end)  {
  tc_db__ctc_ddlalter_table_def__init(req);
  req->n_alter_index_list = table->s->keys;
  if (req->n_alter_index_list > 0) {
    req->alter_index_list = (TcDb__CtcDDLAlterIndexDef **)ctc_ddl_alloc_mem(
       mem_start, mem_end, sizeof(TcDb__CtcDDLAlterIndexDef) * req->n_alter_index_list);
    if (req->alter_index_list == NULL) {
      return HA_ERR_OUT_OF_MEM;
    }
    for (uint i = 0; i < req->n_alter_index_list; i++) {
      req->alter_index_list[i] = (TcDb__CtcDDLAlterIndexDef *)ctc_ddl_alloc_mem(
          mem_start, mem_end, sizeof(TcDb__CtcDDLAlterIndexDef));
      if (req->alter_index_list[i] == NULL) {
        return HA_ERR_OUT_OF_MEM;
      }
      tc_db__ctc_ddlalter_index_def__init(req->alter_index_list[i]);
    }
  }
  return 0;
}

int fill_rebuild_index_req(TABLE *table, THD *thd, ddl_ctrl_t *ddl_ctrl, ctc_ddl_stack_mem *stack_mem) {
  int ret;
  lock_guard<mutex> lock(m_ctc_ddl_protobuf_mem_mutex);
  TcDb__CtcDDLAlterTableDef req;
  char *req_mem_start = ctc_ddl_req_mem;
  char *req_mem_end = req_mem_start + CTC_DDL_PROTOBUF_MEM_SIZE;
  char db_name[SMALL_RECORD_SIZE] = { 0 };
  char table_name[SMALL_RECORD_SIZE] = { 0 };

  ctc_copy_name(db_name, thd->lex->query_tables->get_db_name(), SMALL_RECORD_SIZE);
  ctc_copy_name(table_name, thd->lex->query_tables->table_name, SMALL_RECORD_SIZE);

  ret = init_ctc_optimize_table_def(&req, table, &req_mem_start, req_mem_end);
  if (ret != 0) {
    return ret;
  }
  req.user = db_name;
  req.name = table_name;
  fill_sys_cur_timestamp(thd, &req);
  for (uint32_t i = 0; i < req.n_alter_index_list; ++i) {
    TcDb__CtcDDLAlterIndexDef *req_alter_index_def = req.alter_index_list[i];
    req_alter_index_def->user = db_name;
    req_alter_index_def->table = table_name;
    req_alter_index_def->name = const_cast<char*> (table->key_info[i].name);
  }
  req.db_name = CTC_GET_THD_DB_NAME(thd);
  string sql = string(thd->query().str).substr(0, thd->query().length);
  req.sql_str = const_cast<char *>(sql.c_str());
  size_t msg_len = tc_db__ctc_ddlalter_table_def__get_packed_size(&req);
  stack_mem->set_mem_size(msg_len + sizeof(ddl_ctrl_t));
  void *ctc_ddl_req_msg_mem = stack_mem->get_buf();
  if(ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  if (tc_db__ctc_ddlalter_table_def__pack(&req, (uint8_t *)ctc_ddl_req_msg_mem + sizeof(ddl_ctrl_t)) != msg_len) {
    assert(false);
  }

  ddl_ctrl->msg_len = msg_len + sizeof(ddl_ctrl_t);
  memcpy(ctc_ddl_req_msg_mem, ddl_ctrl, sizeof(ddl_ctrl_t));
  return 0;
}