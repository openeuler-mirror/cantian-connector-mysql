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
#include <unordered_map>
#include "tse_error.h"
#include "my_base.h"
#include "tse_log.h"

static std::unordered_map<ct_errno_t, int> err_code_lookup_map = {
  {ERR_OAMAP_DUP_KEY_ERROR, HA_ERR_GENERIC},
  {ERR_OAMAP_INSERTION_FAILED, HA_ERR_GENERIC},
  {ERR_OAMAP_FETCH_FAILED, HA_ERR_GENERIC},
  {ERR_GENERIC_INTERNAL_ERROR, HA_ERR_GENERIC},
  {ERR_NOT_SUPPORT, HA_ERR_WRONG_COMMAND},
  {ERR_OPERATIONS_NOT_SUPPORT, HA_ERR_WRONG_COMMAND},
  /* Since we rolled back the whole transaction, we must
  tell it also to MySQL so that MySQL knows to empty the
  cached binlog for this transaction */
  {ERR_DEAD_LOCK, HA_ERR_LOCK_DEADLOCK},
  /* The DAAC serialization isolation level is stricter than the MySQL
  repeatable read isolation level. If two transactions conflict:
  - The daac is locked, including lock timeout and transaction area conflict.
  - MySQL will be locked, including lock timeout and success.
  Therefore, the daac transaction conflict error code is temporarily
  classified as lock timeout. */
  {ERR_SERIALIZE_ACCESS, HA_ERR_RECORD_CHANGED},
  /* Starting from 5.0.13, we let MySQL just roll back the
  latest SQL statement in a lock wait timeout. Previously, we
  rolled back the whole transaction. */
  {ERR_LOCK_TIMEOUT, HA_ERR_LOCK_WAIT_TIMEOUT},
  /* If prefix is true then a 768-byte prefix is stored
  locally for BLOB fields. Refer to dict_table_get_format().
  We limit max record size to 16k for 64k page size. */
  {ERR_RECORD_SIZE_OVERFLOW, HA_ERR_TOO_BIG_ROW},
  /* Be cautious with returning this error, since
  mysql could re-enter the storage layer to get
  duplicated key info, the operation requires a
  valid table handle and/or transaction information,
  which might not always be available in the error
  handling stage. */
  {ERR_DUPLICATE_KEY, HA_ERR_FOUND_DUPP_KEY},
  {ERR_CONSTRAINT_VIOLATED_NO_FOUND, HA_ERR_NO_REFERENCED_ROW},
  {ERR_ROW_IS_REFERENCED, HA_ERR_ROW_IS_REFERENCED},
  {ERR_DEF_CHANGED, HA_ERR_TABLE_DEF_CHANGED},
  {ERR_SAVEPOINT_NOT_EXIST, HA_ERR_NO_SAVEPOINT},
  {ERR_NO_MORE_LOCKS, HA_ERR_LOCK_TABLE_FULL},
  {ERR_FILE_NOT_EXIST, HA_ERR_WRONG_FILE_NAME},
  {ERR_SPACE_NOT_EXIST, HA_ERR_TABLESPACE_MISSING},
  {ERR_SPACE_ALREADY_EXIST, HA_ERR_TABLESPACE_EXISTS},
  {ERR_BTREE_LEVEL_EXCEEDED, HA_ERR_INTERNAL_ERROR},
  {ERR_TABLE_OR_VIEW_NOT_EXIST, HA_ERR_NO_SUCH_TABLE},
  {ERR_DC_INVALIDATED, HA_ERR_NO_SUCH_TABLE},
  {ERR_INDEX_INVALID, HA_ERR_WRONG_INDEX},
  {ERR_CONNECTION_FAILED, HA_ERR_NO_CONNECTION},
  {ERR_ALLOC_MEMORY, HA_ERR_SE_OUT_OF_MEMORY},
  {ERR_ROW_LOCKED_NOWAIT, HA_ERR_NO_WAIT_LOCK},
  {ERR_OPERATION_CANCELED, HA_ERR_QUERY_INTERRUPTED},
  {ERR_SEM_FAULT, HA_ERR_INTERNAL_ERROR},
  {ERR_BATCH_DATA_HANDLE_FAILED, HA_ERR_INTERNAL_ERROR},
  {ERR_SHM_SEND_MSG_FAILED, HA_ERR_NO_CONNECTION},
  {ERR_INSTANCE_REG_FAILED, HA_ERR_INTERNAL_ERROR},
  {ERR_AUTOINC_READ_FAILED, HA_ERR_AUTOINC_READ_FAILED},
  {ERR_OBJECT_ALREADY_DROPPED, HA_ERR_TABLE_CORRUPT},
  {ERR_PAGE_CORRUPTED, HA_ERR_TABLE_CORRUPT},
};

bool convert_cantian_err_to_mysql(ct_errno_t error) {
  switch (error) {
    case ERR_CAPABILITY_NOT_SUPPORT:
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "Cantian capability not support.");
      break;
    case ERR_DATABASE_ROLE:
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "Cantian database role not support this operation.");
      break;
    case ERR_COLUMNS_MISMATCH:
      my_error(ER_WRONG_VALUE_COUNT, MYF(0));
      break;
    case ERR_ROW_LOCKED_NOWAIT:
      my_error(ER_LOCK_NOWAIT, MYF(0));
      break;
    case ERR_ROW_SELF_UPDATED:
      my_printf_error(ER_MULTI_UPDATE_KEY_CONFLICT,
        "Primary key/partition key update is not allowed since the table is updated both.", MYF(0));
      break;
    case ERR_COLUMN_HAS_NULL:
      my_error(ER_INVALID_USE_OF_NULL, MYF(0));
      break;
    case ERR_TOO_MANY_CONNECTIONS:
      my_error(ER_CON_COUNT_ERROR, MYF(0));
      break;
    case ERR_NAME_TOO_LONG:
      my_printf_error(ER_TOO_LONG_IDENT, "Identifier name is too long", MYF(0));
      break;
    case ERR_CHILD_DUPLICATE_KEY:
      my_printf_error(ER_FOREIGN_DUPLICATE_KEY_WITH_CHILD_INFO,
        "Foreign key constraint would lead to a duplicate entry in child table", MYF(0));
      break;
    case ERR_FK_NO_INDEX_PAREN_4MYSQL:
      my_error(ER_CANNOT_ADD_FOREIGN, MYF(0));
      break;
    case ERR_EXCEED_MAX_CASCADE_DEPTH:
      my_printf_error(ER_FK_DEPTH_EXCEEDED, "Foreign key cascade delete/update exceeds max depth of 15", MYF(0));
      break;
    case ERR_SNAPSHOT_TOO_OLD:
      my_printf_error(HA_ERR_TOO_MANY_CONCURRENT_TRXS,
                      "snapshot too old, it is advised to modify the parameters _undo_active_segments in Cantian", MYF(0));
      break;
    case ERR_CHILD_ROW_CANNOT_ADD_OR_UPDATE:
      my_printf_error(ER_NO_REFERENCED_ROW_2,
                      "Cannot add or update a child row: a foreign key constraint fails", MYF(0));
      break;
    default:
      return false;
  }
  return true;
}

void tse_alter_table_handle_fault(ct_errno_t error) {
  switch (error) {
    case ERR_CONSTRAINT_VIOLATED_NO_FOUND:
      my_printf_error(ER_NO_REFERENCED_ROW_2, 
        "Cannot add or update a child row: a foreign key constraint fails.", MYF(0));
      break;
    default:
      my_error(ER_GET_ERRNO, MYF(0), error, "Cantian error");
      break;
  }
}


int convert_tse_error_code_to_mysql_impl(ct_errno_t error, const char* funcName, const int line) {
  if (error == CT_SUCCESS) {
    return 0;
  }

  if (convert_cantian_err_to_mysql(error)) {
    return HA_ERR_WRONG_COMMAND;
  }

  int ret = HA_ERR_GENERIC;
  auto iter = err_code_lookup_map.find(error);
  if (iter != err_code_lookup_map.end()) {
    ret = iter->second;
  } else {
   tse_log_system("func %s(line%d) returned with unknown err, cantian ret %d", funcName, line, (int)error);
  }
  tse_log_note("func %s(line%d) returned with errCode %d, cantian ret %d", funcName, line, ret, (int)error);
  return ret;
}
