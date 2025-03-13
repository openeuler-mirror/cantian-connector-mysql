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

// @file storage/ctc/ha_ctc.cc
// description: CTC handler implementation for MySQL storage engine API.
// this module should use ctc_srv rather than knl_intf

/**
  @file ha_ctc.cc

  @details
  The ctc storage engine is set up to use table locks. It
  implements an ctc "SHARE" that is inserted into a hash by table
  name. You can use this to store information of state that any
  ctc handler object will be able to see when it is using that
  table.

  Please read the object definition in ha_ctc.h before reading the rest
  of this file.

  @note
  When you create an CTC table, the MySQL Server creates a table .frm
  (format) file in the database directory, using the table name as the file
  name as is customary with MySQL. No other files are created. To get an idea
  of what occurs, here is an ctc select that would do a scan of an entire
  table:
  @code
  ha_ctc::store_lock
  ha_ctc::external_lock
  ha_ctc::info
  ha_ctc::rnd_init
  ha_ctc::extra
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::rnd_next
  ha_ctc::extra
  ha_ctc::external_lock
  ha_ctc::extra
  ENUM HA_EXTRA_RESET        Reset database to after open
  @endcode

  Here you see that the ctc storage engine has 9 rows called before
  rnd_next signals that it has reached the end of its data. Also note that
  the table in question was already opened; had it not been open, a call to
  ha_ctc::open() would also have been necessary. Calls to
  ha_ctc::extra() are hints as to what will be occurring to the request.

  A Longer Dse can be found called the "Skeleton Engine" which can be
  found on TangentOrg. It has both an engine and a full build environment
  for building a pluggable storage engine.

  Happy coding!<br>
    -Brian
*/

#include "ha_ctc.h"
#include "ha_ctc_ddl.h"
#include "ha_ctcpart.h"
#include "ha_ctc_pq.h"

#include <errno.h>
#include <limits.h>
#include <sql/sql_thd_internal_api.h>
#include <mysql/thread_pool_priv.h>
#include <atomic>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <map>
#include "field_types.h"
#include "my_base.h"
#include "my_macros.h"
#include "my_pointer_arithmetic.h"
#include "my_psi_config.h"
#include "mysql/plugin.h"
#include "sql/current_thd.h"
#include "sql/dd/types/table.h"
#include "sql/discrete_interval.h" // Discrete_interval
#include "sql/field.h"
#include "sql/create_field.h"
#include "sql/sql_base.h"  // enum_tdc_remove_table_type
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_insert.h"
#include "sql/sql_plugin.h"
#include "sql/sql_initialize.h"                // opt_initialize_insecure
#include "sql/dd/upgrade/server.h"             // UPGRADE_FORCE

#include "sql/dd/properties.h"
#include "sql/dd/types/partition.h"
#include "ctc_stats.h"
#include "ctc_error.h"
#include "ctc_log.h"
#include "ctc_srv_mq_module.h"
#include "ctc_util.h"
#include "protobuf/tc_db.pb-c.h"
#include "typelib.h"
#include "datatype_cnvrt_4_index_search.h"
#include "sql/mysqld.h"
#include "sql/plugin_table.h"
#include "sql/dd/object_id.h"
#include "sql/dd/string_type.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/dd_schema.h"
#include "sql/sql_table.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_backup_lock.h"
#include "ctc_meta_data.h"
#include "sql/mysqld.h"
#include "sql/sql_executor.h" // class QEP_TAB
#ifdef FEATURE_X_FOR_MYSQL_32
#include "sql/join_optimizer/access_path.h"
#elif defined(FEATURE_X_FOR_MYSQL_26)
#include "sql/abstract_query_plan.h"
#include "sql/opt_range.h" // QUICK_SELECT_I
#endif


//------------------------------------------------------------------------------//
//                        SYSTEM VARIABLES //
//------------------------------------------------------------------------------//
/*
 * SYSTEM VARIABLES CAN BE DISPLAYED AS:
 * mysql> SHOW GLOBAL VARIABLES like'%ctc%'
 */

#define CTC_MAX_SAMPLE_SIZE (4096)    // MB
#define CTC_MIN_SAMPLE_SIZE (32)      // MB
#define CTC_DEFAULT_SAMPLE_SIZE (128) // MB

static void ctc_statistics_enabled_update(THD * thd, SYS_VAR *, void *var_ptr, const void *save) {
    bool enabled = *static_cast<bool *>(var_ptr) = *static_cast<const bool *>(save);
    ctc_stats::get_instance()->set_statistics_enabled(enabled);
    if (enabled) {
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_DISALLOWED_OPERATION,
            "CTC: ONLY FOR developers profiling and testing purposes! \
            Turning on this switch will cause a significant performance DEGRADATION! .");
    }
}

/* 创库的表空间datafile自动扩展, 默认开 */
bool ctc_db_datafile_autoextend = true;
static MYSQL_SYSVAR_BOOL(db_datafile_autoextend, ctc_db_datafile_autoextend, PLUGIN_VAR_NOCMDARG,
  "Indicates whether to automatically extend the tablespace data files of the CTC database.", nullptr, nullptr, true);
/* 创库的表空间datafile大小, 单位M, 默认32M, 最小1M, 最大8T */
uint32_t ctc_db_datafile_size = 32;
static MYSQL_SYSVAR_UINT(db_datafile_size, ctc_db_datafile_size, PLUGIN_VAR_RQCMDARG,
  "Size of the tablespace data file of the CTC database, in MB.", nullptr, nullptr, 32, 1,  8192 * 1024, 0);
/* 创库的表空间datafile自动扩展大小, 单位M, 默认128M, 最小1M, 最大8T */
uint32_t ctc_db_datafile_extend_size = 128;
static MYSQL_SYSVAR_UINT(db_datafile_extend_size, ctc_db_datafile_extend_size, PLUGIN_VAR_RQCMDARG,
  "Size of the CTC database tablespace data file automatically extended, in MB.", nullptr, nullptr, 128, 1, 8192 * 1024, 0);

bool ctc_concurrent_ddl = true;
static MYSQL_SYSVAR_BOOL(concurrent_ddl, ctc_concurrent_ddl, PLUGIN_VAR_RQCMDARG,
                         "Indicates whether to ban concurrent DDL.", nullptr, nullptr, true);

static mutex m_ctc_metadata_normalization_mutex;
int32_t ctc_metadata_normalization = (int32_t)metadata_switchs::DEFAULT;
static MYSQL_SYSVAR_INT(metadata_normalization, ctc_metadata_normalization, PLUGIN_VAR_READONLY,
                        "Option for Mysql-Cantian metadata normalization.", nullptr, nullptr, -1, -1, 3, 0);

extern int32_t ctc_cluster_role;
extern mutex m_ctc_cluster_role_mutex;
static MYSQL_SYSVAR_INT(cluster_role, ctc_cluster_role, PLUGIN_VAR_READONLY,
                        "flag for Disaster Recovery Cluster Role.", nullptr, nullptr, -1, -1, 2, 0);

static mutex m_ctc_shm_file_num_mutex;

int32_t ctc_max_cursors_no_autocommit = 128;
static MYSQL_SYSVAR_INT(max_cursors_no_autocommit, ctc_max_cursors_no_autocommit, PLUGIN_VAR_RQCMDARG,
                        "Size of max cursors for no autocommit in commit/rollback.", nullptr, nullptr, 128, 0, 8192, 0);

static MYSQL_THDVAR_UINT(lock_wait_timeout, PLUGIN_VAR_RQCMDARG,
                         "Timeout in seconds an CTC transaction may wait "
                         "for a lock before being rolled back. Unit is "
                         "millisecond and values 0 means disable the timeout.",
                         nullptr, nullptr, 50000, 0, 1024 * 1024 * 1024, 0);

static MYSQL_THDVAR_DOUBLE(sampling_ratio, PLUGIN_VAR_RQCMDARG,
                           "sampling ratio used for analyzing tables", nullptr,
                           nullptr, 100, 0.000001, 100, 0);

__attribute__((visibility("default"))) uint32_t ctc_instance_id = 0;
static MYSQL_SYSVAR_UINT(instance_id, ctc_instance_id, PLUGIN_VAR_READONLY,
                         "mysql instance id which is used for cantian", nullptr,
                         nullptr, 0, 0, UINT32_MAX, 0);

static void ctc_stats_auto_recalc_update(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  int ret;
  update_job_info info = { "GATHER_CHANGE_STATS", 19, "SYS", 3, 0 };
  bool val = *static_cast<bool *>(var_ptr) = *static_cast<const bool *>(save);
  info.switch_on = val;
  ret = (ct_errno_t)ctc_update_job(info);
  if (ret != CT_SUCCESS) {
    ctc_log_error("Error update cantian job info: %d", ret);
  }
}

bool ctc_stats_auto_recalc = true;
static MYSQL_SYSVAR_BOOL(stats_auto_recalc, ctc_stats_auto_recalc, PLUGIN_VAR_NOCMDARG,
                         "auto statistics collecting is turn on", nullptr, ctc_stats_auto_recalc_update, true);

bool ctc_enable_x_lock_instance = false;
static MYSQL_SYSVAR_BOOL(enable_x_lock_instance, ctc_enable_x_lock_instance, PLUGIN_VAR_NOCMDARG,
                         "LCOK INSTANCE FOR BACKUP add X latch on the cantian side", nullptr, nullptr, false);

char *ctc_version_str = const_cast<char *>(CTC_VERSION_STR);
static MYSQL_SYSVAR_STR(version, ctc_version_str,
                        PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_READONLY |
                            PLUGIN_VAR_NOPERSIST,
                        "ctc plugin version", nullptr, nullptr, CTC_VERSION_STR);

bool ctc_statistics_enabled = false;
static MYSQL_SYSVAR_BOOL(statistics_enabled, ctc_statistics_enabled, PLUGIN_VAR_NOCMDARG,
                         "If statistical the costs of ctc interfaces.", nullptr, ctc_statistics_enabled_update, false);

uint32_t ctc_autoinc_lock_mode = CTC_AUTOINC_NO_LOCKING;
static MYSQL_SYSVAR_UINT(autoinc_lock_mode, ctc_autoinc_lock_mode, PLUGIN_VAR_RQCMDARG,
                         "The AUTOINC lock modes supported by CTC.", nullptr, nullptr, CTC_AUTOINC_NO_LOCKING,
                         CTC_AUTOINC_OLD_STYLE_LOCKING, CTC_AUTOINC_NO_LOCKING, 0);

uint32_t ctc_update_analyze_time = CTC_ANALYZE_TIME_SEC;
static MYSQL_SYSVAR_UINT(update_analyze_time, ctc_update_analyze_time, PLUGIN_VAR_RQCMDARG,
                         "CBO updating time by CTC. Unit is second.", nullptr, nullptr, CTC_ANALYZE_TIME_SEC,
                         0, 900, 0);

static mutex m_ctc_sample_size_mutex;
uint32_t ctc_sample_size;
static int check_sample_size(THD *, SYS_VAR *, void *save, struct st_mysql_value *value)
{
  longlong in_val;
  value->val_int(value, &in_val);

  if (in_val < CTC_MIN_SAMPLE_SIZE || in_val >= CTC_MAX_SAMPLE_SIZE) {
    std::stringstream error_str;
    error_str << "The value " << in_val
              << " is not within the range of accepted values for the option "
              << "ctc_sample_size.The value must be between "
              << CTC_MIN_SAMPLE_SIZE << " inclusive and "
              << CTC_MAX_SAMPLE_SIZE << " exclusive.";
    my_message(ER_WRONG_VALUE_FOR_VAR, error_str.str().c_str(), MYF(0));
    
    return CT_ERROR;
  }

  *(longlong *)save = in_val;

  return CT_SUCCESS;
}

static void update_sample_size(THD *thd, SYS_VAR *, void *, const void *save)
{
  lock_guard<mutex> lock(m_ctc_sample_size_mutex);
  List_iterator_fast<set_var_base> var_it(thd->lex->var_list);
  var_it.rewind();
  set_var_base *var = nullptr;
  string name_str;
  while((var = var_it++)) {
    if (typeid(*var) == typeid(set_var)) {
      set_var *setvar = dynamic_cast<set_var *>(var);
#ifdef FEATURE_X_FOR_MYSQL_32
      name_str = setvar->m_var_tracker.get_var_name();
#elif defined(FEATURE_X_FOR_MYSQL_26)
      name_str = setvar->var->name.str;
#endif
      if (name_str != "ctc_sample_size") {
        continue;
      }
      bool need_persist = (setvar->type == OPT_PERSIST);
      if (ctc_update_sample_size(*static_cast<const uint32_t *>(save), need_persist) == CT_SUCCESS) {
        ctc_sample_size = *static_cast<const uint32_t *>(save);
      }
    }
  }
}

static MYSQL_SYSVAR_UINT(sample_size, ctc_sample_size, PLUGIN_VAR_RQCMDARG,
                         "The size of the statistical sample data, measured in megabytes (MB).",
                         check_sample_size, update_sample_size,
                         CTC_DEFAULT_SAMPLE_SIZE, CTC_MIN_SAMPLE_SIZE, CTC_MAX_SAMPLE_SIZE, 0);

bool ctc_select_prefetch = true;
static MYSQL_SYSVAR_BOOL(select_prefetch, ctc_select_prefetch, PLUGIN_VAR_RQCMDARG,
                         "Indicates whether using prefetch in select.", nullptr, nullptr, true);

int32_t parallel_read_threads = 4;
static MYSQL_THDVAR_INT(parallel_read_threads, PLUGIN_VAR_OPCMDARG,
                        "Degree of Parallel for turbo plugin for a single table in single session", nullptr, nullptr,
                        4, 1, CT_MAX_PARAL_QUERY, 0);

int32_t ctc_parallel_max_read_threads = 128;
static MYSQL_SYSVAR_INT(parallel_max_read_threads, ctc_parallel_max_read_threads, PLUGIN_VAR_OPCMDARG,
                        "Global Degree of Parallel for turbo plugin", nullptr, nullptr, 128, 1, CT_MAX_PARAL_QUERY, 0);

// All global and session system variables must be published to mysqld before
// use. This is done by constructing a NULL-terminated array of the variables
// and linking to it in the plugin public interface.
static SYS_VAR *ctc_system_variables[] = {
  MYSQL_SYSVAR(sample_size),
  MYSQL_SYSVAR(lock_wait_timeout),
  MYSQL_SYSVAR(instance_id),
  MYSQL_SYSVAR(sampling_ratio),
  MYSQL_SYSVAR(enable_x_lock_instance),
  MYSQL_SYSVAR(db_datafile_autoextend),
  MYSQL_SYSVAR(db_datafile_size),
  MYSQL_SYSVAR(db_datafile_extend_size),
  MYSQL_SYSVAR(concurrent_ddl),
  MYSQL_SYSVAR(metadata_normalization),
  MYSQL_SYSVAR(max_cursors_no_autocommit),
  MYSQL_SYSVAR(version),
  MYSQL_SYSVAR(statistics_enabled),
  MYSQL_SYSVAR(autoinc_lock_mode),
  MYSQL_SYSVAR(cluster_role),
  MYSQL_SYSVAR(update_analyze_time),
  MYSQL_SYSVAR(stats_auto_recalc),
  MYSQL_SYSVAR(select_prefetch),
  MYSQL_SYSVAR(parallel_read_threads),
  MYSQL_SYSVAR(parallel_max_read_threads),
  nullptr
};

/** Operations for altering a table that CTC does not care about */
static const Alter_inplace_info::HA_ALTER_FLAGS CTC_INPLACE_IGNORE =
    Alter_inplace_info::ALTER_COLUMN_DEFAULT |
    Alter_inplace_info::ALTER_COLUMN_COLUMN_FORMAT |
    Alter_inplace_info::ALTER_COLUMN_STORAGE_TYPE |
    Alter_inplace_info::ALTER_RENAME |
    Alter_inplace_info::CHANGE_INDEX_OPTION |
    Alter_inplace_info::ADD_CHECK_CONSTRAINT |
    Alter_inplace_info::DROP_CHECK_CONSTRAINT |
    Alter_inplace_info::SUSPEND_CHECK_CONSTRAINT |
    Alter_inplace_info::ALTER_COLUMN_VISIBILITY;

/** Operations that CTC cares about and can perform without rebuild */
static const Alter_inplace_info::HA_ALTER_FLAGS CTC_ALTER_NOREBUILD =
    Alter_inplace_info::ADD_INDEX |
    Alter_inplace_info::ADD_UNIQUE_INDEX |
    Alter_inplace_info::ADD_SPATIAL_INDEX |
    Alter_inplace_info::DROP_FOREIGN_KEY |
    Alter_inplace_info::ADD_FOREIGN_KEY |
    Alter_inplace_info::DROP_INDEX |
    Alter_inplace_info::DROP_UNIQUE_INDEX |
    Alter_inplace_info::RENAME_INDEX |
    Alter_inplace_info::ALTER_COLUMN_NAME |
    Alter_inplace_info::ALTER_INDEX_COMMENT |
    Alter_inplace_info::ALTER_COLUMN_INDEX_LENGTH;

/** Operations for rebuilding a table in place */
static const Alter_inplace_info::HA_ALTER_FLAGS CTC_ALTER_REBUILD =
    Alter_inplace_info::ADD_PK_INDEX |
    Alter_inplace_info::DROP_PK_INDEX |
    Alter_inplace_info::CHANGE_CREATE_OPTION |
    Alter_inplace_info::ALTER_COLUMN_NULLABLE |
    Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE |
    Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |
    Alter_inplace_info::DROP_STORED_COLUMN |
    Alter_inplace_info::ADD_STORED_BASE_COLUMN |
    Alter_inplace_info::RECREATE_TABLE;

static const Alter_inplace_info::HA_ALTER_FLAGS CTC_ALTER_COL_ORDER =
      Alter_inplace_info::DROP_COLUMN |
      Alter_inplace_info::ALTER_VIRTUAL_COLUMN_ORDER |
      Alter_inplace_info::ALTER_STORED_COLUMN_ORDER;

static const Alter_inplace_info::HA_ALTER_FLAGS PARTITION_OPERATIONS =
      Alter_inplace_info::ADD_PARTITION | Alter_inplace_info::COALESCE_PARTITION;

static const Alter_inplace_info::HA_ALTER_FLAGS COLUMN_TYPE_OPERATIONS =
      Alter_inplace_info::ALTER_STORED_COLUMN_TYPE | Alter_inplace_info::ALTER_COLUMN_EQUAL_PACK_LENGTH;

//------------------------------------------------------------------------------
constexpr int max_prefetch_num = MAX_PREFETCH_REC_NUM;

// ref MAX_RECORD_BUFFER_SIZE, used for private record buffer assigned for each handler
constexpr int MAX_RECORD_BUFFER_SIZE_CTC = (1 * CTC_BUF_LEN);
constexpr uint64 INVALID_VALUE64 = 0xFFFFFFFFFFFFFFFFULL;

bool is_log_table = false;

#define ARRAY_SIZE_TWO 2

static int ctc_rollback_savepoint(handlerton *hton, THD *thd, void *savepoint);
handlerton *ctc_hton;

int ha_ctc_get_inst_id() { return ctc_instance_id; }

void ha_ctc_set_inst_id(uint32_t inst_id) { ctc_instance_id = inst_id; }

handlerton *get_ctc_hton() { return ctc_hton; }

/*
*  Check whether it is CREATE TABLE ... SELECT
*  reference: populate_table
 */
static inline bool is_create_table_check(MYSQL_THD thd) {
  return (thd->lex->sql_command == SQLCOM_CREATE_TABLE && thd->lex->is_exec_started());
}

dml_flag_t ctc_get_dml_flag(THD *thd, bool is_replace, bool auto_inc_used,
                            bool has_explicit_autoinc, bool dup_update) {
  dml_flag_t flag;
  flag.ignore = thd->lex->is_ignore();
  flag.no_foreign_key_check = (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS) ? 1 : 0;
  flag.no_cascade_check = false;
  flag.dd_update = (thd->variables.option_bits & OPTION_DD_UPDATE_CONTEXT) ? 1 : 0;
  flag.is_replace = is_replace;
  flag.no_logging = (thd->in_sub_stmt && (thd->in_sub_stmt & SUB_STMT_TRIGGER)) ? 1 : 0;
  flag.auto_inc_used = auto_inc_used;
  flag.has_explicit_autoinc = has_explicit_autoinc;
  flag.autoinc_lock_mode = ctc_autoinc_lock_mode;
  flag.dup_update= dup_update;
  flag.auto_inc_step = thd->variables.auto_increment_increment;
  flag.auto_inc_offset = thd->variables.auto_increment_offset;
  flag.auto_increase = false;
  flag.is_create_select = is_create_table_check(thd);
  return flag;
}

bool is_initialize() {
  return opt_initialize || opt_initialize_insecure;
}

bool is_starting() {
  return !mysqld_server_started && !is_initialize();
}

bool is_work_flow() {
  return mysqld_server_started && !is_initialize();
}

bool is_ctc_mdl_thd(THD* thd) {
  if (thd->query().str && string(thd->query().str) == "ctc_mdl_thd_notify") {
    return true;
  }
  return false;
}

// 是否为元数据归一的初始化流程
bool is_meta_version_initialize() {
#ifdef METADATA_NORMALIZED
  return is_initialize();
#else
  return false;
#endif
}

// 是否为--upgrade=FORCE
bool is_meta_version_upgrading_force() {
#ifdef METADATA_NORMALIZED
  return (opt_upgrade_mode == UPGRADE_FORCE);
#else
  return false;
#endif
}

bool is_alter_table_scan(bool m_error_if_not_empty) {
  return m_error_if_not_empty;
}

bool ddl_enabled_normal(MYSQL_THD thd) {
  handlerton* hton = get_ctc_hton();
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  assert(sess_ctx != nullptr);
  // 1.CTC_DDL_LOCAL_ENABLED被设置：不能从rewrite插件下发任何SQL语句
  // 2.CTC_DDL_LOCAL_ENABLED没被设置，ctc_concurrent_ddl=true，允许运行到判断是否广播的逻辑中
  // 3.CTC_DDL_LOCAL_ENABLED没被设置，ctc_concurrent_ddl=false，若CTC_DDL_ENABLED被设置，允许运行到判断是否广播的逻辑中
  return !(sess_ctx->set_flag & CTC_DDL_LOCAL_ENABLED) &&
         (ctc_concurrent_ddl == true || (sess_ctx->set_flag & CTC_DDL_ENABLED));
}

bool engine_skip_ddl(MYSQL_THD thd) {
  handlerton* hton = get_ctc_hton();
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  assert(sess_ctx != nullptr);
  // 接口流程不需要走到参天: 用于参天SYS库操作
  return (sess_ctx->set_flag & CTC_DDL_LOCAL_ENABLED) && ctc_concurrent_ddl == true;
}

bool engine_ddl_passthru(MYSQL_THD thd) {
  // 元数据归一初始化场景，接口流程需要走到参天
  if (is_initialize() || is_meta_version_upgrading_force()) {
    return false;
  }
  handlerton* hton = get_ctc_hton();
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  assert(sess_ctx != nullptr);
  bool is_mysql_local = (sess_ctx->set_flag & CTC_DDL_LOCAL_ENABLED);
  return is_initialize() || !mysqld_server_started || is_mysql_local;
}

bool ha_ctc::is_replay_ddl(MYSQL_THD thd) {
  char db_name[SMALL_RECORD_SIZE] = { 0 };
  ctc_split_normalized_name(table->s->normalized_path.str, db_name, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
  ctc_copy_name(db_name, db_name, SMALL_RECORD_SIZE);
  if (mysql_system_db.find(db_name) != mysql_system_db.end()) {
    return false;
  }
  
  handlerton* hton = get_ctc_hton();
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  assert(sess_ctx != nullptr);

  uint ctc_var_flag = (CTC_DDL_LOCAL_ENABLED | CTC_REPLAY_DDL);
  return (sess_ctx->set_flag & ctc_var_flag) == ctc_var_flag;
}

static int ctc_reg_instance() {
  uint32_t inst_id = MYSQL_PROC_START;
  uint32_t count = 0;
  ct_errno_t ret = CT_SUCCESS;

  while (count++ < CTC_START_TIMEOUT) {
    ret = (ct_errno_t)ctc_alloc_inst_id(&inst_id);
    if (ret == CT_SUCCESS) {
      ha_ctc_set_inst_id(inst_id);
      ctc_log_system("[CTC_INIT]:ctc reg instance success, inst_id:%u",
                     ha_ctc_get_inst_id());
      break;
    }
    ctc_log_system("[CTC_INIT]:ctc reg instance failed and sleep %u/%u", count,
                   CTC_START_TIMEOUT);
    sleep(1);
  }
  return convert_ctc_error_code_to_mysql(ret);
}

static void ctc_unreg_instance() {
  // 元数据归一流程初始化阶段下发参天, 主干不下发
  if (opt_initialize_insecure && !CHECK_HAS_MEMBER(handlerton, get_inst_id)) {
    return;
  }

  uint32_t inst_id;
  inst_id = ha_ctc_get_inst_id();
  ct_errno_t ret = (ct_errno_t)ctc_release_inst_id(inst_id);
  if (ret != CT_SUCCESS) {
    ctc_log_error("ctc release instance id:%u failed, ret:%d",
                  ha_ctc_get_inst_id(), ret);
  } else {
    ctc_log_system("ctc release instance id:%u success", ha_ctc_get_inst_id());
  }
}

/*
*  Check if the ALTER TABLE operations need table copy
*  reference: is_inplace_alter_impossible()
*  Alter_info::ALTER_TABLE_ALGORITHM_COPY tag set at ha_ctc::create or spcified by ALGORITHMY = COPY
*/
bool is_alter_table_copy(MYSQL_THD thd, const char *name) {
  if (!thd->lex->alter_info) {
    return false;
  }

  if (thd->lex->alter_info->requested_algorithm == Alter_info::ALTER_TABLE_ALGORITHM_COPY) {
    if (name == nullptr) {
      return true;
    }

    // COPY 算法时，正常表open接口下发参天, #sql开头的表直接返回，不下发参天
    if (is_prefix(name, tmp_file_prefix)) {
      return true;
    }
  }

  return false;
}

static bool is_lock_table(MYSQL_THD thd) {
  if (thd->lex->sql_command == SQLCOM_LOCK_TABLES) {
    return true;
  }
  return false;
}

void ha_ctc::prep_cond_push(const Item *cond) {
  Item *item = const_cast<Item *>(cond);
  Item *pushed_cond = nullptr;
  Item *remainder = nullptr;
  cond_push_term(item, pushed_cond, remainder, Item_func::COND_AND_FUNC);
  m_pushed_conds = pushed_cond;
  m_remainder_conds = remainder;
}

// 返回值检测设置
void ha_ctc::check_error_code_to_mysql(THD *thd, ct_errno_t *ret) {
  bool no_foreign_key_check = thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS;
  //判断ret是否返回死锁，死锁则回滚
  if (*ret == ERR_DEAD_LOCK) {
    thd_mark_transaction_to_rollback(thd, 1);
    return;
  } else if(no_foreign_key_check == false) {
    return;
  }

  static set<ct_errno_t> g_ctc_ignore_foreign_key_check_ret_value = {
    ERR_CONSTRAINT_VIOLATED_NO_FOUND, ERR_ROW_IS_REFERENCED};
  if (g_ctc_ignore_foreign_key_check_ret_value.count(*ret) > 0) {
    ctc_log_system("ctc_ignore_foreign_key_check catching.");
    *ret = CT_SUCCESS;
  }
}

bool ha_ctc::check_unsupported_operation(THD *thd, HA_CREATE_INFO *create_info) {
  // 不支持的操作
  if (thd->lex->alter_info && (thd->lex->alter_info->flags &
                               Alter_info::ALTER_EXCHANGE_PARTITION)) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "The current operation is not supported.");
    return true;
  }

  if (create_info != nullptr && (create_info->options & HA_LEX_CREATE_TMP_TABLE) && !IS_METADATA_NORMALIZATION()) {
    my_error(ER_NOT_ALLOWED_COMMAND, MYF(0));
    return HA_ERR_UNSUPPORTED;
  }

  if (create_info != nullptr && create_info->index_file_name) {
    my_error(ER_ILLEGAL_HA, MYF(0), table_share != nullptr ? table_share->table_name.str : " ");
    return true;
  }
  return false;
}

enum dd_index_keys {
  /** Index identifier */
  DD_INDEX_ID,
  /** Space id */
  DD_INDEX_SPACE_ID,
  /** Table id */
  DD_TABLE_ID,
  /** Root page number */
  DD_INDEX_ROOT,
  /** Creating transaction ID */
  DD_INDEX_TRX_ID,
  /** Sentinel */
  DD_INDEX__LAST
};
 
/** CTC private keys for dd::Table */
enum dd_table_keys {
  /** Auto-increment counter */
  DD_TABLE_AUTOINC,
  /** DATA DIRECTORY (static metadata) */
  DD_TABLE_DATA_DIRECTORY,
  /** Dynamic metadata version */
  DD_TABLE_VERSION,
  /** Discard flag. Please don't use it directly, and instead use
  dd_is_discarded and dd_set_discarded functions. Discard flag is defined
  for both dd::Table and dd::Partition and it's easy to confuse.
  The functions will choose right implementation for you, depending on
  whether the argument is dd::Table or dd::Partition. */
  DD_TABLE_DISCARD,
  /** Columns before first instant ADD COLUMN */
  DD_TABLE_INSTANT_COLS,
  /** Sentinel */
  DD_TABLE__LAST
};
 
const char *const dd_index_key_strings[DD_INDEX__LAST] = {
    "id", "space_id", "table_id", "root", "trx_id"};
 
static constexpr dd::Object_id g_dd_dict_space_id = 1;
 
/** CTC private key strings for dd::Table. @see dd_table_keys */
const char *const dd_table_key_strings[DD_TABLE__LAST] = {
    "autoinc", "data_directory", "version", "discard", "instant_col"};
 
static void dd_set_autoinc(dd::Properties &se_private_data, uint64 autoinc) {
  /* The value of "autoinc" here is the AUTO_INCREMENT attribute
  specified at table creation. AUTO_INCREMENT=0 will silently
  be treated as AUTO_INCREMENT=1. Likewise, if no AUTO_INCREMENT
  attribute was specified, the value would be 0. */
 
  if (autoinc > 0) {
    /* CTC persists the "previous" AUTO_INCREMENT value. */
    autoinc--;
  }
 
  uint64 version = 0;
 
  if (se_private_data.exists(dd_table_key_strings[DD_TABLE_AUTOINC])) {
    /* Increment the dynamic metadata version, so that any previously buffered persistent dynamic metadata
       will be ignored after this transaction commits. */
 
    if (!se_private_data.get(dd_table_key_strings[DD_TABLE_VERSION],
                             &version)) {
      version++;
    } else {
      /* incomplete se_private_data */
      assert(0);
    }
  }
 
  se_private_data.set(dd_table_key_strings[DD_TABLE_VERSION], version);
  se_private_data.set(dd_table_key_strings[DD_TABLE_AUTOINC], autoinc);
}
 
bool ha_ctc::get_se_private_data(dd::Table *dd_table, bool reset) {
  static uint n_tables = 1024;
  static uint n_indexes = 0;
  static uint n_pages = 4;
  
  DBUG_TRACE;
  assert(dd_table != nullptr);
 
  if (reset) {
    n_tables = 0;
    n_indexes = 0;
    n_pages = 4;
  }
 
  if ((*(const_cast<const dd::Table *>(dd_table))->columns().begin())
          ->is_auto_increment()) {
    dd_set_autoinc(dd_table->se_private_data(), 0);
  }
 
  dd_table->set_se_private_id(++n_tables);
  dd_table->set_tablespace_id(g_dd_dict_space_id);
 
  /* Set the table id for each column to be conform with the
  implementation in dd_write_table(). */
  for (auto dd_column : *dd_table->table().columns()) {
    dd_column->se_private_data().set(dd_index_key_strings[DD_TABLE_ID],
                                     n_tables);
  }
 
  for (dd::Index *i : *dd_table->indexes()) {
    i->set_tablespace_id(g_dd_dict_space_id);
 
    dd::Properties &p = i->se_private_data();
 
    p.set(dd_index_key_strings[DD_INDEX_ROOT], n_pages++);
    p.set(dd_index_key_strings[DD_INDEX_ID], ++n_indexes);
    p.set(dd_index_key_strings[DD_INDEX_TRX_ID], 0);
    p.set(dd_index_key_strings[DD_INDEX_SPACE_ID], 0);
    p.set(dd_index_key_strings[DD_TABLE_ID], n_tables);
  }
 
  return false;
}

static handler *ctc_create_handler(handlerton *hton, TABLE_SHARE *table, bool partitioned, MEM_ROOT *mem_root) {
  if (partitioned) {
    ha_ctcpart *file = new (mem_root) ha_ctcpart(hton, table);
    if (file && (file->initialize() || file->init_partitioning(mem_root))) {
      delete file;
      return nullptr;
    }

    return file;
  }

  ha_ctc *file = new (mem_root) ha_ctc(hton, table);
  if (file && file->initialize()) {
    delete file;
    return nullptr;
  }

  return file;
}

static bool ctc_check_if_log_table(const char* db_name, const char* table_name) {
  LEX_CSTRING cstr_db_name = {db_name, strlen(db_name)};
  LEX_CSTRING cstr_table_name = {table_name, strlen(table_name)};
  if (cstr_db_name.length == MYSQL_SCHEMA_NAME.length && !my_strcasecmp(system_charset_info, cstr_db_name.str, MYSQL_SCHEMA_NAME.str)) {
    if (cstr_table_name.length == GENERAL_LOG_NAME.length && !my_strcasecmp(system_charset_info, cstr_table_name.str, GENERAL_LOG_NAME.str)) {
      return true;
    }
    if (cstr_table_name.length == SLOW_LOG_NAME.length && !my_strcasecmp(system_charset_info, cstr_table_name.str, SLOW_LOG_NAME.str)) {
      return true;
    }
  }
  return false;
}

/**
  @brief Check if the given db.tablename is a system table for this SE.

  @param db                         Database name to check.
  @param table_name                 table name to check.
  @param is_sql_layer_system_table  if the supplied db.table_name is a SQL
                                    layer system table.

  @retval true   Given db.table_name is supported system table.
  @retval false  Given db.table_name is not a supported system table.
*/
static bool ctc_is_supported_system_table(const char *db MY_ATTRIBUTE((unused)),
                                          const char *table_name MY_ATTRIBUTE((unused)),
                                          bool is_sql_layer_system_table MY_ATTRIBUTE((unused))) {

  if (IS_METADATA_NORMALIZATION()) {
    return true;
  }
  return false;
}

/**
  @brief Forms a precise type from the < 4.1.2 format precise type plus the
 charset-collation code.
  @param old_prtype: the MySQL type code and the flags DATA_BINARY_TYPE etc.
  @param charset_coll: MySQL charset-collation code

  @return precise type, including the charset-collation code.
 */
uint ctc_dtype_form_prtype(uint old_prtype, uint charset_coll) {
  return (old_prtype + (charset_coll << 16));
}

uint get_ctc_type_from_mysql_dd_type(uint *unsigned_flag, uint *binary_type, uint *charset_no,
                                           dd::enum_column_types dd_type,
                                           const CHARSET_INFO *field_charset,
                                           bool is_unsigned) {
  *unsigned_flag = 0;
  *binary_type = DATA_BINARY_TYPE;
  *charset_no = 0;

  switch (dd_type) {
    case dd::enum_column_types::ENUM:
    case dd::enum_column_types::SET:
      /* SQL-layer has its own unsigned flag set to zero, even though
      internally this is an unsigned integer type. */
      *unsigned_flag = DATA_UNSIGNED;
      /* ENUM and SET are handled as string types by SQL-layer,
      hence the charset check. */
      if (field_charset != &my_charset_bin) *binary_type = 0;
      return (DATA_INT);
    case dd::enum_column_types::VAR_STRING: /* old <= 4.1 VARCHAR. */
    case dd::enum_column_types::VARCHAR:    /* new >= 5.0.3 true VARCHAR. */
      *charset_no = field_charset->number;
      if (field_charset == &my_charset_bin) {
        return (DATA_BINARY);
      } else {
        *binary_type = 0;
        if (field_charset == &my_charset_latin1) {
          return (DATA_VARCHAR);
        } else {
          return (DATA_VARMYSQL);
        }
      }
    case dd::enum_column_types::BIT:
      /* MySQL always sets unsigned flag for both its BIT types. */
      *unsigned_flag = DATA_UNSIGNED;
      *charset_no = my_charset_bin.number;
      return (DATA_FIXBINARY);
    case dd::enum_column_types::STRING:
      *charset_no = field_charset->number;
      if (field_charset == &my_charset_bin) {
        return (DATA_FIXBINARY);
      } else {
        *binary_type = 0;
        if (field_charset == &my_charset_latin1) {
          return (DATA_CHAR);
        } else {
          return (DATA_MYSQL);
        }
      }
    case dd::enum_column_types::DECIMAL:
    case dd::enum_column_types::FLOAT:
    case dd::enum_column_types::DOUBLE:
    case dd::enum_column_types::NEWDECIMAL:
    case dd::enum_column_types::LONG:
    case dd::enum_column_types::LONGLONG:
    case dd::enum_column_types::TINY:
    case dd::enum_column_types::SHORT:
    case dd::enum_column_types::INT24:
      /* Types based on Field_num set unsigned flag from value stored
      in the data-dictionary (YEAR being the exception). */
      if (is_unsigned) *unsigned_flag = DATA_UNSIGNED;
      switch (dd_type) {
        case dd::enum_column_types::DECIMAL:
          return (DATA_DECIMAL);
        case dd::enum_column_types::FLOAT:
          return (DATA_FLOAT);
        case dd::enum_column_types::DOUBLE:
          return (DATA_DOUBLE);
        case dd::enum_column_types::NEWDECIMAL:
          *charset_no = my_charset_bin.number;
          return (DATA_FIXBINARY);
        default:
          break;
      }
      return (DATA_INT);
    case dd::enum_column_types::DATE:
    case dd::enum_column_types::NEWDATE:
    case dd::enum_column_types::TIME:
    case dd::enum_column_types::DATETIME:
      return (DATA_INT);
    case dd::enum_column_types::YEAR:
    case dd::enum_column_types::TIMESTAMP:
      /* MySQL always sets unsigned flag for YEAR and old TIMESTAMP type. */
      *unsigned_flag = DATA_UNSIGNED;
      return (DATA_INT);
    case dd::enum_column_types::TIME2:
    case dd::enum_column_types::DATETIME2:
    case dd::enum_column_types::TIMESTAMP2:
      *charset_no = my_charset_bin.number;
      return (DATA_FIXBINARY);
    case dd::enum_column_types::GEOMETRY:
      /* Field_geom::binary() is always true. */
      return (DATA_GEOMETRY);
    case dd::enum_column_types::TINY_BLOB:
    case dd::enum_column_types::MEDIUM_BLOB:
    case dd::enum_column_types::BLOB:
    case dd::enum_column_types::LONG_BLOB:
      *charset_no = field_charset->number;
      if (field_charset != &my_charset_bin) *binary_type = 0;
      return (DATA_BLOB);
    case dd::enum_column_types::JSON:
      /* JSON fields are stored as BLOBs.
      Field_json::binary() always returns true even though data in
      such columns are stored in UTF8. */
      *charset_no = my_charset_utf8mb4_bin.number;
      return (DATA_BLOB);
    case dd::enum_column_types::TYPE_NULL:
      /* Compatibility with get_innobase_type_from_mysql_type(). */
      *charset_no = field_charset->number;
      if (field_charset != &my_charset_bin) *binary_type = 0;
      break;
    default:
      return -1;
  }
  return (0);
}

void ctc_dict_mem_fill_column_struct(dict_col *column, uint mtype, uint prtype, uint col_len) {
  column->mtype = (unsigned int)mtype;
  column->prtype = (unsigned int)prtype;
  column->len = (unsigned int)col_len;
}

/** Constructs fake dict_col describing column for foreign key type
compatibility check from column description in Ha_fk_column_type form.

  @note dict_col_t which is produced by this call is not valid for general purposes.
  @param[out]	col		dict_col filled by this function
  @param[in]	fk_col_type	foreign key type information
*/
static void ctc_fill_fake_column_struct(
    dict_col *col, const Ha_fk_column_type *fk_col_type) {
  uint unsigned_type;
  uint binary_type;
  uint charset_no;

  uint mtype = get_ctc_type_from_mysql_dd_type(&unsigned_type, &binary_type, &charset_no, fk_col_type->type,
      fk_col_type->field_charset, fk_col_type->is_unsigned);

  uint fake_prtype = ctc_dtype_form_prtype(unsigned_type | binary_type, charset_no);
  /* Fake prtype only contains info which is relevant for foreign key
  type compatibility check, especially the info used in ctc_cmp_cols_are_equal. */

  uint col_len = calc_pack_length(fk_col_type->type, fk_col_type->char_length, fk_col_type->elements_count,
      true, fk_col_type->numeric_scale, fk_col_type->is_unsigned);

  memset(col, 0, sizeof(dict_col));
  ctc_dict_mem_fill_column_struct(col, mtype, fake_prtype, col_len);
}

/** Checks if a data main type is a string type. Also a BLOB is considered a
 string type.
  @return true if string type */
bool ctc_dtype_is_string_type(uint mtype)  {
  if (mtype <= DATA_BLOB || mtype == DATA_MYSQL || mtype == DATA_VARMYSQL) {
    return (true);
  }

  return (false);
}

/** Checks if a type is a binary string type. Note that for tables created with
 < 4.0.14, we do not know if a DATA_BLOB column is a BLOB or a TEXT column. For
 those DATA_BLOB columns this function currently returns FALSE.
  @param mtype main data type
  @param prtype precise type
  @return true if binary string type
*/
bool ctc_dtype_is_binary_string_type(uint mtype, uint prtype) {
  if ((mtype == DATA_FIXBINARY) || (mtype == DATA_BINARY) ||
      (mtype == DATA_BLOB && (prtype & DATA_BINARY_TYPE))) {
    return (true);
  }

  return (false);
}

/** Checks if a type is a non-binary string type. That is, dtype_is_string_type
 is TRUE and dtype_is_binary_string_type is FALSE. Note that for tables created
 with < 4.0.14, we do not know if a DATA_BLOB column is a BLOB or a TEXT column.
 For those DATA_BLOB columns this function currently returns TRUE.
 @param mtype main data type
 @param prtype precise type
 @return true if non-binary string type
 */
bool ctc_dtype_is_non_binary_string_type(uint mtype, uint prtype) {
  if (ctc_dtype_is_string_type(mtype) == true &&
      ctc_dtype_is_binary_string_type(mtype, prtype) == false) {
    return (true);
  }

  return (false);
}

/** Gets the MySQL charset-collation code for MySQL string types.
 @return MySQL charset-collation code */
static inline uint ctc_dtype_get_charset_coll(uint prtype) {
  return ((prtype >> 16) & CHAR_COLL_MASK);
}

bool ctc_cmp_cols_are_equal(const dict_col *col1, const dict_col *col2,
                        bool check_charsets) {
  if (ctc_dtype_is_non_binary_string_type(col1->mtype, col1->prtype) &&
      ctc_dtype_is_non_binary_string_type(col2->mtype, col2->prtype)) {
    /* Both are non-binary string types: they can be compared if
    and only if the charset-collation is the same */

    if (check_charsets) {
      return (ctc_dtype_get_charset_coll(col1->prtype) ==
              ctc_dtype_get_charset_coll(col2->prtype));
    } else {
      return (true);
    }
  }

  if (ctc_dtype_is_binary_string_type(col1->mtype, col1->prtype) &&
      ctc_dtype_is_binary_string_type(col2->mtype, col2->prtype)) {
    /* Both are binary string types: they can be compared */
    return (true);
  }

  if (col1->mtype != col2->mtype) {
    return (false);
  }

  if (col1->mtype == DATA_INT &&
      (col1->prtype & DATA_UNSIGNED) != (col2->prtype & DATA_UNSIGNED)) {
    /* The storage format of an unsigned integer is different
    from a signed integer: in a signed integer we OR
    0x8000... to the value of positive integers. */
    return (false);
  }
  return (col1->mtype != DATA_INT || col1->len == col2->len);
}


/** Check if types of child and parent columns in foreign key are compatible.
  @param[in]	check_charsets		Indicates whether we need to check that charsets of string columns
                                        match. Which is true in most cases.
  @return True if types are compatible, False if not.
*/
static bool ctc_check_fk_column_compat(const Ha_fk_column_type *child_column_type,
                                       const Ha_fk_column_type *parent_column_type, bool check_charsets) {
  dict_col dict_child_col, dict_parent_col;

  ctc_fill_fake_column_struct(&dict_child_col, child_column_type);
  ctc_fill_fake_column_struct(&dict_parent_col, parent_column_type);

  return (ctc_cmp_cols_are_equal(&dict_child_col, &dict_parent_col, check_charsets));
}

/*
  Return a session context for current thread.
  Initialize one if session context is not exists.
  A session context is one-to-one mapping of thread.
*/
thd_sess_ctx_s *get_or_init_sess_ctx(handlerton *hton, THD *thd) {
  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);
  if (sess_ctx == nullptr) {
    sess_ctx = (thd_sess_ctx_s *)my_malloc(PSI_NOT_INSTRUMENTED,
                                           sizeof(thd_sess_ctx_s), MYF(MY_WME));
    if (sess_ctx == nullptr) {
      ctc_log_error("my_malloc error for sess_ctx");
      return nullptr;
    }
    
    memset(sess_ctx, 0xFF, sizeof(thd_sess_ctx_s));
    sess_ctx->is_ctc_trx_begin = 0;
    sess_ctx->sql_stat_start = 0;
    sess_ctx->cursors_map = new unordered_map<ctc_handler_t *, uint64_t>;
    sess_ctx->invalid_cursors = nullptr;
    assert(sess_ctx->cursors_map->size() == 0);
    sess_ctx->msg_buf = nullptr;
    sess_ctx->set_flag = 0;
    thd_set_ha_data(thd, hton, sess_ctx);
  }
  return sess_ctx;
}

/*
  Called by handlerton functions to get a new ctc handler.
  This ctc handler will be only use for one time and initialized with session context.
  Handlerton functions need it since they don't have m_tch member.
*/
int get_tch_in_handler_data(handlerton *hton, THD *thd, ctc_handler_t &tch, bool alloc_msg_buf MY_ATTRIBUTE((unused))) {
  memset(&tch, 0, sizeof(tch));
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  if (sess_ctx == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  
  if (sess_ctx->thd_id != thd->thread_id()) {
    sess_ctx->sess_addr = INVALID_VALUE64;
    sess_ctx->thd_id = thd->thread_id();
    sess_ctx->bind_core = 0;
    sess_ctx->is_ctc_trx_begin = 0;
    sess_ctx->sql_stat_start = 0;
  }
  
  tch.inst_id = ctc_instance_id;
  tch.ctx_addr = INVALID_VALUE64;
  tch.sess_addr = sess_ctx->sess_addr;
  tch.thd_id = sess_ctx->thd_id;
  tch.bind_core = sess_ctx->bind_core;
  tch.sql_command = (uint8_t)thd->lex->sql_command;
  tch.query_id = thd->query_id;
  tch.sql_stat_start = sess_ctx->sql_stat_start;
  tch.pre_sess_addr = 0;
#ifndef WITH_CANTIAN
  tch.msg_buf = sess_ctx->msg_buf;

  if (sess_ctx->msg_buf == nullptr && alloc_msg_buf) {
    void *shm_inst = get_one_shm_inst(&tch);
    sess_ctx->msg_buf = (void*)shm_alloc((shm_seg_s *)shm_inst, sizeof(dsw_message_block_t));
    sem_init(&(((dsw_message_block_t*)(sess_ctx->msg_buf))->head.sem), 1, 0);
    tch.msg_buf = sess_ctx->msg_buf;
  }
#endif
  return 0;
}

static void ctc_copy_cursors_to_free(thd_sess_ctx_s *sess_ctx, uint64_t *cursors, uint32_t left) {
  uint32_t idx = 0;
  if (sess_ctx->invalid_cursors && sess_ctx->invalid_cursors->size() > 0) {
    uint32_t invalid_csize = sess_ctx->invalid_cursors->size();
    memcpy(cursors, &(*sess_ctx->invalid_cursors)[0], invalid_csize * sizeof(uint64_t));
    sess_ctx->invalid_cursors->clear();
    idx = invalid_csize;
  }
  if (left == 0) {
    unordered_map<ctc_handler_t *, uint64_t>::iterator it;
    for (it = sess_ctx->cursors_map->begin(); it != sess_ctx->cursors_map->end(); it++) {
      cursors[idx++] = it->second;
    }
    sess_ctx->cursors_map->clear();
  }
}

void update_sess_ctx_cursor_by_tch(ctc_handler_t &tch, handlerton *hton, THD *thd) {
  if (tch.cursor_addr == INVALID_VALUE64) {
    return;
  }

  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);
  assert(sess_ctx != nullptr);
  unordered_map<ctc_handler_t *, uint64_t>::iterator it;
  it = sess_ctx->cursors_map->find(&tch);
  if (it != sess_ctx->cursors_map->end()) {
    uint64_t current_cursor = it->second;
    if (current_cursor == tch.cursor_addr) {
      return;
    }
    if (sess_ctx->invalid_cursors == nullptr) {
      sess_ctx->invalid_cursors = new vector<uint64_t>;
    }
    sess_ctx->invalid_cursors->push_back(current_cursor);
  }
  (*sess_ctx->cursors_map)[&tch] = tch.cursor_addr;

  if (sess_ctx->invalid_cursors == nullptr) {
    return;
  }
  int32_t total_csize = sess_ctx->cursors_map->size() + sess_ctx->invalid_cursors->size();
  if (total_csize >= SESSION_CURSOR_NUM) {
    uint32_t free_csize = sess_ctx->invalid_cursors->size();
    uint64_t *cursors = (uint64_t *)ctc_alloc_buf(&tch, sizeof(uint64_t) * free_csize);
    if ((total_csize != 0) && (cursors == nullptr)) {
      ctc_log_error("ctc_alloc_buf for cursors in update_sess_ctx_cursor_by_tch failed");
    }
    assert((total_csize == 0) ^ (cursors != nullptr));
    ctc_copy_cursors_to_free(sess_ctx, cursors, 1);
    assert(sess_ctx->invalid_cursors->empty());
    ctc_log_system("[FREE CURSORS] free %d cursors in advance.", free_csize);
    ctc_free_session_cursors(&tch, cursors, free_csize);
    ctc_free_buf(&tch, (uint8_t *)cursors);
  }
}

/*
  Since session address and sql_stat_start may be changed in tch after processing in cantian,
  call this method if session address or sql_stat_start may be updated in tch.
*/
void update_sess_ctx_by_tch(ctc_handler_t &tch, handlerton *hton, THD *thd) {
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  if (sess_ctx == nullptr) {
    ctc_log_error("update_sess_ctx_by_tch failed, thd_sess_ctx_s my_malloc error!");
    return;
  }
  
  sess_ctx->thd_id = tch.thd_id;
  sess_ctx->sess_addr = tch.sess_addr;
  sess_ctx->bind_core = tch.bind_core;
  sess_ctx->sql_stat_start = tch.sql_stat_start;
}

/*
  1. Make sure that session address is invalid everytime thd's been updated.
  2. Call this method if the following ctc interface may be the
     first call of a dml sql (may use sql_stat_start).
*/
void update_member_tch(ctc_handler_t &tch, handlerton *hton, THD *thd, bool alloc_msg_buf MY_ATTRIBUTE((unused))) {
  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);
  if (sess_ctx == nullptr || sess_ctx->thd_id != thd->thread_id()) {
    tch.thd_id = thd->thread_id();
    tch.sess_addr = INVALID_VALUE64;
    tch.bind_core = 0;
    tch.sql_command = (uint8_t)thd->lex->sql_command;
    tch.query_id = thd->query_id;
    tch.sql_stat_start = 0;
    tch.cursor_ref = 0;
    tch.pre_sess_addr = 0;
    tch.msg_buf = nullptr;
    tch.change_data_capture = 0;
    tch.read_only_in_ct = false;
    return;
  }

  tch.cursor_addr = (tch.sess_addr == sess_ctx->sess_addr) ? tch.cursor_addr : INVALID_VALUE64;
  tch.sess_addr = sess_ctx->sess_addr;
  tch.thd_id = sess_ctx->thd_id;
  tch.bind_core = sess_ctx->bind_core;
  tch.sql_command = (uint8_t)thd->lex->sql_command;
  tch.query_id = thd->query_id;
  tch.sql_stat_start = sess_ctx->sql_stat_start;
  tch.pre_sess_addr = 0;
#ifndef WITH_CANTIAN
  tch.msg_buf = sess_ctx->msg_buf;

  if (sess_ctx->msg_buf == nullptr && alloc_msg_buf) {
    void *shm_inst = get_one_shm_inst(&tch);
    sess_ctx->msg_buf = (void*)shm_alloc((shm_seg_s *)shm_inst, sizeof(dsw_message_block_t));
    sem_init(&(((dsw_message_block_t*)(sess_ctx->msg_buf))->head.sem), 1, 0);
    tch.msg_buf = sess_ctx->msg_buf;
  }
#endif
#ifdef METADATA_NORMALIZED
  if (thd->is_reading_dd) {
    tch.sql_stat_start = 1;
  }
#endif
}

// called in disconnect when current thd is no longer used
void release_sess_ctx(thd_sess_ctx_s *sess_ctx, handlerton *hton, THD *thd) {
  assert(sess_ctx);
  delete sess_ctx->cursors_map;
  sess_ctx->cursors_map = nullptr;
  if (sess_ctx->invalid_cursors) {
    delete sess_ctx->invalid_cursors;
    sess_ctx->invalid_cursors = nullptr;
  }
#ifndef WITH_CANTIAN
  if (sess_ctx->msg_buf != nullptr) {
    sem_destroy(&(((dsw_message_block_t*)(sess_ctx->msg_buf))->head.sem));
    shm_free(nullptr, sess_ctx->msg_buf);
    sess_ctx->msg_buf = nullptr;
  }
#endif
  my_free(sess_ctx);
  thd_set_ha_data(thd, hton, nullptr);
}

/** Creates an CTC transaction struct for the thd if it does not yet have
 one. Starts a new CTC transaction if a transaction is not yet started. And
 assigns a new snapshot for a consistent read if the transaction does not yet
 have one.
 @return 0 */
static int ctc_start_trx_and_assign_scn(
    handlerton *hton, /*!< in: CTC handlerton */
    THD *thd)         /*!< in: MySQL thread handle of the user for
                      whom the transaction should be committed */
{
  DBUG_TRACE;

  if (engine_ddl_passthru(thd) && is_alter_table_copy(thd)) {
    return 0;
  }

  /* Assign a read view if the transaction does not have it yet.
  Do this only if transaction is using REPEATABLE READ isolation
  level. */
  enum_tx_isolation mysql_isolation = thd_get_trx_isolation(thd);
  if (mysql_isolation != ISO_REPEATABLE_READ) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, HA_ERR_UNSUPPORTED,
                        "CTC: WITH CONSISTENT SNAPSHOT"
                        " was ignored because this phrase"
                        " can only be used with"
                        " REPEATABLE READ isolation level.");
    return 0;
  }

  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(ctc_hton, thd);
  // get_tch_in_handler_data若成功返回，则sess_ctx肯定不为空
  assert(sess_ctx != nullptr);
  assert(sess_ctx->is_ctc_trx_begin == 0);
  if (sess_ctx == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  uint32_t autocommit = !thd->in_multi_stmt_transaction_mode();
  int isolation_level = isolation_level_to_cantian(mysql_isolation);
  uint32_t lock_wait_timeout = THDVAR(thd, lock_wait_timeout);
  ctc_trx_context_t trx_context = {isolation_level, autocommit, lock_wait_timeout, false};
  bool is_mysql_local = (sess_ctx->set_flag & CTC_DDL_LOCAL_ENABLED);
  ct_errno_t ret = (ct_errno_t)ctc_trx_begin(&tch, trx_context, is_mysql_local);
  update_sess_ctx_by_tch(tch, hton, thd);
  if (ret != CT_SUCCESS) {
    ctc_log_error("start trx failed with error code: %d", ret);
    return convert_ctc_error_code_to_mysql(ret);
  }
  sess_ctx->is_ctc_trx_begin = 1;
  trans_register_ha(thd, !autocommit, hton, nullptr);
  return 0;
}

void broadcast_and_reload_buffer(ctc_handler_t *tch, ctc_invalidate_broadcast_request *req) {
  if (req->buff_len + sizeof(invalidate_obj_entry_t) > DD_BROADCAST_RECORD_LENGTH) {
    (void)ctc_broadcast_mysql_dd_invalidate(tch, req);
    memset(req->buff, 0, DD_BROADCAST_RECORD_LENGTH);
    req->buff_len = 1;
  }
}

template <typename T>
static typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), void>::type
  invalidate_remote_dd(T *thd, ctc_handler_t *tch)
{
  ctc_invalidate_broadcast_request req;
  req.mysql_inst_id = ctc_instance_id;
  req.buff_len = 1;
  req.is_dcl = false;
  req.is_flush = (tch->sql_command == SQLCOM_FLUSH) ? true : false;
  invalidate_obj_entry_t *obj = NULL;
 
  for (auto invalidate_it : thd->invalidates()) {
    switch (invalidate_it.second) {
      case T::OBJ_ABSTRACT_TABLE:
      case T::OBJ_EVENT:
      case T::OBJ_COLUMN_STATISTICS:
      case T::OBJ_RT_PROCEDURE:
      case T::OBJ_RT_FUNCTION:
          broadcast_and_reload_buffer(tch, &req);
          obj = (invalidate_obj_entry_t *)((char *)req.buff + req.buff_len);
          obj->type = invalidate_it.second;
          strncpy(obj->first, invalidate_it.first.first.c_str(), SMALL_RECORD_SIZE - 1);
          strncpy(obj->second, invalidate_it.first.second.c_str(), SMALL_RECORD_SIZE - 1);
          req.buff_len += sizeof(invalidate_obj_entry_t);
          printf("\n[invalidate_remote_dd] add to invalidate %d, %s, %s.\n", invalidate_it.second, invalidate_it.first.first.c_str(), invalidate_it.first.second.c_str()); fflush(stdout);
          break;
      case T::OBJ_SCHEMA:
      case T::OBJ_TABLESPACE:
      case T::OBJ_RESOURCE_GROUP:
      case T::OBJ_SPATIAL_REFERENCE_SYSTEM:
          broadcast_and_reload_buffer(tch, &req);
          obj = (invalidate_obj_entry_t *)((char *)req.buff + req.buff_len);
          obj->type = invalidate_it.second;
          strncpy(obj->first, invalidate_it.first.first.c_str(), SMALL_RECORD_SIZE - 1);
          strncpy(obj->second, "", SMALL_RECORD_SIZE - 1);
          req.buff_len += sizeof(invalidate_obj_entry_t);
          printf("\n[invalidate_remote_dd] add to invalidate %d, %s, %s.\n", invalidate_it.second, invalidate_it.first.first.c_str(), invalidate_it.first.second.c_str()); fflush(stdout);
          break;
      case T::OBJ_CHARSET:
      case T::OBJ_COLLATION:
          printf("\n[invalidate_remote_dd] add to invalidate %d, %s, %s.\n", invalidate_it.second, invalidate_it.first.first.c_str(), invalidate_it.first.second.c_str()); fflush(stdout);
          break;
      default:
          break;
    }
  }
  req.buff[0] = '1';
  (void)ctc_broadcast_mysql_dd_invalidate(tch, &req);
}

template <typename T>
static typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), void>::type
  invalidate_remote_dd(T *thd MY_ATTRIBUTE((unused)), ctc_handler_t *tch MY_ATTRIBUTE((unused))) {
  // do nothing
}

bool invalidate_remote_dcl_cache(ctc_handler_t *tch)
{
  ctc_invalidate_broadcast_request req;
  req.mysql_inst_id = ctc_instance_id;
  req.buff_len = 0;
  req.is_dcl = true;
  req.is_flush = (tch->sql_command == SQLCOM_FLUSH) ? true : false;
  bool result = ctc_broadcast_mysql_dd_invalidate(tch, &req);
  return result;
}

static void ctc_register_trx(handlerton *hton, THD *thd) {
  trans_register_ha(thd, false, hton, nullptr);
  if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
    trans_register_ha(thd, true, hton, nullptr);
  }
}

// 利用SFINAE特性，控制是否调用thd->is_empty()
template <typename T>
static typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, is_empty), void>::type
  commit_preprocess(T* thd, ctc_handler_t *tch) {
  if (is_work_flow() && !thd->is_empty() && !thd->is_attachable_transaction_active()) {
    (void)invalidate_remote_dd(thd, tch);
    thd->clear();
  }
}

template <typename T>
static typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, is_empty), void>::type
  commit_preprocess(T* thd MY_ATTRIBUTE((unused)), ctc_handler_t *tch MY_ATTRIBUTE((unused))) {
  // no action here
}

#ifdef METADATA_NORMALIZED
static void attachable_trx_update_pre_addr(THD *thd, ctc_handler_t *tch, bool set_to_pre_addr) {
  if (thd->is_attachable_transaction_active() && (thd->tx_isolation == ISO_READ_UNCOMMITTED)
      && (thd->pre_sess_addr != 0) && thd->query_plan.get_command() == SQLCOM_RENAME_TABLE) {
    tch->pre_sess_addr = set_to_pre_addr ? thd->pre_sess_addr : 0;
  }
}
#else
static void attachable_trx_update_pre_addr(THD *thd MY_ATTRIBUTE((unused)),
                               ctc_handler_t *tch MY_ATTRIBUTE((unused)), bool set_to_pre_addr MY_ATTRIBUTE((unused))) {
}
#endif

static void ctc_free_cursors_no_autocommit(THD *thd, ctc_handler_t *tch, thd_sess_ctx_s *sess_ctx) {
  if (!thd->in_multi_stmt_transaction_mode()) {
    return;
  }

  int32_t total_csize = sess_ctx->cursors_map->size();
  if (sess_ctx->invalid_cursors != nullptr) {
    total_csize += sess_ctx->invalid_cursors->size();
  }

  if (total_csize <= ctc_max_cursors_no_autocommit) {
    return;
  }

  uint64_t *cursors = (uint64_t *)ctc_alloc_buf(tch, sizeof(uint64_t) * total_csize);
  if ((total_csize != 0) && (cursors == nullptr)) {
    ctc_log_error("ctc_alloc_buf for cursors in ctc_free_cursors_no_autocommit failed");
  }
  assert((total_csize == 0) ^ (cursors != nullptr));
  ctc_copy_cursors_to_free(sess_ctx, cursors, 0);
  ctc_free_session_cursors(tch, cursors, total_csize);
  ctc_free_buf(tch, (uint8_t *)cursors);
}

/**
  Commits a transaction in an ctc database or marks an SQL statement ended.
  @param: hton in, ctc handlerton
  @param: thd in, MySQL thread handle of the user for whom the transaction
  should be committed
  @param: commit_trx in, true - commit transaction false - the current SQL
  statement ended
  @return 0 or deadlock error if the transaction was aborted by another
         higher priority transaction.
*/
static int ctc_commit(handlerton *hton, THD *thd, bool commit_trx) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd) || is_create_table_check(thd) || is_lock_table(thd))) {
    END_RECORD_STATS(EVENT_TYPE_COMMIT)
    return 0;
  }

  ctc_handler_t tch;
  bool is_ddl_commit = false;
  bool will_commit = commit_trx || (!thd->in_multi_stmt_transaction_mode());
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch, will_commit));
  ct_errno_t ret = CT_SUCCESS;
  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);
  assert(sess_ctx != nullptr);

  if (will_commit) {
    commit_preprocess(thd, &tch);
    attachable_trx_update_pre_addr(thd, &tch, true);

    int32_t total_csize = sess_ctx->cursors_map->size();
    if (sess_ctx->invalid_cursors != nullptr) {
      total_csize += sess_ctx->invalid_cursors->size();
    }
    uint64_t *cursors = (uint64_t *)ctc_alloc_buf(&tch, sizeof(uint64_t) * total_csize);
    if ((total_csize != 0) && (cursors == nullptr)) {
      ctc_log_error("ctc_alloc_buf for cursors in ctc_commit failed");
    }
    assert((total_csize == 0) ^ (cursors != nullptr));
    ctc_copy_cursors_to_free(sess_ctx, cursors, 0);
    ret = (ct_errno_t)ctc_trx_commit(&tch, cursors, total_csize, &is_ddl_commit);
    ctc_free_buf(&tch, (uint8_t *)cursors);
    if (ret != CT_SUCCESS) {
      ctc_log_error("commit atomic ddl failed with error code: %d", ret);
      END_RECORD_STATS(EVENT_TYPE_COMMIT)
      return convert_ctc_error_code_to_mysql(ret);
    }
    if (is_ddl_commit && !engine_skip_ddl(thd)) {
      ctc_ddl_broadcast_request broadcast_req {{0}, {0}, {0}, {0}, 0, 0, 0, 0, {0}};
      string sql = string(thd->query().str).substr(0, thd->query().length);
      FILL_BROADCAST_BASE_REQ(broadcast_req, sql.c_str(), thd->m_main_security_ctx.priv_user().str,
        thd->m_main_security_ctx.priv_host().str, ctc_instance_id, thd->lex->sql_command);
      if (thd->db().str != NULL && thd->db().length > 0) {
        strncpy(broadcast_req.db_name, thd->db().str, SMALL_RECORD_SIZE - 1);
      }
      broadcast_req.options &= (~CTC_NOT_NEED_CANTIAN_EXECUTE);
      ret = (ct_errno_t)ctc_execute_mysql_ddl_sql(&tch, &broadcast_req, false);
      DBUG_EXECUTE_IF("core_after_ddl_cantian_commit_broadcast", { assert(0); });
      ctc_log_system("[CTC_BROARDCAST_ATOMIC_DDL]:ret:%d, query:%s, user_name:%s, err_code:%d, broadcast_inst_id:%u, "
        "conn_id:%u, ctc_inst_id:%u", ret, broadcast_req.sql_str, broadcast_req.user_name,
        broadcast_req.err_code, broadcast_req.mysql_inst_id, tch.thd_id, tch.inst_id);
      assert (ret == CT_SUCCESS);
    }
    sess_ctx->is_ctc_trx_begin = 0;
  } else {
    ctc_free_cursors_no_autocommit(thd, &tch, sess_ctx);
  }

  if (!commit_trx) {
    sess_ctx->sql_stat_start = 1;  // indicate cantian for a new sql border
    tch.sql_stat_start = 1;
  }
  END_RECORD_STATS(EVENT_TYPE_COMMIT)
  return 0;
}

/**
  Rollback a transaction in an ctc database or marks an SQL statement ended.
  @param: hton in, ctc handlerton
  @param: thd in, handle to the MySQL thread of the user whose transaction
  should be rolled back be committed
  @param: commit_trx in, TRUE - rollback entire transaction FALSE - rollback the
  current statement only statement ended
  @return 0 or deadlock error if the transaction was aborted by another
         higher priority transaction.
  @note:
*/
static int ctc_rollback(handlerton *hton, THD *thd, bool rollback_trx) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  if (thd->lex->sql_command == SQLCOM_DROP_TABLE) {
    ctc_log_error("[CTC_TRX]:rollback when drop table, rollback_trx=%d", rollback_trx);
  }

  bool will_rollback = rollback_trx || !thd->in_multi_stmt_transaction_mode();
  ct_errno_t ret = CT_SUCCESS;
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);
  assert(sess_ctx != nullptr);

  if (will_rollback) {
    int32_t total_csize = sess_ctx->cursors_map->size();
    if (sess_ctx->invalid_cursors != nullptr) {
      total_csize += sess_ctx->invalid_cursors->size();
    }
    uint64_t *cursors = (uint64_t *)ctc_alloc_buf(&tch, sizeof(uint64_t) * total_csize);
    if ((total_csize != 0) && (cursors == nullptr)) {
      ctc_log_error("ctc_alloc_buf for cursors in ctc_rollback failed");
    }
    assert((total_csize == 0) ^ (cursors != nullptr));
    ctc_copy_cursors_to_free(sess_ctx, cursors, 0);
    ret = (ct_errno_t)ctc_trx_rollback(&tch, cursors, total_csize);

    if (ret != CT_SUCCESS) {
      ctc_free_buf(&tch, (uint8_t *)cursors);
      ctc_log_error("rollback trx failed with error code: %d", ret);
      END_RECORD_STATS(EVENT_TYPE_ROLLBACK)
      return convert_ctc_error_code_to_mysql(ret);
    }
    ctc_free_buf(&tch, (uint8_t *)cursors);
    sess_ctx->is_ctc_trx_begin = 0;
  } else if (sess_ctx->sql_stat_start == 0) {
    int32_t total_csize = sess_ctx->cursors_map->size();
    if (sess_ctx->invalid_cursors != nullptr) {
      total_csize += sess_ctx->invalid_cursors->size();
    }
    uint64_t *cursors = (uint64_t *)ctc_alloc_buf(&tch, sizeof(uint64_t) * total_csize);
    if ((total_csize != 0) && (cursors == nullptr)) {
      ctc_log_error("ctc_alloc_buf for cursors in ctc_rollback failed");
    }
    assert((total_csize == 0) ^ (cursors != nullptr));
    ctc_copy_cursors_to_free(sess_ctx, cursors, 0);
    (void)ctc_srv_rollback_savepoint(&tch, cursors, total_csize, CTC_SQL_START_INTERNAL_SAVEPOINT);
    ctc_free_buf(&tch, (uint8_t *)cursors);
  } else {
    ctc_free_cursors_no_autocommit(thd, &tch, sess_ctx);
  }

  if (!rollback_trx) {
    sess_ctx->sql_stat_start = 1;  // indicate cantian for a new sql border
    tch.sql_stat_start = 1;
  }
  END_RECORD_STATS(EVENT_TYPE_ROLLBACK)
  return 0;
}

static int ctc_close_connect(handlerton *hton, THD *thd) {
  BEGIN_RECORD_STATS
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);

  if (thd->is_attachable_transaction_active() || is_initialize() || is_ctc_mdl_thd(thd)) {
    tch.is_broadcast = false;
  } else {
    tch.is_broadcast = true;
  }

  ctc_handler_t local_tch;
  memset(&local_tch, 0, sizeof(local_tch));
  local_tch.inst_id = tch.inst_id;
  local_tch.sess_addr = tch.sess_addr;
  local_tch.thd_id = tch.thd_id;
  local_tch.is_broadcast = tch.is_broadcast;

  int ret = ctc_close_session(&local_tch);
  release_sess_ctx(sess_ctx, hton, thd);
  END_RECORD_STATS(EVENT_TYPE_CLOSE_CONNECTION)
  return convert_ctc_error_code_to_mysql((ct_errno_t)ret);
}

static void ctc_kill_connection(handlerton *hton, THD *thd) {
  BEGIN_RECORD_STATS
  ctc_handler_t tch;
  int ret = get_tch_in_handler_data(hton, thd, tch);
  if (ret != CT_SUCCESS) {
    return;
  }
  if (tch.sess_addr == INVALID_VALUE64) {
    ctc_log_system("[CTC_KILL_SESSION]:trying to kill a thd without session assigned, conn_id=%u, instid=%u",
      tch.thd_id, tch.inst_id);
    END_RECORD_STATS(EVENT_TYPE_KILL_CONNECTION)
    return;
  }

  if (is_ddl_sql_cmd(thd->lex->sql_command)) {
    END_RECORD_STATS(EVENT_TYPE_KILL_CONNECTION)
    return;
  }

  ctc_handler_t local_tch;
  memset(&local_tch, 0, sizeof(local_tch));
  local_tch.inst_id = tch.inst_id;
  local_tch.sess_addr = tch.sess_addr;
  local_tch.thd_id = tch.thd_id;

  ctc_kill_session(&local_tch);
  ctc_log_system("[CTC_KILL_SESSION]:conn_id:%u, ctc_instance_id:%u", tch.thd_id, tch.inst_id);
  END_RECORD_STATS(EVENT_TYPE_KILL_CONNECTION)
}

static int ctc_pre_create_db4cantian(THD *thd, ctc_handler_t *tch) {
  BEGIN_RECORD_STATS
  if (engine_skip_ddl(thd)) {
    END_RECORD_STATS(EVENT_TYPE_PRE_CREATE_DB)
    return CT_SUCCESS;
  }
  char user_name[SMALL_RECORD_SIZE] = { 0 };
  ctc_copy_name(user_name, thd->lex->name.str, SMALL_RECORD_SIZE);
  int error_code = 0;
  char error_message[ERROR_MESSAGE_LEN] = {0};

  ctc_log_system("[CTC_INIT]:ctc_pre_create_db4cantian begin");
  DBUG_EXECUTE_IF("core_before_create_tablespace_and_db", { assert(0); });  // 有锁的问题
  string sql = string(thd->query().str).substr(0, thd->query().length);

  ctc_db_infos_t db_infos;
  db_infos.name = user_name;
  db_infos.datafile_size = ctc_db_datafile_size;
  db_infos.datafile_autoextend = ctc_db_datafile_autoextend;
  db_infos.datafile_extend_size = ctc_db_datafile_extend_size;
  int ret = ctc_pre_create_db(tch, sql.c_str(), &db_infos, &error_code, error_message);

  DBUG_EXECUTE_IF("core_after_create_tablespace_and_db", { assert(0); });  // 元数据不一致的问题

  ctc_log_system("[CTC_PRE_CREATE_DB]:ret:%d, database:%s, error_code:%d, error_message:%s, conn_id:%u, ctc_instance_id:%u",
    ret, thd->lex->name.str, error_code, error_message, tch->thd_id, tch->inst_id);

  if (ret != CT_SUCCESS) {
    /* 如果参天上报tablespace或user已存在，且创库命令包含if not exists关键字，则忽略此错误 */
    if (error_code == ERR_USER_NOT_EMPTY_4MYSQL) {
        if (thd->lex->create_info->options & HA_LEX_CREATE_IF_NOT_EXISTS) {
            END_RECORD_STATS(EVENT_TYPE_PRE_CREATE_DB)
            return CT_SUCCESS;
        }
        my_printf_error(ER_DB_CREATE_EXISTS, "Can't create database '%s'; database exists", MYF(0), thd->lex->name.str);
        END_RECORD_STATS(EVENT_TYPE_PRE_CREATE_DB)
        return ER_DB_CREATE_EXISTS;
    }

    if (error_code != 0) {
        my_error(ER_CANT_CREATE_DB, MYF(0), thd->lex->name.str, error_code, error_message);
    }
  }
  ctc_log_system("[CTC_INIT]:ctc_pre_create_db4cantian end, ret=%d", ret);
  END_RECORD_STATS(EVENT_TYPE_PRE_CREATE_DB)
  return ret;
}

static void ctc_lock_table_handle_error(int err_code, ctc_lock_table_info *lock_info, ctc_handler_t &tch, THD *thd) {
  DBUG_EXECUTE_IF("ctc_lock_table_fail_DDL_LOCKED", { err_code = ERR_USER_DDL_LOCKED; });
  DBUG_EXECUTE_IF("ctc_lock_table_fail_VERSION_NOT_MATCH", { err_code = CTC_DDL_VERSION_NOT_MATCH; });
  DBUG_EXECUTE_IF("ctc_lock_table_fail_DISALLOW_OPERATION", { err_code = ER_DISALLOWED_OPERATION; });

  switch (err_code) {
    case ERR_USER_DDL_LOCKED:
      my_printf_error(ER_DISALLOWED_OPERATION, "Instance has been locked, disallow this operation", MYF(0));
      ctc_log_system("[CTC_MDL_LOCK]: Instance has been locked, disallow this operation,"
                     "lock_info=(%s, %s), sql=%s, conn_id=%u, ctc_instance_id=%u",
                     lock_info->db_name, lock_info->table_name, thd->query().str, tch.thd_id, tch.inst_id);
      break;

    case CTC_DDL_VERSION_NOT_MATCH:
      my_printf_error(ER_DISALLOWED_OPERATION, "Version not match. Please make sure cluster on the same version.", MYF(0));
      ctc_log_system("[CTC_MDL_LOCK]: Version not match,lock_info=(%s, %s), sql=%s", lock_info->db_name, lock_info->table_name, thd->query().str);
      break;

    default:
      break;
  }

  return;
}

static int ctc_notify_pre_event(THD *thd, handlerton *ctc_hton, ctc_handler_t &tch, ctc_lock_table_info *lock_info) {
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(ctc_hton, thd);
  assert(sess_ctx != nullptr);
  if (sess_ctx == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  int ret = 0;
  int err_code = 0;
  const char *cur_db_name = CTC_GET_THD_DB_NAME(thd);
  enum_sql_command sql_command = thd->lex->sql_command;
  if (sql_command == SQLCOM_CREATE_DB || sql_command == SQLCOM_DROP_DB || sql_command == SQLCOM_ALTER_DB) {
    cur_db_name = nullptr;
  }

  if (is_work_flow()) {
    ret = ctc_lock_table(&tch, cur_db_name, lock_info, &err_code);
 
    DBUG_EXECUTE_IF("ctc_lock_table_fail", { ret = -1; });
    if (ret != 0) {
      ctc_lock_table_handle_error(err_code, lock_info, tch, thd);
      return ret;
    }
    ctc_log_system("[CTC_MDL_LOCK]: current node get another node lock success, err=%d, lock_info=(%s, %s), sql=%s, conn_id=%u, ctc_instance_id=%u",
                    err_code, lock_info->db_name, lock_info->table_name, thd->query().str, tch.thd_id, tch.inst_id);
  }

  switch (sql_command) {
      case SQLCOM_CREATE_DB:{
          ret = ctc_pre_create_db4cantian(thd, &tch);
          break;
      }
      case SQLCOM_DROP_DB:{
          char err_msg[ERROR_MESSAGE_LEN] = {0};
          ret = ctc_drop_db_pre_check(&tch, lock_info->db_name, &err_code, err_msg);
          if (ret != 0) {
            my_printf_error(ER_DISALLOWED_OPERATION, "Can't drop database '%s' (errno: %d - %s)", MYF(0),
                            lock_info->db_name, err_code, err_msg);
          }
          break;
      }
      default:
          break;
  }
  return ret;
}

static int ctc_notify_post_event(THD *thd, handlerton *ctc_hton, ctc_handler_t &tch, ctc_lock_table_info *lock_info) {
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(ctc_hton, thd);
  assert(sess_ctx != nullptr);
  if (sess_ctx == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }

  DBUG_EXECUTE_IF("core_before_ctc_unlock_table", { assert(0); });  // 解锁前core

  int ret = 0;
  if (is_work_flow()) {
    if ((MDL_key::enum_mdl_namespace)lock_info->mdl_namespace == MDL_key::ACL_CACHE) {
      invalidate_remote_dcl_cache(&tch);
      update_sess_ctx_by_tch(tch, ctc_hton, thd);
      ctc_log_system("[CTC_ACL_CACHE]:invalidate dcl cache.");

      if (thd->is_error()) {
        ctc_log_system("CTC_POST_EVENT]: no need to record sql str since thd has an error.");
        return ret;
      }
      if (is_dcl_sql_cmd(thd->lex->sql_command) && ctc_record_sql(thd, true)) {
        ctc_log_error("[CTC_POST_EVENT]:record dcl sql str failed. sql:%s", thd->query().str);
      }

      return ret;
    }
 
    ctc_log_system("[UNLOCK_TABLE]: ctc_unlock_table lock_info=(%s, %s), sql=%s", lock_info->db_name, lock_info->table_name, thd->query().str);
    ret = ctc_unlock_table(&tch, ctc_instance_id, lock_info);

    if (ret != 0) {
      ctc_log_error("[CTC_MDL_LOCK]: unlock failed, ret: %d, sql: %s, conn_id: %u, ctc_instance_id: %u",
                    ret, thd->query().str, tch.thd_id, tch.inst_id);
    }
  }
  return ret;
}

/**
  Notify/get permission from interested storage engines before acquiring
  exclusive lock for the key.

  The returned argument 'victimized' specify reason for lock
  not granted. If 'true', lock was refused in an attempt to
  resolve a possible MDL->GSL deadlock. Locking may then be retried.

  @return False if notification was successful and it is OK to acquire lock,
          True if one of SEs asks to abort lock acquisition.
*/
static bool ctc_notify_exclusive_mdl(THD *thd, const MDL_key *mdl_key,
                                     ha_notification_type notification_type,
                                     bool *victimized MY_ATTRIBUTE((unused))) {
  if (is_ctc_mdl_thd(thd) || (notification_type == HA_NOTIFY_PRE_EVENT &&
      mdl_key->mdl_namespace() == MDL_key::ACL_CACHE)) {
    return false;
  }
  /*
    we can not check sql length while using prepare statement,
    so we need to check the sql length before ddl sql again
  */
  size_t query_len = thd->query().length;
  if (!IS_METADATA_NORMALIZATION() && query_len > MAX_DDL_SQL_LEN_CONTEXT) {
    string err_msg = "`" + string(thd->query().str).substr(0, 100) + "...` Is Large Than " + to_string(MAX_DDL_SQL_LEN_CONTEXT);
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), err_msg.c_str());
    return true;
  }

  if (engine_ddl_passthru(thd)) {
    return false;
  }
  
  if (!IS_METADATA_NORMALIZATION()) {
    if (engine_skip_ddl(thd)) {
      ctc_log_warning("[CTC_NOMETA_SQL]:record sql str only generate metadata. sql:%s", thd->query().str);
      return false;
    }

    if (!ddl_enabled_normal(thd)) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "DDL not allowed in this mode, Please check the value of @@ctc_concurrent_ddl.");
      return true;
    }

    if (thd->lex->query_tables == nullptr && mdl_key->mdl_namespace() != MDL_key::SCHEMA) {
      return false;
    }
 
    if (mysql_system_db.find(mdl_key->db_name()) != mysql_system_db.end()) {
      return false;
    }
  }

  int ret = 0;
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(ctc_hton, thd, tch));

  ctc_lock_table_info lock_info = {{0}, {0}, {0}, {0}, thd->lex->sql_command, (int32_t)mdl_key->mdl_namespace()};
  FILL_USER_INFO_WITH_THD(lock_info, thd);
  strncpy(lock_info.db_name, mdl_key->db_name(), SMALL_RECORD_SIZE - 1);
  strncpy(lock_info.table_name, mdl_key->name(), SMALL_RECORD_SIZE - 1);

  if (notification_type == HA_NOTIFY_PRE_EVENT) {
    ret = ctc_notify_pre_event(thd, ctc_hton, tch, &lock_info);
  } else {
    ret = ctc_notify_post_event(thd, ctc_hton, tch, &lock_info);
  }
  update_sess_ctx_by_tch(tch, ctc_hton, thd);
  
  if (ret != 0) {
    ctc_unlock_table(&tch, ctc_instance_id, &lock_info);
    return true;
  }
  
  return false;
}

static bool ctc_notify_alter_table(THD *thd, const MDL_key *mdl_key,
                                   ha_notification_type notification_type) {
  vector<MDL_ticket*> ticket_list;
  if (IS_METADATA_NORMALIZATION() && notification_type == HA_NOTIFY_PRE_EVENT) {
    int pre_lock_ret = ctc_lock_table_pre(thd, ticket_list, MDL_SHARED_UPGRADABLE);
    if (pre_lock_ret != 0) {
      ctc_lock_table_post(thd, ticket_list);
      my_printf_error(ER_LOCK_WAIT_TIMEOUT, "[ctc_notify_alter_table]: LOCK TABLE FAILED", MYF(0));
      return true;
    }
  }

  bool ret = ctc_notify_exclusive_mdl(thd, mdl_key, notification_type, nullptr);
  if (IS_METADATA_NORMALIZATION() && notification_type == HA_NOTIFY_PRE_EVENT) {
    ctc_lock_table_post(thd, ticket_list);
  }

  return ret;
}

static const unsigned int MAX_SAVEPOINT_NAME_LEN = 64;
static const int BASE36 = 36;  // 0~9 and a~z, total 36 encoded character

/**
 Sets a transaction savepoint.
 @param: hton in, ctc handlerton
 @param: thd in, handle to the MySQL thread
 @param: savepoint in, savepoint data
 @return: 0 if succeeds
*/
static int ctc_set_savepoint(handlerton *hton, THD *thd, void *savepoint) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  char name[MAX_SAVEPOINT_NAME_LEN];
  longlong2str((unsigned long long)savepoint, name, BASE36);
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  if (!strcmp(name, CTC_SQL_START_INTERNAL_SAVEPOINT)) {
      my_error(ER_DISALLOWED_OPERATION, MYF(0), "this savepoint has been used by sys db!");
      return ER_DISALLOWED_OPERATION;
  }
  ct_errno_t ret = (ct_errno_t)ctc_srv_set_savepoint(&tch, name);
  if (ret != CT_SUCCESS) {
    ctc_log_error("set trx savepoint failed with error code: %d", ret);
  }
  END_RECORD_STATS(EVENT_TYPE_SET_SAVEPOINT)
  return convert_ctc_error_code_to_mysql(ret);
}

/**
 Rollback to a transaction savepoint.
 @param: hton in, ctc handlerton
 @param: thd in, handle to the MySQL thread
 @param: savepoint in, savepoint data
 @return: 0 if succeeds
*/
static int ctc_rollback_savepoint(handlerton *hton, THD *thd, void *savepoint) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  char name[MAX_SAVEPOINT_NAME_LEN];
  longlong2str((unsigned long long)savepoint, name, BASE36);
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  thd_sess_ctx_s *sess_ctx = (thd_sess_ctx_s *)thd_get_ha_data(thd, hton);
  assert(sess_ctx != nullptr);
  int32_t total_csize = sess_ctx->cursors_map->size();
  if (sess_ctx->invalid_cursors != nullptr) {
    total_csize += sess_ctx->invalid_cursors->size();
  }
  uint64_t *cursors = (uint64_t *)ctc_alloc_buf(&tch, sizeof(uint64_t) * total_csize);
  if ((total_csize != 0) && (cursors == nullptr)) {
    ctc_log_error("ctc_alloc_buf for cursors in ctc_rollback_savepoint failed");
  }
  assert((total_csize == 0) ^ (cursors != nullptr));
  ctc_copy_cursors_to_free(sess_ctx, cursors, 0);
  ct_errno_t ret = (ct_errno_t)ctc_srv_rollback_savepoint(&tch, cursors, total_csize, name);
  ctc_free_buf(&tch, (uint8_t *)cursors);
  if (ret != CT_SUCCESS) {
    ctc_log_error("rollback to trx savepoint failed with error code: %d", ret);
  }
  END_RECORD_STATS(EVENT_TYPE_ROLLBACK_SAVEPOINT)
  return convert_ctc_error_code_to_mysql(ret);
}

/**
 Release a transaction savepoint.
 @param: hton in, ctc handlerton
 @param: thd in, handle to the MySQL thread
 @param: savepoint in, savepoint data
 @return: 0 if succeeds
*/
static int ctc_release_savepoint(handlerton *hton, THD *thd, void *savepoint) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  /**
   * SQLCOM_SAVEPOINT命令如果发现之前已保存有重名的savepoint，mysql会触发调用ctc_release_savepoint，
   * 该种场景下就不需要再调用ctc_srv_release_savepoint了，knl_set_savepoint接口内部会去掉重名的savepoint；
   *
   * ctc_srv_release_savepoint底层调到knl_release_savepoint，会把当前savepoint及其之后的全都release掉，
   * 与innodb行为不一致，在某些场景下会引发缺陷
   */
  if (thd->query_plan.get_command() == SQLCOM_SAVEPOINT) {
    END_RECORD_STATS(EVENT_TYPE_RELEASE_SAVEPOINT)
    return 0;
  }

  char name[MAX_SAVEPOINT_NAME_LEN];
  longlong2str((unsigned long long)savepoint, name, BASE36);
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
  ct_errno_t ret = (ct_errno_t)ctc_srv_release_savepoint(&tch, name);
  if (ret != CT_SUCCESS) {
    ctc_log_error("release trx savepoint failed with error code: %d", ret);
  }
  END_RECORD_STATS(EVENT_TYPE_RELEASE_SAVEPOINT)
  return convert_ctc_error_code_to_mysql(ret);
}

static int ctc_set_var_meta(MYSQL_THD thd, list<set_var_info> variables_info, ctc_set_opt_request *set_opt_request) {
  ctc_handler_t tch;
  tch.inst_id = ctc_instance_id;
  handlerton* hton = get_ctc_hton();

  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));

  set_opt_request->mysql_inst_id = ctc_instance_id;
  set_opt_request->opt_num = variables_info.size();
  set_opt_request->err_code = 0;
  memset(set_opt_request->err_msg, 0, ERROR_MESSAGE_LEN);
  set_opt_request->set_opt_info = (set_opt_info_t *)my_malloc(PSI_NOT_INSTRUMENTED,
                                                              variables_info.size() * sizeof(set_opt_info_t),
                                                              MYF(MY_WME));
  if (set_opt_request->set_opt_info == nullptr) {
    ctc_log_error("alloc mem failed, set_opt_info size(%lu)", variables_info.size() * sizeof(set_opt_info_t));
    return HA_ERR_OUT_OF_MEM;
  }
  memset(set_opt_request->set_opt_info, 0, variables_info.size() * sizeof(set_opt_info_t));
  set_opt_info_t *set_opt_info_begin = set_opt_request->set_opt_info;
  for (auto it = variables_info.begin(); it != variables_info.end(); ++it) {
    auto var_info = *it;
    strncpy(set_opt_info_begin->var_name, var_info.var_name, SMALL_RECORD_SIZE - 1);
    strncpy(set_opt_info_begin->var_value, var_info.var_value, MAX_DDL_SQL_LEN - 1);
    set_opt_info_begin->options |= CTC_NOT_NEED_CANTIAN_EXECUTE;
    set_opt_info_begin->options |= (thd->lex->contains_plaintext_password ?
                                   CTC_CURRENT_SQL_CONTAIN_PLAINTEXT_PASSWORD : 0);
    set_opt_info_begin->options |= var_info.options;
    if (var_info.var_is_int) {
      // actual value of the variable type int
      set_opt_info_begin->var_is_int = true;
    }
    memset(set_opt_info_begin->base_name, 0, SMALL_RECORD_SIZE);
    strncpy(set_opt_info_begin->base_name, var_info.base_name, SMALL_RECORD_SIZE - 1);
    set_opt_info_begin += 1;
  }
  int ret = ctc_execute_set_opt(&tch, set_opt_request, true);
  update_sess_ctx_by_tch(tch, hton, thd);
  my_free(set_opt_request->set_opt_info);
  set_opt_request->set_opt_info = nullptr;
  return ret;
}

static void ctc_set_var_info(set_var_info *var_info, const char *base_name, string name,
                             string value, uint32_t options, bool var_is_int) {
  if (base_name != nullptr) {
    strncpy(var_info->base_name, base_name, SMALL_RECORD_SIZE - 1);
  }
  strncpy(var_info->var_name, name.c_str(), SMALL_RECORD_SIZE - 1);
  strncpy(var_info->var_value, value.c_str(), MAX_DDL_SQL_LEN - 1);
  var_info->options = options;
  var_info->var_is_int = var_is_int;
}

static uint32_t ctc_set_var_option(bool is_null_value, bool is_set_default_value,
                                   enum_var_type type) {
  uint32_t options = 0;
  if (is_null_value) {
    options |= CTC_SET_VARIABLE_TO_NULL;
  }
  if (is_set_default_value) {
    options |= CTC_SET_VARIABLE_TO_DEFAULT;
  }
  if (type == OPT_PERSIST_ONLY) {
    options |= CTC_SET_VARIABLE_PERSIST_ONLY;
  }
  if (type == OPT_PERSIST) {
    options |= CTC_SET_VARIABLE_PERSIST;
  }
  return options;
}

static int ctc_get_sysvar_value_string(set_var *var, string &name_str, string &val_str,
                                       bool &is_null_value, bool &is_default_value) {
  if (!var->value) {
    is_default_value = true;
    val_str = "";
  } else {
    String str;
    String* new_str = var->value->val_str(&str);
    if (!new_str) {
      is_null_value = true;
      val_str = "null";
    } else {
      val_str = new_str->c_ptr();
    }
  }
  if (strlen(val_str.c_str()) > MAX_DDL_SQL_LEN) {
    my_printf_error(ER_DISALLOWED_OPERATION, "Set the variable '%s' failed: value is too long",
                    MYF(0), name_str.c_str());
    ctc_log_error("Set the variable '%s' failed: value is too long", name_str.c_str());
    return -1;
  }
  return 0;
}

static int ctc_emplace_sysvars(set_var_base *var, THD *thd, list<set_var_info> &variables_info) {
  UNUSED_PARAM(thd);
  int ret = 0;
  set_var *setvar = dynamic_cast<set_var *>(var);
  set_var_info var_info;
  memset(&var_info, 0, sizeof(var_info));
  string name_str;
  string val_str;
  bool is_null_value = false;
  bool is_set_default_value = false;
  bool need_forward = false;
  if (setvar) {
#ifdef FEATURE_X_FOR_MYSQL_26
    if (setvar->var) {
      need_forward = !setvar->var->is_readonly() && setvar->is_global_persist() &&
                     setvar->var->check_scope(OPT_GLOBAL);
    }
    const char *base_name = setvar->base.str;
    name_str = setvar->var->name.str;
#elif defined(FEATURE_X_FOR_MYSQL_32)
    std::function<bool(const System_variable_tracker &, sys_var *)> f = [&thd, &need_forward, setvar]
    (const System_variable_tracker &, sys_var *system_var) {
      if (system_var) {
        need_forward = !system_var->is_readonly() && setvar->is_global_persist();
      }
      return true;
    };
    const char *base_name = setvar->m_var_tracker.get_var_name();
    name_str = setvar->m_var_tracker.get_var_name();
#endif
    if (IS_METADATA_NORMALIZATION() && need_forward) {
      bool var_is_int = false;
      if (setvar->value && setvar->value->result_type() == INT_RESULT) {
        var_is_int = true;
      }
      if (ctc_get_sysvar_value_string(setvar, name_str, val_str, is_null_value, is_set_default_value)) {
        return -1;
      }
      uint32_t options = ctc_set_var_option(is_null_value, is_set_default_value, setvar->type);
      ctc_set_var_info(&var_info, base_name, name_str, val_str, options, var_is_int);
      variables_info.emplace_back(var_info);
    }
  }
  return ret;
}

/**
  Broadcast system variables when metadata is normalized.
*/
static int ctc_update_sysvars(handlerton *hton, THD *thd) {
  UNUSED_PARAM(hton);
  if (is_ctc_mdl_thd(thd)) {
    return 0;
  }
  List_iterator_fast<set_var_base> var_it(thd->lex->var_list);
  set_var_base *var = nullptr;
  int ret = 0;
  
  var_it.rewind();
  list<set_var_info> variables_info;
  while ((var = var_it++)) {
    if (typeid(*var) == typeid(set_var)) {
      if (ctc_emplace_sysvars(var, thd, variables_info)) {
        return -1;
      }
    }
  }
  if (!variables_info.empty()) {
    if (variables_info.size() > CTC_MAX_SET_VAR_NUM) {
      my_printf_error(ER_DISALLOWED_OPERATION, "There are currently %lu variables that need to be set,"
                      "but only %d variables can be set at a time. ", MYF(0),
                      variables_info.size(), CTC_MAX_SET_VAR_NUM);
      ctc_log_error("There are currently %lu variables that need to be set,"
                    "but only %d variables can be set at a time. ", variables_info.size(), CTC_MAX_SET_VAR_NUM);
      return -1;
    }
    ctc_set_opt_request set_opt_request;
    ret = ctc_set_var_meta(thd, variables_info, &set_opt_request);
    if (ret != 0 && set_opt_request.err_code != 0) {
      string err_msg = set_opt_request.err_msg;
      my_printf_error(set_opt_request.err_code, "%s", MYF(0), err_msg.c_str());
      ctc_log_error("Error code %d, %s", set_opt_request.err_code, err_msg.c_str());
      return ret;
    }
  }
  return ret;
}

/**
 fill prefetch buffer by calling batch-read intfs
 @param: buf out, return one record in mysql format in order to reduce memcpy
 times
 @return: 0 if succeeds
*/
int ha_ctc::prefetch_and_fill_record_buffer(uchar *buf, ctc_prefetch_fn prefetch) {
  // update max col id needed by executor
  set_max_col_index_4_reading();
  uint32_t fetched_num;
  assert(m_rec_buf != nullptr);

  // alloc prefetch memory
  if (m_prefetch_buf == nullptr) {
    m_prefetch_buf = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, MAX_RECORD_SIZE, MYF(MY_WME));
  }
  if (m_prefetch_buf == nullptr) {
    ctc_log_error("alloc mem failed, m_prefetch_buf size(%u)", MAX_RECORD_SIZE);
    return HA_ERR_OUT_OF_MEM;
  }

  ct_errno_t ret = (ct_errno_t)prefetch(&m_tch, m_prefetch_buf, m_record_lens,
                                        &fetched_num, m_rowids, m_cantian_rec_len);
  check_error_code_to_mysql(ha_thd(), &ret);
  if (ret != CT_SUCCESS) {
    return convert_ctc_error_code_to_mysql(ret);
  }
  if (fetched_num == 0) {
    return HA_ERR_END_OF_FILE;
  }
  actual_fetched_nums = fetched_num;
  reset_rec_buf(true);

  cur_off_in_prefetch_buf = 0;
  cur_fill_buf_index = 0;
  uint32_t filled_num = fetched_num <= (m_rec_buf->max_records() + 1) ? fetched_num : m_rec_buf->max_records() + 1;
  // convert record to mysql format and save in prefetch buffer
  for (uint32_t i = 0; i < filled_num; i++) {
    uint8_t *tmpRecBuf = (i == 0) ? buf : m_rec_buf->add_record();
    if (max_col_index != INVALID_MAX_UINT32) {
      record_buf_info_t record_buf = {m_prefetch_buf + cur_off_in_prefetch_buf, tmpRecBuf, nullptr};
      index_info_t index = {active_index, max_col_index};
      cnvrt_to_mysql_record(*table, &index, &record_buf, m_tch, nullptr);
    }
    cur_off_in_prefetch_buf += m_record_lens[cur_fill_buf_index++];
  }

  bool is_out_of_range = (cur_off_in_prefetch_buf <= (uint64_t)(MAX_RECORD_SIZE - m_cantian_rec_len)) &&
                          actual_fetched_nums < MAX_PREFETCH_REC_NUM;
  m_rec_buf->set_out_of_range(is_out_of_range);
  
  return 0;
}

void ha_ctc::fill_record_to_rec_buffer() {
  m_rec_buf->clear();
  for (uint32_t i = 0; i < m_rec_buf->max_records() && cur_fill_buf_index < actual_fetched_nums; i++) {
    uint8_t *tmpRecBuf = m_rec_buf->add_record();
    if (max_col_index != INVALID_MAX_UINT32) {
      record_buf_info_t record_buf = {m_prefetch_buf + cur_off_in_prefetch_buf, tmpRecBuf, nullptr};
      index_info_t index = {active_index, max_col_index};
      cnvrt_to_mysql_record(*table, &index, &record_buf, m_tch, nullptr);
    }
    cur_off_in_prefetch_buf += m_record_lens[cur_fill_buf_index++];
  }

  bool is_out_of_range = (cur_off_in_prefetch_buf <= (uint64_t)(MAX_RECORD_SIZE - m_cantian_rec_len)) &&
                          actual_fetched_nums < MAX_PREFETCH_REC_NUM;
  m_rec_buf->set_out_of_range(is_out_of_range);
}

void ha_ctc::reset_rec_buf(bool is_prefetch) {
  cur_pos_in_buf = INVALID_MAX_UINT32;
  if (is_prefetch) {
    cur_pos_in_buf = 0;
  }
  if (!m_rec_buf) {
    return;
  }
  m_rec_buf->reset();
}

void ha_ctc::set_max_col_index_4_reading() {
  max_col_index = table->read_set->n_bits - 1;
  // max_col_index equals to num_of_cols - 1 in table if it's not a read-only
  // scan
  if (!bitmap_is_clear_all(table->write_set)) {
    return;
  }
  if (bitmap_is_clear_all(table->read_set)) {
    max_col_index = INVALID_MAX_UINT32;
    return;
  }
  // find max col index needed by optimizer according to read_set
  // max_col_index is needed by cantian2mysql func to get a early return for
  // fullfilling record_buffer
  if (m_is_covering_index && active_index != MAX_KEY) { // index_only
    auto index_info = (*table).key_info[active_index];
    uint key_fields = index_info.actual_key_parts;
    for (int key_id = key_fields -1; key_id >= 0; key_id--) {
      uint col_id = index_info.key_part[key_id].field->field_index();
      if (bitmap_is_set(table->read_set, col_id)) {
        max_col_index = col_id;
        break;
      }
    }
  } else {
    while (!bitmap_is_set(table->read_set, max_col_index)) {
      max_col_index--;
    }
  }
  
}

bool ha_ctc::pre_check_for_cascade(bool is_update) {
  TABLE_SHARE_FOREIGN_KEY_PARENT_INFO *fk = table->s->foreign_key_parent;
 
  for (uint i = 0; i < table->s->foreign_key_parents; i++) {
    if (is_update) {
      if (dd::Foreign_key::RULE_CASCADE != fk[i].update_rule &&
          dd::Foreign_key::RULE_SET_NULL != fk[i].update_rule) {
        return false;
      }
    } else {
      if (dd::Foreign_key::RULE_CASCADE != fk[i].delete_rule &&
          dd::Foreign_key::RULE_SET_NULL != fk[i].delete_rule) {
        return false;
      }
    }
  }
  return true;
}

ha_ctc::ha_ctc(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg), m_rec_buf(nullptr), m_share(nullptr) {
  ref_length = ROW_ID_LENGTH;
}

ha_ctc::~ha_ctc() {
  if (m_ctc_buf != nullptr) {
    ctc_free_buf(&m_tch, m_ctc_buf);
    m_ctc_buf = nullptr;
  }

  if (m_read_buf != nullptr && !is_single_run_mode()) {
    ctc_free_buf(&m_tch, m_read_buf);
    m_read_buf = nullptr;
  }

  if (m_rec_buf_data != nullptr) {
    my_free(m_rec_buf_data);
    m_rec_buf_data = nullptr;
  }

  if (m_rec_buf != nullptr) {
    delete m_rec_buf;
    m_rec_buf = nullptr;
  }

  if (m_prefetch_buf != nullptr) {
    my_free(m_prefetch_buf);
    m_prefetch_buf = nullptr;
  }

  if (m_cond != nullptr) {
    free_m_cond(m_tch, m_cond);
    m_cond = nullptr;
  }
  free_blob_addrs();
}

int ha_ctc::initialize() {
  THD *thd = ha_thd();
  int ret = get_tch_in_handler_data(ht, thd, m_tch);
  if (ret != 0) {
    ctc_log_error("alloc session context mem error.");
    return ret;
  }
  m_tch.inst_id = ha_ctc_get_inst_id();
  m_tch.cursor_addr = INVALID_VALUE64;
  m_tch.cursor_ref = 0;
  m_tch.cursor_valid = false;
  m_tch.part_id = (uint32_t)0xFFFFFFFF;
  return CT_SUCCESS;
}

bool ctc_is_temporary(const dd::Table *table_def) {
  return table_def ? table_def->is_temporary() : true;
}

/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @see
  handler::ha_open() in handler.cc

  @param        name                  Full path of table name (name of the
  file).
  @param        mode                  Open mode flags.
  @param        test_if_locked        ?
  @param        table_def             dd::Table object describing table
                                      being open. Can be NULL for temporary
                                      tables created by optimizer.

  @retval >0    Error.
  @retval  0    Success.
*/

EXTER_ATTACK int ha_ctc::open(const char *name, int, uint test_if_locked, const dd::Table *table_def) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  assert(table_share == table->s);
  THD *thd = ha_thd();

  if (!(test_if_locked & (HA_OPEN_TMP_TABLE | HA_OPEN_INTERNAL_TABLE))) {
    if (table_share->m_part_info == nullptr) {
      if (!(m_share = get_share<Ctc_share>())) {
        return HA_ERR_OUT_OF_MEM;
      }
    }
    lock_shared_ha_data();
    ct_errno_t ret = (ct_errno_t)initialize_cbo_stats();
    unlock_shared_ha_data();
    if (ret != CT_SUCCESS) {
      if (table_share->m_part_info == nullptr) {
        free_share<Ctc_share>();
      }
      END_RECORD_STATS(EVENT_TYPE_OPEN_TABLE)
      return convert_ctc_error_code_to_mysql(ret);
    }
  }

  if (is_replay_ddl(thd)) {
    END_RECORD_STATS(EVENT_TYPE_OPEN_TABLE)
    return 0;
  }
  // rename table的故障场景下，参天的表不存在，需要忽略open close table的错误
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd, table->s->table_name.str) || is_lock_table(thd))) {
    END_RECORD_STATS(EVENT_TYPE_OPEN_TABLE)
    return 0;
  }

  char user_name[SMALL_RECORD_SIZE] = {0};
  char table_name[SMALL_RECORD_SIZE] = {0};
  bool is_tmp_table = ctc_is_temporary(table_def);
  ctc_split_normalized_name(name, user_name, SMALL_RECORD_SIZE, table_name, SMALL_RECORD_SIZE, &is_tmp_table);
  if (!is_tmp_table) {
    ctc_copy_name(table_name, table->s->table_name.str, SMALL_RECORD_SIZE);
  }

  m_cantian_rec_len = get_cantian_record_length(table);
  assert(m_cantian_rec_len >= 0 && m_cantian_rec_len <= CT_MAX_RECORD_LENGTH);
  m_max_batch_num = MAX_RECORD_SIZE / m_cantian_rec_len;
  m_max_batch_num = m_max_batch_num > UINT_MAX ? UINT_MAX : m_max_batch_num;  // restricted by uint32

  update_member_tch(m_tch, ctc_hton, thd);
  key_used_on_scan = table_share->primary_key;

  // table_def can be null while index_merge using union, we can not get table_name by table_def
  ct_errno_t ret = (ct_errno_t)ctc_open_table(&m_tch, table_name, user_name);
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);

  check_error_code_to_mysql(ha_thd(), &ret);
  if (ret != CT_SUCCESS && !(test_if_locked & (HA_OPEN_TMP_TABLE | HA_OPEN_INTERNAL_TABLE))
      && table_share->m_part_info == nullptr) {
    free_share<Ctc_share>();
  }

  END_RECORD_STATS(EVENT_TYPE_OPEN_TABLE)
  return convert_ctc_error_code_to_mysql(ret);
}

/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/

int ha_ctc::close(void) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();

  if (get_server_state() != SERVER_OPERATING && thd == nullptr) {
    END_RECORD_STATS(EVENT_TYPE_CLOSE_TABLE)
    return 0;
  }

  if (m_share && table_share->m_part_info == nullptr) {
    free_share<Ctc_share>();
  }

  if (is_replay_ddl(thd)) {
    END_RECORD_STATS(EVENT_TYPE_CLOSE_TABLE)
    return 0;
  }
  
  // rename table的故障场景下，参天的表不存在，需要忽略open close table的错误
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd, table->s->table_name.str))) {
    END_RECORD_STATS(EVENT_TYPE_CLOSE_TABLE)
    return 0;
  }

  update_member_tch(m_tch, ctc_hton, thd);
  if (m_tch.ctx_addr == INVALID_VALUE64) {
    ctc_log_warning("[CTC_CLOSE_TABLE]:Close a table that is not open.");
    END_RECORD_STATS(EVENT_TYPE_CLOSE_TABLE)
    return 0;
  }

  ct_errno_t ret = (ct_errno_t)ctc_close_table(&m_tch);
  check_error_code_to_mysql(ha_thd(), &ret);
  m_tch.ctx_addr = INVALID_VALUE64;
  END_RECORD_STATS(EVENT_TYPE_CLOSE_TABLE)
  return convert_ctc_error_code_to_mysql(ret);
}

int ha_ctc::handle_auto_increment(bool &has_explicit_autoinc) {
  THD *thd = ha_thd();
  const Discrete_interval *insert_id_info;
  insert_id_for_cur_row = 0;
  /*
    if has explicit auto_inc value
    1. specify value that is neither null nor zero
    2. specify zero but in NO_AUTO_VALUE_ON_ZERO sql mode
  */
  if (table->next_number_field->val_int() != 0 ||
      (table->autoinc_field_has_explicit_non_null_value &&
       thd->variables.sql_mode & MODE_NO_AUTO_VALUE_ON_ZERO)) {
    if (thd->is_error() &&
        thd->get_stmt_da()->mysql_errno() == ER_TRUNCATED_WRONG_VALUE) {
        return HA_ERR_AUTOINC_ERANGE;
    }
    has_explicit_autoinc = true;
    return CT_SUCCESS;
  }

  /*
   1. Value set by 'SET INSERT_ID=#'
   2. Partition table use auto_inc column as part key, update to 0
  */
  if ((insert_id_info = thd->auto_inc_intervals_forced.get_next()) != nullptr) {
    // store insert_id to auto_inc col, reset insert_id
    ulonglong forced_val = insert_id_info->minimum();
    table->next_number_field->store(forced_val, true);
    thd->auto_inc_intervals_forced.clear();
    has_explicit_autoinc = true;
    insert_id_for_cur_row = forced_val;
    return CT_SUCCESS;
  }
 
  has_explicit_autoinc = false;
  return CT_SUCCESS;
}

/**
  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

  @details
  Dse of this would be:
  @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
  @endcode

  See ha_tina.cc for an ctc of extracting all of the data as strings.
  ha_berekly.cc has an ctc of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments. This case also applies to
  write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

  @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/
#ifdef METADATA_NORMALIZED
EXTER_ATTACK int ha_ctc::write_row(uchar *buf, bool write_through) {
#endif
#ifndef METADATA_NORMALIZED
EXTER_ATTACK int ha_ctc::write_row(uchar *buf) {
#endif
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();

  if (engine_ddl_passthru(thd) && is_create_table_check(thd)) {
    END_RECORD_STATS(EVENT_TYPE_WRITE_ROW)
    return CT_SUCCESS;
  }

  int cantian_record_buf_size = CTC_BUF_LEN;
  uint16_t serial_column_offset = 0;
  int error_result = CT_SUCCESS;
  bool auto_inc_used = false;
  bool has_explicit_autoinc = false;
  ha_statistic_increment(&System_status_var::ha_write_count);

  // if has auto_inc column
  if (table->next_number_field && buf == table->record[0]) {
    error_result = handle_auto_increment(has_explicit_autoinc);
    if (error_result != CT_SUCCESS) {
      END_RECORD_STATS(EVENT_TYPE_WRITE_ROW)
      return error_result;
    }
    auto_inc_used = true;
  }

  if (!m_rec_buf_4_writing) {
    dml_flag_t flag = ctc_get_dml_flag(thd, false, auto_inc_used, has_explicit_autoinc, false);
#ifdef METADATA_NORMALIZED
    flag.write_through = write_through;
#endif
#ifndef METADATA_NORMALIZED
        flag.write_through = false;
#endif
    error_result = convert_mysql_record_and_write_to_cantian(buf, &cantian_record_buf_size, &serial_column_offset, flag);
    END_RECORD_STATS(EVENT_TYPE_WRITE_ROW)
    return error_result;
  }

  // flush records to engine if buffer is full
  if (m_rec_buf_4_writing->records() == m_rec_buf_4_writing->max_records()) {
    error_result = bulk_insert();
    if (error_result != CT_SUCCESS) {
      delete m_rec_buf_4_writing;
      m_rec_buf_4_writing = nullptr;
      END_RECORD_STATS(EVENT_TYPE_WRITE_ROW)
      return error_result;
    }

    m_rec_buf_4_writing->reset();
  }

  uchar *cur_write_pos = m_rec_buf_4_writing->add_record();
  memset(cur_write_pos, 0, sizeof(row_head_t));
  record_buf_info_t record_buf = {cur_write_pos, buf, &cantian_record_buf_size};
  
  update_member_tch(m_tch, ctc_hton, thd, false);
  error_result = mysql_record_to_cantian_record(*table, &record_buf, m_tch, &serial_column_offset);
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);  // update for ctc_knl_write_lob in mysql_record_to_cantian_record

  if (error_result != 0) {
    m_rec_buf_4_writing->reset();
    return error_result;
  }
  assert(cantian_record_buf_size <= m_cantian_rec_len);
  END_RECORD_STATS(EVENT_TYPE_WRITE_ROW)
  return CT_SUCCESS;
}

int ha_ctc::convert_mysql_record_and_write_to_cantian(uchar *buf, int *cantian_record_buf_size,
                                                      uint16_t *serial_column_offset, dml_flag_t flag) {
  int error_result;
  ct_errno_t ret;
  uint64_t cur_last_insert_id = 0;
  if (m_ctc_buf == nullptr) {
    m_ctc_buf = ctc_alloc_buf(&m_tch, BIG_RECORD_SIZE);
    if (m_ctc_buf == nullptr) {
      return convert_ctc_error_code_to_mysql(ERR_ALLOC_MEMORY);
    }
  }
  memset(m_ctc_buf, 0, sizeof(row_head_t));
  record_buf_info_t record_buf = {m_ctc_buf, buf, cantian_record_buf_size};
  
  update_member_tch(m_tch, ctc_hton, ha_thd());
  error_result = mysql_record_to_cantian_record(*table, &record_buf, m_tch, serial_column_offset);
  update_sess_ctx_by_tch(m_tch, ctc_hton, ha_thd());  // update for ctc_knl_write_lob in mysql_record_to_cantian_record

  if (error_result != 0) {
    return error_result;
  }
  update_member_tch(m_tch, ctc_hton, ha_thd());
  record_info_t record_info = {m_ctc_buf, (uint16_t)*cantian_record_buf_size, nullptr, nullptr};
  ret = (ct_errno_t)ctc_write_row(&m_tch, &record_info, *serial_column_offset, &cur_last_insert_id, flag);
  update_sess_ctx_by_tch(m_tch, ctc_hton, ha_thd());
  check_error_code_to_mysql(ha_thd(), &ret);

  if (table->next_number_field && buf == table->record[0] && !flag.has_explicit_autoinc) {
    table->next_number_field->store(cur_last_insert_id, true);
    insert_id_for_cur_row = cur_last_insert_id;
  }

  return convert_ctc_error_code_to_mysql(ret);
}

int ha_ctc::bulk_insert_low(dml_flag_t flag, uint *dup_offset) {
  record_info_t record_info = {m_rec_buf_data, (uint16_t)m_cantian_rec_len, nullptr, nullptr};
  return ctc_bulk_write(&m_tch, &record_info, m_rec_buf_4_writing->records(), dup_offset, flag, nullptr);
}

int ha_ctc::bulk_insert() {
  ct_errno_t ret;
  uint dup_offset = 0;
  THD *thd = ha_thd();
  dml_flag_t flag = ctc_get_dml_flag(thd, false, false, false, false);
  update_member_tch(m_tch, ctc_hton, thd);
  ret = (ct_errno_t)bulk_insert_low(flag, &dup_offset);
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  check_error_code_to_mysql(thd, &ret);
  
  if (ret != CT_SUCCESS) {
    // refresh table->record[0] to make sure that duplicated contents are correctly set in output
    if (ret == ERR_DUPLICATE_KEY) {
      record_buf_info_t record_buf = {m_rec_buf_4_writing->record(dup_offset), table->record[0], nullptr};
      index_info_t index = {UINT_MAX, UINT_MAX};
      cantian_record_to_mysql_record(*table, &index, &record_buf, m_tch, nullptr);
    }
    return convert_ctc_error_code_to_mysql(ret);
  }
  return CT_SUCCESS;
}

void ha_ctc::start_bulk_insert(ha_rows rows) {
  assert(m_rec_buf_4_writing == nullptr);

  THD *thd = ha_thd();
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd) || is_create_table_check(thd))) {
    return;
  }

  if (m_rec_buf_data == nullptr) {
    m_rec_buf_data = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, MAX_RECORD_BUFFER_SIZE_CTC, MYF(MY_WME));
  }

  if (rows == 1 || m_is_insert_dup || m_is_replace || m_ignore_dup || table->s->blob_fields > 0 ||
      table->next_number_field || m_rec_buf_data == nullptr) {
    m_rec_buf_4_writing = nullptr;
    return;
  }
  m_rec_buf_4_writing = new Record_buffer{m_max_batch_num, (size_t)m_cantian_rec_len, m_rec_buf_data};
}

int ha_ctc::end_bulk_insert() {
  if (engine_ddl_passthru(ha_thd()) && is_create_table_check(ha_thd())) {
    return 0;
  }

  if (m_rec_buf_4_writing == nullptr) {
    return 0;
  }

  int ret = 0;
  if (m_rec_buf_4_writing->records() != 0) {
    ret = bulk_insert();
    if (ret != 0) {
      set_my_errno(ret);
    }
  }

  delete m_rec_buf_4_writing;
  m_rec_buf_4_writing = nullptr;
  return ret;
}

static bool check_if_update_primary_key(TABLE *table) {
  if (table->s->primary_key < MAX_KEY) {
    KEY *keyinfo;
    keyinfo = table->s->key_info + table->s->primary_key;
    for (uint i = 0; i < keyinfo->user_defined_key_parts; i++) {
      uint fieldnr = keyinfo->key_part[i].fieldnr - 1;
      if (bitmap_is_set(table->write_set, fieldnr)) {
        return true;
      }
    }
  }
 
  return false;
}

int ctc_cmp_key_values(TABLE *table, const uchar *old_data, const uchar *new_data, uint key_nr) {
  if (key_nr == MAX_KEY) {
    return 0;
  }

  KEY *key_info = table->key_info + key_nr;
  KEY_PART_INFO *key_part = key_info->key_part;
  KEY_PART_INFO *key_part_end = key_part + key_info->user_defined_key_parts;

  for (; key_part != key_part_end; key_part++) {
    Field *field = key_part->field;
    if (key_part->key_part_flag & (HA_BLOB_PART | HA_VAR_LENGTH_PART)) {
      if (field->cmp_binary((old_data + key_part->offset), (new_data + key_part->offset), (ulong)key_part->length)) {
        return 1;
      }
    } else if (memcmp(old_data + key_part->offset, new_data + key_part->offset, key_part->length)) {
      return 1;
    }
  }

  return 0;
}

/**
  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

  @details
  Currently new_data will not have an updated auto_increament record. You can
  do this for ctc by doing:

  @code

  if (table->next_number_field && record == table->record[0])
    update_auto_increment();

  @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

  @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
EXTER_ATTACK int ha_ctc::update_row(const uchar *old_data, uchar *new_data) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  m_is_replace = (thd->lex->sql_command == SQLCOM_REPLACE ||
                  thd->lex->sql_command == SQLCOM_REPLACE_SELECT) ? true : m_is_replace;
  if (thd->lex->sql_command == SQLCOM_REPLACE || thd->lex->sql_command == SQLCOM_REPLACE_SELECT) {
    uint key_nr = table->file->errkey;
    if (key_nr < MAX_KEY && ctc_cmp_key_values(table, old_data, new_data, key_nr) != 0) {
      END_RECORD_STATS(EVENT_TYPE_UPDATE_ROW)
      return HA_ERR_KEY_NOT_FOUND;
    }
  }

  int cantian_new_record_buf_size = CTC_BUF_LEN;
  uint16_t serial_column_offset = 0;
  ha_statistic_increment(&System_status_var::ha_update_count);
  

  vector<uint16_t> upd_fields;
  bool update_primary_key = m_tch.change_data_capture && check_if_update_primary_key(table);
  for (uint16_t i = 0; i < table->write_set->n_bits; i++) {
    if (update_primary_key || bitmap_is_set(table->write_set, i)) {
      upd_fields.push_back(i);
    }
  }

  if (upd_fields.size() == 0) {
    END_RECORD_STATS(EVENT_TYPE_UPDATE_ROW)
    return HA_ERR_RECORD_IS_THE_SAME;
  }

  if (m_ctc_buf == nullptr) {
    m_ctc_buf = ctc_alloc_buf(&m_tch, BIG_RECORD_SIZE);
    if (m_ctc_buf == nullptr) {
      END_RECORD_STATS(EVENT_TYPE_UPDATE_ROW)
      return convert_ctc_error_code_to_mysql(ERR_ALLOC_MEMORY);
    }
  }
  memset(m_ctc_buf, 0, sizeof(row_head_t));

  record_buf_info_t record_buf = {m_ctc_buf, new_data, &cantian_new_record_buf_size};
  ct_errno_t ret = (ct_errno_t)mysql_record_to_cantian_record(*table, &record_buf,
                                             m_tch, &serial_column_offset, &upd_fields);
  if (ret != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_UPDATE_ROW)
    return ret;
  }
  // return if only update gcol
  if (upd_fields.size() == 0) {
    END_RECORD_STATS(EVENT_TYPE_UPDATE_ROW)
    return 0;
  }
  // (m_ignore_dup && m_is_replace) -> special case for load data ... replace into
  bool dup_update = m_is_insert_dup || (m_ignore_dup && m_is_replace);
  dml_flag_t flag = ctc_get_dml_flag(thd, m_is_replace, false, false, dup_update);
  if (!flag.no_foreign_key_check) {
    flag.no_cascade_check = flag.dd_update ? true : pre_check_for_cascade(true);
  }
  ret = (ct_errno_t)ctc_update_row(&m_tch, cantian_new_record_buf_size, m_ctc_buf,
                                   &upd_fields[0], upd_fields.size(), flag);
  check_error_code_to_mysql(thd, &ret);
  END_RECORD_STATS(EVENT_TYPE_UPDATE_ROW)
  return convert_ctc_error_code_to_mysql(ret);
}

/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_ctc::delete_row(const uchar *buf) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  UNUSED_PARAM(buf);
  ha_statistic_increment(&System_status_var::ha_delete_count);
  THD *thd = ha_thd();
  m_is_replace = (thd->lex->sql_command == SQLCOM_REPLACE ||
                  thd->lex->sql_command == SQLCOM_REPLACE_SELECT) ? true : m_is_replace;
  // m_is_insert_dup for on duplicate key update in partiton table when new row is in different partition
  dml_flag_t flag = ctc_get_dml_flag(thd, m_is_replace || m_is_insert_dup, false, false, false);
  if (!flag.no_foreign_key_check) {
    flag.no_cascade_check = flag.dd_update ? true : pre_check_for_cascade(false);
  }
  ct_errno_t ret = (ct_errno_t)ctc_delete_row(&m_tch, table->s->reclength, flag);
  check_error_code_to_mysql(thd, &ret);
  END_RECORD_STATS(EVENT_TYPE_DELETE_ROW)
  return convert_ctc_error_code_to_mysql(ret);
}

bool ha_ctc::is_record_buffer_wanted(ha_rows *const max_rows) const {
  *max_rows = 0;
  return false;
}

void ha_ctc::set_ror_intersect() {
#ifdef FEATURE_X_FOR_MYSQL_32
  if (table->reginfo.qep_tab && table->reginfo.qep_tab->access_path() &&
      table->reginfo.qep_tab->access_path()->type == AccessPath::ROWID_INTERSECTION) {
    m_ror_intersect = true;
  }
#elif defined(FEATURE_X_FOR_MYSQL_26)
  if (table->reginfo.qep_tab && table->reginfo.qep_tab->quick() &&
      table->reginfo.qep_tab->quick()->get_type() == QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT) {
    m_ror_intersect = true;
  }
#endif
}

// @ref set_record_buffer
int ha_ctc::set_prefetch_buffer() {
  if (m_rec_buf) {
    delete m_rec_buf;
    m_rec_buf = nullptr;
  }
  if (!can_prefetch_records()) {
    return CT_SUCCESS;
  }

  // calculate how many rows to fetch
  uint32_t mysql_rec_length = table->s->rec_buff_length;
  // max rows that internal buf can support
  ha_rows max_rows = max_prefetch_num;
  // max rows that record buffer can support
  max_rows = (max_rows > MAX_RECORD_BUFFER_SIZE_CTC / mysql_rec_length) ?
    MAX_RECORD_BUFFER_SIZE_CTC / mysql_rec_length : max_rows;
  // max rows that limited by array in shared mem intf
  max_rows = (max_rows > max_prefetch_num - 1) ? max_prefetch_num - 1 : max_rows;

  // alloc m_rec_buf_data
  if (m_rec_buf_data == nullptr) {
    m_rec_buf_data = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, MAX_RECORD_BUFFER_SIZE_CTC, MYF(MY_WME));
  }
  if (m_rec_buf_data == nullptr) {
    ctc_log_error("alloc mem failed, m_rec_buf_data size(%u)", MAX_RECORD_BUFFER_SIZE_CTC);
    return ERR_ALLOC_MEMORY;
  }

  m_rec_buf = new Record_buffer{max_rows, mysql_rec_length, m_rec_buf_data};
  ctc_log_note("prefetch record %llu", max_rows);
  return CT_SUCCESS;
}

/*
  All table scans call this first.
  The order of a table scan is:

  ha_tina::store_lock
  ha_tina::external_lock
  ha_tina::info
  ha_tina::rnd_init
  ha_tina::extra
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::rnd_next
  ha_tina::extra
  ha_tina::external_lock
  ha_tina::extra
  ENUM HA_EXTRA_RESET   Reset database to after open

  Each call to ::rnd_next() represents a row returned in the can. When no more
  rows can be returned, rnd_next() returns a value of HA_ERR_END_OF_FILE.
  The ::info() call is just for the optimizer.

*/

// @ref row_prebuilt_t::can_prefetch_records()
bool ha_ctc::can_prefetch_records() const {
  // do not prefetch if set ctc_select_prefetch = false
  if (!ctc_select_prefetch) {
    return false;
  }

  // do not prefetch if it's not a read-only scan
  THD *thd = ha_thd();
  if (thd->lex->sql_command != SQLCOM_SELECT) {
    return false;
  }

  // do not prefetch if we have column that may be extremely huge
  if (table->s->blob_fields) {
    return false;
  }

  // do not prefetch, for with recursive & big_tables
  if (thd->lex->all_query_blocks_list && thd->lex->all_query_blocks_list->is_recursive()) {
    return false;
  }

  return true;
}

/**
  @brief
  After malloc outline memory for blob field (in convert_blob_to_mysql),
  we need to push them into m_blob_addrs and free them after usage.

  @details
  Our purpose is to free the memory we malloc for the current buf that mysql pass to ctc,
  so we need to move the field to the current buf we get (either record[0] or record[1], depends on mysql).
  Like with DUP_REPLACE (replace into) / DUP_UPDATE (on duplicate key update) sql,
  write_record (sql_insert.cc) will use table->record[1] for read instead of table->record[0].

  @see
  sql_insert.cc, reference ha_federated::convert_row_to_internal_format in ha_federated.cc.
*/
void ha_ctc::update_blob_addrs(uchar *record) {
  ptrdiff_t old_ptr = (ptrdiff_t)(record - table->record[0]);
  for (uint i = 0; i < table->s->blob_fields; i++) {
    uint field_no = table->s->blob_field[i];
    if (!bitmap_is_set(table->read_set, field_no)) {
      continue;
    }

    if (table->field[field_no]->is_virtual_gcol()) {
      continue;
    }
    
    Field_blob *blob_field = down_cast<Field_blob *>(table->field[field_no]);
    blob_field->move_field_offset(old_ptr);

    if (blob_field->is_null()) {
      blob_field->move_field_offset(-old_ptr);
      continue;
    }
    
    m_blob_addrs.push_back(blob_field->get_blob_data());
    blob_field->move_field_offset(-old_ptr);
  }
}

void ha_ctc::free_blob_addrs() {
  for (auto addr : m_blob_addrs) {
    my_free(addr);
  }
  m_blob_addrs.clear();
}

void free_m_cond(ctc_handler_t m_tch, ctc_conds *cond) {
  if (cond == nullptr) {
    return;
  }

  if (cond->cond_list != nullptr) {
    int size = cond->cond_list->elements;
    ctc_conds *node = cond->cond_list->first;
    ctc_conds *tmpNode = nullptr;
    for (int i = 0; i < size; i++) {
      if (node == nullptr) {
        break;
      }
      tmpNode = node->next;
      free_m_cond(m_tch, node);
      node = tmpNode;
    }
    ctc_free_buf(&m_tch, (uint8_t *)(cond->cond_list));
  }

  if (cond->field_info.field_value != nullptr) {
    ctc_free_buf(&m_tch, (uint8_t *)(cond->field_info.field_value));
    cond->field_info.field_value = nullptr;
  }

  ctc_free_buf(&m_tch, (uint8_t *)cond);
}

int ctc_fill_conds(ctc_handler_t m_tch, const Item *pushed_cond, Field **field,
                   ctc_conds *m_cond, bool no_backslash) {
  memset(m_cond, 0, sizeof(ctc_conds));
  Item *items = const_cast<Item *>(pushed_cond);
  return dfs_fill_conds(m_tch, items, field, m_cond, no_backslash, NULL);
}

/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the ctc in the introduction at the top of this file to see when
  rnd_init() is called.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/

int ha_ctc::rnd_init(bool) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  m_index_sorted = false;
  THD *thd = ha_thd();
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd) || is_create_table_check(thd) ||
      is_alter_table_scan(m_error_if_not_empty))) {
    END_RECORD_STATS(EVENT_TYPE_RND_INIT)
    return 0;
  }

  set_ror_intersect();
  ct_errno_t ret = (ct_errno_t)set_prefetch_buffer();
  if (ret != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_RND_INIT)
    return ret;
  }
  expected_cursor_action_t action = EXP_CURSOR_ACTION_SELECT;
  if (m_select_lock == lock_mode::EXCLUSIVE_LOCK) {
    enum_sql_command sql_command = (enum_sql_command)thd_sql_command(ha_thd());
    if (sql_command == SQLCOM_DELETE) {
      action = EXP_CURSOR_ACTION_DELETE;
    } else if (sql_command != SQLCOM_ALTER_TABLE){ // action can't be set to update when alter operation using copy algorithm
      action = EXP_CURSOR_ACTION_UPDATE;
    }
  }
  update_member_tch(m_tch, ctc_hton, ha_thd());
  m_tch.cursor_valid = false;
  ret = (ct_errno_t)ctc_rnd_init(&m_tch, action, get_select_mode(), m_cond);
  update_sess_ctx_by_tch(m_tch, ctc_hton, ha_thd());

  if (!(table_share->tmp_table != NO_TMP_TABLE && table_share->tmp_table != TRANSACTIONAL_TMP_TABLE)
    || !is_log_table) {
    update_sess_ctx_cursor_by_tch(m_tch, ctc_hton, thd);
  }

  check_error_code_to_mysql(ha_thd(), &ret);
  cnvrt_to_mysql_record = cantian_record_to_mysql_record;
  reset_rec_buf();
  END_RECORD_STATS(EVENT_TYPE_RND_INIT)
  return convert_ctc_error_code_to_mysql(ret);
}

/**
  @brief
  alloc m_read_buf for select.
*/
int ha_ctc::ctc_alloc_ctc_buf_4_read() {
  // no need alloc/copy/free record in single run mode
  if (is_single_run_mode()) {
    m_read_buf = nullptr;
    return CT_SUCCESS;
  }
  
  if (m_read_buf != nullptr) {
    return CT_SUCCESS;
  }

  m_read_buf = ctc_alloc_buf(&m_tch, BIG_RECORD_SIZE);
  if (m_read_buf == nullptr) {
    return convert_ctc_error_code_to_mysql(ERR_ALLOC_MEMORY);
  }
  return CT_SUCCESS;
}

/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
int ha_ctc::rnd_next(uchar *buf) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  if (unlikely(!m_rec_buf || m_rec_buf->records() == 0)) {
    THD *thd = ha_thd();
    if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd) || is_create_table_check(thd) ||
        is_alter_table_scan(m_error_if_not_empty))) {
      END_RECORD_STATS(EVENT_TYPE_RND_NEXT)
      return HA_ERR_END_OF_FILE;
    }
  }

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);

  if (!m_rec_buf || m_rec_buf->max_records() == 0) {
    int ret = CT_SUCCESS;
    ct_errno_t ct_ret = CT_SUCCESS;
    CTC_RETURN_IF_NOT_ZERO(ctc_alloc_ctc_buf_4_read());
    record_info_t record_info = {m_read_buf, 0, nullptr, nullptr};
    ct_ret = (ct_errno_t)ctc_rnd_next(&m_tch, &record_info);
    ret = process_cantian_record(buf, &record_info, ct_ret, HA_ERR_END_OF_FILE);
    END_RECORD_STATS(EVENT_TYPE_RND_NEXT)
    return ret;
  }

  // initial fetch
  if (m_rec_buf->records() != 0) {
    if (cur_pos_in_buf >= actual_fetched_nums - 1) {
      // records in rec buf are not enough
      // reset cur_pos_in_buf
      cur_pos_in_buf = INVALID_MAX_UINT32;
      if (m_rec_buf->is_out_of_range()) {
        set_my_errno(HA_ERR_END_OF_FILE);
        END_RECORD_STATS(EVENT_TYPE_RND_NEXT)
        return HA_ERR_END_OF_FILE;
      }
    } else {
      // directly fetch record from rec buf
      uint8_t *curRecordStart = m_rec_buf->record(cur_pos_in_buf % m_rec_buf->max_records());
      memcpy(buf, curRecordStart, m_rec_buf->record_size());
      cur_pos_in_buf += 1;
      if (cur_pos_in_buf % m_rec_buf->max_records() == 0) {
        fill_record_to_rec_buffer();
      }
      END_RECORD_STATS(EVENT_TYPE_RND_NEXT)
      return 0;
    }
  }

  int mysql_ret = prefetch_and_fill_record_buffer(buf, ctc_rnd_prefetch);
  if (mysql_ret != 0) {
    set_my_errno(mysql_ret);
    END_RECORD_STATS(EVENT_TYPE_RND_NEXT)
    return mysql_ret;
  }
  END_RECORD_STATS(EVENT_TYPE_RND_NEXT)
  return 0;
}

/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
  @code
  my_store_ptr(ref, ref_length, current_position);
  @endcode

  @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

  @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_ctc::position(const uchar *) {
  BEGIN_RECORD_STATS
  if (cur_pos_in_buf == INVALID_MAX_UINT32) {
    ctc_position(&m_tch, ref, ref_length);
    END_RECORD_STATS(EVENT_TYPE_POSITION)
    return;
  }
  assert(cur_pos_in_buf < max_prefetch_num);
  memcpy(ref, &m_rowids[cur_pos_in_buf], ref_length);
  END_RECORD_STATS(EVENT_TYPE_POSITION)
}

/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

  @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and
  sql_update.cc.

  @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
EXTER_ATTACK int ha_ctc::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  ha_statistic_increment(&System_status_var::ha_read_rnd_count);
  int ret = CT_SUCCESS;
  ct_errno_t ct_ret = CT_SUCCESS;
  CTC_RETURN_IF_NOT_ZERO(ctc_alloc_ctc_buf_4_read());
  record_info_t record_info = {m_read_buf, 0, nullptr, nullptr};
  uint key_len = ref_length;
  if (IS_CTC_PART(m_tch.part_id)) {
    key_len -= PARTITION_BYTES_IN_POS;
  }
  ct_ret = (ct_errno_t)ctc_rnd_pos(&m_tch, key_len, pos, &record_info);
  ret = process_cantian_record(buf, &record_info, ct_ret, HA_ERR_KEY_NOT_FOUND);
  END_RECORD_STATS(EVENT_TYPE_RND_POS)
  return ret;
}

/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

  @details
  Currently this table handler doesn't implement most of the fields really
  needed. SHOW also makes use of this data.

  You will probably want to have the following in your code:
  @code
  if (records < 2)
    records = 2;
  @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_show.cc, sql_table.cc, sql_union.cc, and sql_update.cc.

  @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_show.cc, sql_table.cc, sql_union.cc and sql_update.cc
*/

void ha_ctc::info_low() {
  if (m_share && m_share->cbo_stats != nullptr) {
    stats.records = m_share->cbo_stats->ctc_cbo_stats_table->estimate_rows;
  }
}

int ha_ctc::info(uint flag) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd) || is_create_table_check(thd))) {
    END_RECORD_STATS(EVENT_TYPE_GET_CBO)
    return 0;
  }

  ct_errno_t ret = CT_SUCCESS;
  if (flag & HA_STATUS_VARIABLE) {
    if (thd->lex->sql_command == SQLCOM_DELETE &&
        thd->lex->query_block->where_cond() == nullptr) {
      records(&stats.records);
      END_RECORD_STATS(EVENT_TYPE_GET_CBO)
      return 0;
    }
    // analyze..update histogram on colname flag
    if ((flag & HA_STATUS_VARIABLE) && (flag & HA_STATUS_NO_LOCK) &&
      thd->lex->sql_command == SQLCOM_ANALYZE) {
      ret = (ct_errno_t)analyze(thd, nullptr);
      if (ret != CT_SUCCESS) {
        END_RECORD_STATS(EVENT_TYPE_GET_CBO)
        return convert_ctc_error_code_to_mysql(ret);
      }
    }
    
    ret = (ct_errno_t)get_cbo_stats_4share();
    if (ret != CT_SUCCESS) {
      END_RECORD_STATS(EVENT_TYPE_GET_CBO)
      return convert_ctc_error_code_to_mysql(ret);
    }
  }

  info_low();
  if (stats.records < 2) {
    /* This is a lie, but you don't want the optimizer to see zero or 1 */
    stats.records = 3;
  }

  if (flag & HA_STATUS_ERRKEY) {
    char index_name[CTC_MAX_KEY_NAME_LENGTH + 1] = { 0 };
    ret = (ct_errno_t)ctc_get_index_name(&m_tch, index_name);

    if (ret == CT_SUCCESS) {
      for (uint i = 0; i < table->s->keys; i++) {
        if (strncmp(table->key_info[i].name, index_name, strlen(index_name)) == 0) {
          table->file->errkey = i;
          break;
        }
      }
      if (table->file->errkey == UINT_MAX) {
        END_RECORD_STATS(EVENT_TYPE_GET_CBO)
        return HA_ERR_KEY_NOT_FOUND;
      }
    }
  }
  END_RECORD_STATS(EVENT_TYPE_GET_CBO)
  return convert_ctc_error_code_to_mysql(ret);
}

int ha_ctc::analyze(THD *thd, HA_CHECK_OPT *) {
  BEGIN_RECORD_STATS
  if (engine_ddl_passthru(thd)) {
    if (m_share) {
      m_share->need_fetch_cbo = true;
    }
    END_RECORD_STATS(EVENT_TYPE_CBO_ANALYZE)
    return 0;
  }

  if (table->s->tmp_table) {
    END_RECORD_STATS(EVENT_TYPE_CBO_ANALYZE)
    return HA_ADMIN_OK;
  }

  char user_name_str[SMALL_RECORD_SIZE] = { 0 };
  ctc_copy_name(user_name_str, thd->lex->query_tables->get_db_name(), SMALL_RECORD_SIZE);

  ct_errno_t ret = (ct_errno_t)ctc_analyze_table(
    &m_tch, user_name_str, thd->lex->query_tables->table_name, THDVAR(thd, sampling_ratio));
  check_error_code_to_mysql(thd, &ret);
  if (ret == CT_SUCCESS && m_share && table_share->m_part_info == nullptr) {
    m_share->need_fetch_cbo = true;
  }
  END_RECORD_STATS(EVENT_TYPE_CBO_ANALYZE)
  return convert_ctc_error_code_to_mysql(ret);
}

int ha_ctc::optimize(THD *thd, HA_CHECK_OPT *)
{
  if (engine_ddl_passthru(thd)) {
    return 0;
  }
  ctc_ddl_stack_mem stack_mem(0);
  update_member_tch(m_tch, ctc_hton, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, m_tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ct_errno_t ret = (ct_errno_t)fill_rebuild_index_req(table, thd, &ddl_ctrl, &stack_mem);
  if (ret != 0) {
    return convert_ctc_error_code_to_mysql(ret);
  }

  void *ctc_ddl_req_msg_mem = stack_mem.get_buf();
  if (ctc_ddl_req_msg_mem == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  ctc_register_trx(ctc_hton, thd);
  ret = (ct_errno_t)ctc_alter_table(ctc_ddl_req_msg_mem, &ddl_ctrl);
  ctc_ddl_hook_cantian_error("ctc_optimize_table_cantian_error", thd, &ddl_ctrl, &ret);
  m_tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);

  return ctc_ddl_handle_fault("ctc_optimize table", thd, &ddl_ctrl, ret);
}

/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

    @see
  ha_innodb.cc
*/
int ha_ctc::extra(enum ha_extra_function operation) {
  DBUG_TRACE;

  switch (operation) {
    case HA_EXTRA_WRITE_CAN_REPLACE:
      m_is_replace = true;
      break;
    case HA_EXTRA_WRITE_CANNOT_REPLACE:
      m_is_replace = false;
      break;
    case HA_EXTRA_IGNORE_DUP_KEY:
      m_ignore_dup = true;
      break;
    case HA_EXTRA_NO_IGNORE_DUP_KEY:
      m_ignore_dup = false;
      m_is_insert_dup = false;
      break;
    case HA_EXTRA_INSERT_WITH_UPDATE:
      m_is_insert_dup = true;
      break;
    case HA_EXTRA_KEYREAD:
      m_is_covering_index = true;
      break;
    case HA_EXTRA_NO_KEYREAD:
    default:
      m_is_covering_index = false;
      break;
  }
  return 0;
}

int ha_ctc::reset() {
  m_is_replace = false;
  m_is_insert_dup = false;
  m_is_covering_index = false;
  m_ignore_dup = false;
  m_error_if_not_empty = false;
  free_blob_addrs();
  m_pushed_conds = nullptr;
  m_remainder_conds = nullptr;
  if (m_cond != nullptr) {
    free_m_cond(m_tch, m_cond);
    m_cond = nullptr;
  }

  return 0;
}

int ha_ctc::rnd_end() {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  if (engine_ddl_passthru(thd) && (is_alter_table_copy(thd) || is_create_table_check(thd) ||
      is_alter_table_scan(m_error_if_not_empty))) {
    END_RECORD_STATS(EVENT_TYPE_RND_END)
    return 0;
  }

  int ret = CT_SUCCESS;
  if ((table_share->tmp_table != NO_TMP_TABLE && table_share->tmp_table != TRANSACTIONAL_TMP_TABLE) ||
    is_log_table) {
    ct_errno_t ctc_ret = (ct_errno_t)ctc_rnd_end(&m_tch);
    check_error_code_to_mysql(ha_thd(), &ctc_ret);
    ret = convert_ctc_error_code_to_mysql(ctc_ret);
  }

  m_tch.cursor_valid = false;
  m_tch.cursor_addr = INVALID_VALUE64;
  END_RECORD_STATS(EVENT_TYPE_RND_END)
  return ret;
}

int ha_ctc::index_init(uint index, bool sorted) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  set_ror_intersect();
  ct_errno_t ret = (ct_errno_t)set_prefetch_buffer();
  if (ret != CT_SUCCESS) {
    return ret;
  }
  update_member_tch(m_tch, ctc_hton, ha_thd(), false);
  m_index_sorted = sorted;
  active_index = index;
  reset_rec_buf();
  if (m_tch.cursor_ref <= 0) {
    m_tch.cursor_ref = 0;
    m_tch.cursor_addr = INVALID_VALUE64;
  }
  m_tch.cursor_ref++;
  m_tch.cursor_valid = false;
  END_RECORD_STATS(EVENT_TYPE_INDEX_INIT)
  return CT_SUCCESS;
}

int ha_ctc::index_end() {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  active_index = MAX_KEY;
  
  int ret = CT_SUCCESS;
  if (table_share->tmp_table != NO_TMP_TABLE && table_share->tmp_table != TRANSACTIONAL_TMP_TABLE) {
    ret = (ct_errno_t)ctc_index_end(&m_tch);
    assert(ret == CT_SUCCESS);
  }

  m_tch.cursor_ref--;
  if (m_tch.cursor_ref <= 0) {
    m_tch.cursor_valid = false;
  }
  END_RECORD_STATS(EVENT_TYPE_INDEX_END)
  return ret;
}

int ha_ctc::cmp_ref(const uchar *ref1, const uchar *ref2) const {
  DBUG_TRACE;
  return ctc_cmp_cantian_rowid((const rowid_t *)ref1, (const rowid_t *)ref2);
}

int ha_ctc::process_cantian_record(uchar *buf, record_info_t *record_info, ct_errno_t ct_ret, int rc_ret) {
  int ret = CT_SUCCESS;
  check_error_code_to_mysql(ha_thd(), &ct_ret);
  if (ct_ret != CT_SUCCESS) {
    ret = convert_ctc_error_code_to_mysql(ct_ret);
    return ret;
  }

  if (record_info->record_len == 0) {
    set_my_errno(rc_ret);
    ret = rc_ret;
    return ret;
  }

  record_buf_info_t record_buf = {record_info->record, buf, nullptr};
  index_info_t index = {active_index, UINT_MAX};
  cnvrt_to_mysql_record(*table, &index, &record_buf, m_tch, record_info);
  update_blob_addrs(buf);

  return ret;
}

EXTER_ATTACK int ha_ctc::index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  ha_statistic_increment(&System_status_var::ha_read_key_count);

  // reset prefetch buf if calling multiple index_read continuously without index_init as interval
  if (m_rec_buf && m_rec_buf->records() > 0) {
    reset_rec_buf();
  }

  m_is_covering_index = m_ror_intersect ? false : m_is_covering_index;
  m_action = m_is_covering_index ? EXP_CURSOR_ACTION_INDEX_ONLY : EXP_CURSOR_ACTION_SELECT;
  if (m_select_lock == lock_mode::EXCLUSIVE_LOCK) {
    enum_sql_command sql_command = (enum_sql_command)thd_sql_command(ha_thd());
    if (sql_command == SQLCOM_DELETE) {
      m_action = EXP_CURSOR_ACTION_DELETE;
      m_is_covering_index = false;
    } else if (sql_command != SQLCOM_ALTER_TABLE) {
      m_action = EXP_CURSOR_ACTION_UPDATE;
      m_is_covering_index = false;
    }
  }
  cnvrt_to_mysql_record = m_is_covering_index ? cantian_index_record_to_mysql_record : cantian_record_to_mysql_record;

  index_key_info_t index_key_info;
  memset(&index_key_info.key_info, 0, sizeof(index_key_info.key_info));
  index_key_info.find_flag = find_flag;
  index_key_info.action = m_action;
  index_key_info.active_index = active_index;
  index_key_info.sorted = m_index_sorted;
  index_key_info.need_init = !m_tch.cursor_valid;
  int len = strlen(table->key_info[active_index].name);
  memcpy(index_key_info.index_name, table->key_info[active_index].name, len + 1);
  index_key_info.index_skip_scan = false;
#ifdef FEATURE_X_FOR_MYSQL_32
  if (table->reginfo.qep_tab && table->reginfo.qep_tab->range_scan() &&
      table->reginfo.qep_tab->range_scan()->type == AccessPath::INDEX_SKIP_SCAN) {
    index_key_info.index_skip_scan = true;
  }
#elif defined(FEATURE_X_FOR_MYSQL_26)
  if (table->reginfo.qep_tab && table->reginfo.qep_tab->quick_optim() &&
      table->reginfo.qep_tab->quick_optim()->get_type() == QUICK_SELECT_I::QS_TYPE_SKIP_SCAN) {
    index_key_info.index_skip_scan = true;
  }
#endif
  int ret = ctc_fill_index_key_info(table, key, key_len, end_range, &index_key_info, index_key_info.index_skip_scan);
  if (ret != CT_SUCCESS) {
      ctc_log_error("ha_ctc::index_read: fill index key info failed, ret(%d).", ret);
      END_RECORD_STATS(EVENT_TYPE_INDEX_READ)
      return ret;
  }

  bool has_right_key = !index_key_info.index_skip_scan && end_range != nullptr && end_range->length != 0;

  dec4_t d4[MAX_KEY_COLUMNS * 2];
  ret = ctc_convert_index_datatype(table, &index_key_info, has_right_key, d4);
  if (ret != CT_SUCCESS) {
      ctc_log_error("ha_ctc::index_read: convert data type for index search failed, ret(%d).", ret);
      END_RECORD_STATS(EVENT_TYPE_INDEX_READ)
      return ret;
  }

  CTC_RETURN_IF_NOT_ZERO(ctc_alloc_ctc_buf_4_read());
  update_member_tch(m_tch, ctc_hton, ha_thd());
  record_info_t record_info = {m_read_buf, 0, nullptr, nullptr};

  attachable_trx_update_pre_addr(ha_thd(), &m_tch, true);
  ct_errno_t ct_ret = (ct_errno_t)ctc_index_read(&m_tch, &record_info, &index_key_info,
                                                 get_select_mode(), m_cond, m_is_replace || m_is_insert_dup);
  update_sess_ctx_by_tch(m_tch, ctc_hton, ha_thd());
  attachable_trx_update_pre_addr(ha_thd(), &m_tch, false);
  if (index_key_info.need_init) {
    if (!(table_share->tmp_table != NO_TMP_TABLE && table_share->tmp_table != TRANSACTIONAL_TMP_TABLE)) {
      update_sess_ctx_cursor_by_tch(m_tch, ctc_hton, ha_thd());
    }
    index_key_info.need_init = false;
  }

  ret = process_cantian_record(buf, &record_info, ct_ret, HA_ERR_KEY_NOT_FOUND);
  END_RECORD_STATS(EVENT_TYPE_INDEX_READ)
  return ret;
}

EXTER_ATTACK int ha_ctc::index_read_last(uchar *buf, const uchar *key_ptr, uint key_len) {
  return index_read(buf, key_ptr, key_len, HA_READ_PREFIX_LAST);
}

int ha_ctc::index_fetch(uchar *buf) {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  int mysql_ret = 0;

  if (!m_rec_buf || m_rec_buf->max_records() == 0) {
    int ret = CT_SUCCESS;
    ct_errno_t ct_ret = CT_SUCCESS;
    CTC_RETURN_IF_NOT_ZERO(ctc_alloc_ctc_buf_4_read());
    record_info_t record_info = {m_read_buf, 0, nullptr, nullptr};
    attachable_trx_update_pre_addr(ha_thd(), &m_tch, true);
    ct_ret = (ct_errno_t)ctc_general_fetch(&m_tch, &record_info);
    attachable_trx_update_pre_addr(ha_thd(), &m_tch, false);
    ret = process_cantian_record(buf, &record_info, ct_ret, HA_ERR_END_OF_FILE);
    END_RECORD_STATS(EVENT_TYPE_INDEX_FETCH)
    return ret;
  }

  // initial fetch
  if (m_rec_buf->records() != 0) {
    if (cur_pos_in_buf >= actual_fetched_nums - 1) {
      // records in rec buf are not enough
      // reset cur_pos_in_buf
      cur_pos_in_buf = INVALID_MAX_UINT32;
      if (m_rec_buf->is_out_of_range()) {
        set_my_errno(HA_ERR_END_OF_FILE);
        END_RECORD_STATS(EVENT_TYPE_INDEX_FETCH)
        return HA_ERR_END_OF_FILE;
      }
    } else {
      // directly fetch record from rec buf
      uint8_t *curRecordStart = m_rec_buf->record(cur_pos_in_buf % m_rec_buf->max_records());
      memcpy(buf, curRecordStart, m_rec_buf->record_size());
      cur_pos_in_buf += 1;
      if (cur_pos_in_buf % m_rec_buf->max_records() == 0) {
        fill_record_to_rec_buffer();
      }
      END_RECORD_STATS(EVENT_TYPE_INDEX_FETCH)
      return CT_SUCCESS;
    }
  }

  attachable_trx_update_pre_addr(ha_thd(), &m_tch, true);
  mysql_ret = prefetch_and_fill_record_buffer(buf, ctc_general_prefetch);
  attachable_trx_update_pre_addr(ha_thd(), &m_tch, false);

  if (mysql_ret != 0) {
    set_my_errno(mysql_ret);
    END_RECORD_STATS(EVENT_TYPE_INDEX_FETCH)
    return mysql_ret;
  }
  END_RECORD_STATS(EVENT_TYPE_INDEX_FETCH)
  return CT_SUCCESS;
}

int ha_ctc::index_next_same(uchar *buf, const uchar *, uint) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  return index_fetch(buf);
}

/**
  @brief
  Used to read forward through the index.
*/
int ha_ctc::index_next(uchar *buf) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_next_count);
  return index_fetch(buf);
}

/**
  @brief
  Used to read backwards through the index.
*/
int ha_ctc::index_prev(uchar *buf) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_prev_count);
  return index_fetch(buf);
}

/**
  @brief
  index_first() asks for the first key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_ctc::index_first(uchar *buf) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_first_count);
  m_tch.cursor_addr = INVALID_VALUE64;
  m_tch.cursor_valid = false;
  int error = index_read(buf, nullptr, 0, HA_READ_AFTER_KEY);
  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */
  if (error == HA_ERR_KEY_NOT_FOUND) {
    error = HA_ERR_END_OF_FILE;
  }
  return error;
}

/**
  @brief
  index_last() asks for the last key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_ctc::index_last(uchar *buf) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_last_count);
  int error = index_read(buf, nullptr, 0, HA_READ_BEFORE_KEY);
  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */
  if (error == HA_ERR_KEY_NOT_FOUND) {
    error = HA_ERR_END_OF_FILE;
  }
  return error;
}

/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases
  where the optimizer realizes that all rows will be removed as a result of an
  SQL statement.

  @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_select_lex_unit::exec().

  @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_ctc::delete_all_rows() {
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  update_member_tch(m_tch, ctc_hton, thd);
  dml_flag_t flag = ctc_get_dml_flag(thd, false, false, false, false);
  if (!flag.no_foreign_key_check) {
    flag.no_cascade_check = pre_check_for_cascade(false);
  }
  ct_errno_t ret = (ct_errno_t)ctc_delete_all_rows(&m_tch, flag);
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  check_error_code_to_mysql(thd, &ret);
  if (thd->lex->is_ignore() && ret == ERR_ROW_IS_REFERENCED) {
    END_RECORD_STATS(EVENT_TYPE_DELETE_ALL_ROWS)
    return 0;
  }
  END_RECORD_STATS(EVENT_TYPE_DELETE_ALL_ROWS)
  return convert_ctc_error_code_to_mysql(ret);
}

/**
  @brief
  max_supported_keys() is called when create indexes;

  @details
  To get the the maximum number of indexes per table of CANTIAN
*/
uint ha_ctc::max_supported_keys() const {
  return CTC_MAX_KEY_NUM;
}

/**
  @brief
  max_supported_key_length() is called when create indexes;

  @details
  To get the max possible key length of CANTIAN
*/
uint ha_ctc::max_supported_key_length() const {
  return CTC_MAX_KEY_LENGTH;
}

/**
  @brief
  max_supported_key_parts() is called when create indexes;

  @details
  To get the maximum columns of a composite index
*/
uint ha_ctc::max_supported_key_parts() const {
  return CTC_MAX_KEY_PARTS;
}

/**
  @brief
  max_supported_key_part_length() is called when create indexes;

  @details
  To get the maximum supported indexed columns length
*/
uint ha_ctc::max_supported_key_part_length(
    HA_CREATE_INFO *create_info MY_ATTRIBUTE((unused))) const {
  return CTC_MAX_KEY_PART_LENGTH;
}

enum_alter_inplace_result ha_ctc::check_if_supported_inplace_alter(
    TABLE *altered_table, Alter_inplace_info *ha_alter_info) {
  DBUG_TRACE;
  THD *thd = ha_thd();

  // remote node execute ALTER statement using default way
  if (engine_ddl_passthru(thd)) {
    return HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
  }

  m_error_if_not_empty = ha_alter_info->error_if_not_empty;
  if (ha_alter_info->handler_flags & ~(CTC_INPLACE_IGNORE | CTC_ALTER_NOREBUILD | CTC_ALTER_REBUILD)) {
    if (ha_alter_info->handler_flags & COLUMN_TYPE_OPERATIONS) {
      ha_alter_info->unsupported_reason = my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_COLUMN_TYPE);
      return HA_ALTER_INPLACE_NOT_SUPPORTED;
    }
  }

  /* Only support NULL -> NOT NULL change if strict table sql_mode is set. */
  if ((ha_alter_info->handler_flags & Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE) &&
      !thd_is_strict_mode(thd)) {
    ha_alter_info->unsupported_reason = my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_NOT_NULL);
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  // alter table add column containing stored generated column: json_array() as default
  if (ha_alter_info->handler_flags & Alter_inplace_info::ADD_STORED_GENERATED_COLUMN) {
    ha_alter_info->unsupported_reason = my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON);
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  if (ha_alter_info->handler_flags & CTC_ALTER_COL_ORDER) {
    ha_alter_info->unsupported_reason = "Altering column order or drop column";
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  if ((ha_alter_info->handler_flags & Alter_inplace_info::ALTER_RENAME) &&
      (strcmp(altered_table->s->db.str, table->s->db.str) != 0)) {
    ha_alter_info->unsupported_reason = "Table is renamed cross database";
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  // alter table add NOT NULL column
  if (ha_alter_info->handler_flags & Alter_inplace_info::ADD_STORED_BASE_COLUMN) {
    uint32_t old_table_fields = table->s->fields;
    uint32_t new_table_fields = altered_table->s->fields;
    uint32_t drop_list_size = ha_alter_info->alter_info->drop_list.size();
    uint32_t create_list_size = ha_alter_info->alter_info->create_list.size();
    uint32_t index_drop_count = ha_alter_info->index_drop_count;

    // if this table have any added columns
    uint32_t add_column_size = new_table_fields - old_table_fields + (drop_list_size - index_drop_count);

    ctc_log_system("[SUPPORT_INPLACE]:old_table_fields:%u new_table_fields:%u drop_list_size:%u create_list_size:%u add_column_size:%u, index_drop_count:%u",
      old_table_fields, new_table_fields, drop_list_size, create_list_size, add_column_size, index_drop_count);

    for (int i = add_column_size; i > 0; i--) {
      int32_t add_column_idx = create_list_size - i;
      if (add_column_idx < 0) {
        assert(0);
        ctc_log_error("[SUPPORT_INPLACE]: add_column_idx smaller than 0.");
        break;
      }

      bool is_nullable = ha_alter_info->alter_info->create_list[add_column_idx]->is_nullable;  // NOT NULL
      bool is_have_default_val = ha_alter_info->alter_info->create_list[add_column_idx]->constant_default == nullptr ? false : true;
      if (!is_nullable && !is_have_default_val) {
        ha_alter_info->unsupported_reason = my_get_err_msg(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON);
        return HA_ALTER_INPLACE_NOT_SUPPORTED;
      }
    }
  }

  if (ha_alter_info->handler_flags & Alter_inplace_info::REORGANIZE_PARTITION) {
    ha_alter_info->unsupported_reason = "Reorganize Partition";
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }

  if ((ha_alter_info->handler_flags & PARTITION_OPERATIONS) != 0 &&
       altered_table->part_info->get_full_clone(thd)->part_type == partition_type::HASH) {
      ha_alter_info->unsupported_reason = "INPLACE is not supported for this operation.";
      return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }
  return HA_ALTER_INPLACE_EXCLUSIVE_LOCK;
}

/**
  @brief
  Construct ctc range key based on mysql range key
*/
void ha_ctc::set_ctc_range_key(ctc_key *ctc_key, key_range *mysql_range_key, bool is_min_key) {
  if (!mysql_range_key) {
    ctc_key->key = nullptr;
    ctc_key->cmp_type = CMP_TYPE_NULL;
    ctc_key->len = 0;
    ctc_key->col_map = 0;
    return;
  }

  ctc_key->col_map = mysql_range_key->keypart_map;
  ctc_key->key = mysql_range_key->key;
  ctc_key->len = mysql_range_key->length;
  
  switch(mysql_range_key->flag) {
    case HA_READ_KEY_EXACT:
      ctc_key->cmp_type = CMP_TYPE_CLOSE_INTERNAL;
      break;
    case HA_READ_BEFORE_KEY:
      ctc_key->cmp_type = CMP_TYPE_OPEN_INTERNAL;
      break;
    case HA_READ_AFTER_KEY:
      ctc_key->cmp_type = is_min_key ? CMP_TYPE_OPEN_INTERNAL : CMP_TYPE_CLOSE_INTERNAL;
      break;
    default:
      ctc_key->cmp_type = CMP_TYPE_NULL;
  }
}

/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_ctc::records_in_range(uint inx, key_range *min_key,
                                 key_range *max_key) {
  BEGIN_RECORD_STATS
  DBUG_TRACE;
  ctc_key ctc_min_key;
  ctc_key ctc_max_key;
  set_ctc_range_key(&ctc_min_key, min_key, true);
  set_ctc_range_key(&ctc_max_key, max_key, false);
  if (ctc_max_key.len < ctc_min_key.len) {
    ctc_max_key.cmp_type = CMP_TYPE_NULL;
  } else if (ctc_max_key.len > ctc_min_key.len) {
    ctc_min_key.cmp_type = CMP_TYPE_NULL;
  }
  ctc_range_key key = {&ctc_min_key, &ctc_max_key};

  uint64_t n_rows = 0;
  double density;

  if (m_share) {
    if (!m_share->cbo_stats->is_updated) {
        ctc_log_debug("table %s has not been analyzed", table->alias);
        END_RECORD_STATS(EVENT_TYPE_CBO_RECORDS_IN_RANGE)
        return 1;
    }
    density = calc_density_one_table(inx, &key, m_share->cbo_stats->ctc_cbo_stats_table, *table);
    /*
    * This is a safe-guard logic since we don't handle ctc call error in this method,
    * we need this to make sure that our optimizer continue to work even when we
    * miscalculated the density, and it's still prefer index read
    */
    n_rows = m_share->cbo_stats->ctc_cbo_stats_table->estimate_rows * density;
  }

  /*
  * The MySQL optimizer seems to believe an estimate of 0 rows is
  * always accurate and may return the result 'Empty set' based on that
  */
  if (n_rows == 0) {
      n_rows = 1;
  }
  END_RECORD_STATS(EVENT_TYPE_CBO_RECORDS_IN_RANGE)
  return n_rows;
}

int ha_ctc::records(ha_rows *num_rows) /*!< out: number of rows */
{
  DBUG_TRACE;
  uint64_t n_rows = 0;
  ct_errno_t ret = CT_SUCCESS;
 
  THD *thd = ha_thd();
  update_member_tch(m_tch, ctc_hton, thd);
  char *index_name = nullptr;
  if (active_index != MAX_KEY) {
    index_name = const_cast<char*> (table->key_info[active_index].name);
  }

  /* Count the records */
  ret = (ct_errno_t)ctc_scan_records(&m_tch, &n_rows, index_name);
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  
  assert(ret == CT_SUCCESS);
  if (ret != CT_SUCCESS) {
    ctc_log_error("scan records failed with error code: %d", ret);
    return convert_ctc_error_code_to_mysql(ret);
  }
 
  *num_rows = n_rows;
  return 0;
}
 
int ha_ctc::records_from_index(ha_rows *num_rows, uint inx)
{
  BEGIN_RECORD_STATS
  active_index = inx;
  int ret = records(num_rows);
  active_index = MAX_KEY;
  END_RECORD_STATS(EVENT_TYPE_CBO_RECORDS)
  return ret;
}

/**
 * Retrieves and validates the statistics sample size system variable from Cantian.
 * 
 * This function:
 * 1. Gets the sample size value from Cantian engine via ctc_get_sample_size_value()
 * 2. Validates that the value falls within acceptable range [CTC_MIN_SAMPLE_SIZE, CTC_MAX_SAMPLE_SIZE]
 * 3. If valid, updates the global ctc_sample_size variable used for statistics sampling
 * 
 * The sample size determines how much data is sampled when gathering table statistics.
 * Invalid values are silently ignored, leaving ctc_sample_size unchanged.
 */
void ctc_get_sample_size_value() {
  uint32_t sample_size = 0;
  if (ctc_get_sample_size(&sample_size) == CT_SUCCESS) {
    if (sample_size >= CTC_MIN_SAMPLE_SIZE && sample_size <= CTC_MAX_SAMPLE_SIZE) {
      ctc_sample_size = sample_size;
    }
  }
}

int32_t ctc_get_cluster_role() {
  /* Normally, the cluster type should only be PRIMARY or STANDBY */
  if (ctc_cluster_role == (int32_t)dis_cluster_role::PRIMARY ||
      ctc_cluster_role == (int32_t)dis_cluster_role::STANDBY) {
      return ctc_cluster_role;
  }
  lock_guard<mutex> lock(m_ctc_cluster_role_mutex);
  bool is_slave = false;
  bool cantian_cluster_ready = false;
  int ret = ctc_query_cluster_role(&is_slave, &cantian_cluster_ready);
  if (ret != CT_SUCCESS || !cantian_cluster_ready) {
    ctc_cluster_role = (int32_t)dis_cluster_role::CLUSTER_NOT_READY;
    ctc_log_error("[Disaster Rocovery] ctc_query_cluster_role failed with error code: %d, is_slave:%d, cantian_cluster_ready: %d", ret, is_slave, cantian_cluster_ready);
    return ctc_cluster_role;
  }
  ctc_log_system("[Disaster Recovery] is_slave:%d, cantian_cluster_ready:%d", is_slave, cantian_cluster_ready);
  // ctc_cluster_role: The character the node was before change
  // is_slave: The character the node is now
  // Only reset 'read_only's to false when the node turns from slave to master
  if (is_slave) {
    ctc_set_mysql_read_only();
  } else if (ctc_cluster_role == (int32_t)dis_cluster_role::STANDBY) {
    ctc_reset_mysql_read_only();
  }
  ctc_cluster_role = is_slave ? (int32_t)dis_cluster_role::STANDBY : (int32_t)dis_cluster_role::PRIMARY;
 
  return ctc_cluster_role;
}

int32_t ctc_get_shm_file_num(uint32_t *shm_file_num) {
  lock_guard<mutex> lock(m_ctc_shm_file_num_mutex);
  int ret = ctc_query_shm_file_num(shm_file_num);
  return ret;
}

extern uint32_t g_shm_file_num;
int32_t ctc_get_shm_usage(uint32_t *ctc_shm_usage) {
  uint32_t size = (g_shm_file_num + 1)  * MEM_CLASS_NUM * sizeof(uint32_t);
  uint32_t *shm_usage = (uint32_t *)ctc_alloc_buf(NULL, size);
  memset(shm_usage, 0, size);
  int ret = ctc_query_shm_usage(shm_usage);
  memcpy(ctc_shm_usage, shm_usage, size);
  ctc_free_buf(nullptr, (uint8_t *)shm_usage);
  return ret;
}

/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

  @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for ctc, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

  @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

  @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_ctc::store_lock(THD *, THR_LOCK_DATA **to,
                                   enum thr_lock_type lock_type) {
  /*
    This method should not be called for internal temporary tables
    as they don't have properly initialized THR_LOCK and THR_LOCK_DATA
    structures.
    cantian engine dose not need mysql lock type, need long testing on this.
    May need map mysql lock type to cantian lock type in the future after figure
    out they lock meaning.
  */
  DBUG_TRACE;

  // SELECT FOR SHARE / SELECT FOR UPDATE use exclusive lock
  if ((lock_type == TL_READ_WITH_SHARED_LOCKS && ctc_get_cluster_role() == (int32_t)dis_cluster_role::PRIMARY) ||
      (lock_type >= TL_WRITE_ALLOW_WRITE && lock_type <= TL_WRITE_ONLY)) {
    m_select_lock = lock_mode::EXCLUSIVE_LOCK;
  } else {
    m_select_lock = lock_mode::SHARED_LOCK;
  }

  return to;
}

/**
  @brief
  As MySQL will execute an external lock for every new table it uses when it
  starts to process an SQL statement (an exception is when MySQL calls
  start_stmt directly for the handle when thread has table lock explicitly),
  we will use this function to indicate CTC that a new SQL statement has started.

  @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

  @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_ctc::external_lock(THD *thd, int lock_type) {
  /*
    cantian dose not need mysql lock type, need long testing on this.
    May need map mysql lock type to cantian lock type in the future after figure
    out they lock meaning.
  */
  DBUG_TRACE;
  BEGIN_RECORD_STATS
  if (IS_METADATA_NORMALIZATION() &&
    ctc_check_if_log_table(table_share->db.str, table_share->table_name.str)) {
    is_log_table = true;
    if (!thd->in_sub_stmt) {
      thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(ctc_hton, thd);
      sess_ctx->sql_stat_start = 1;
      m_tch.sql_stat_start = 1;
    }
    END_RECORD_STATS(EVENT_TYPE_BEGIN_TRX)
    return 0;
  }

  is_log_table = false;
  
  if (engine_ddl_passthru(thd) && (is_create_table_check(thd) || is_alter_table_copy(thd))) {
    END_RECORD_STATS(EVENT_TYPE_BEGIN_TRX)
    return 0;
  }
  
  // F_RDLCK:0, F_WRLCK:1, F_UNLCK:2
  if (lock_type == F_UNLCK) {
    m_select_lock = lock_mode::NO_LOCK;
    END_RECORD_STATS(EVENT_TYPE_BEGIN_TRX)
    return 0;
  }

  int ret =  start_stmt(thd, TL_IGNORE);
  END_RECORD_STATS(EVENT_TYPE_BEGIN_TRX)
  return ret;
}

/**
  @brief
  When thread has table lock explicitly, MySQL will execute an external lock
  for every new table it uses instead of external lock.
  1. Without explicit table lock, MySQL execute external_lock,
     we make external_lock to call start_stmt for ctc_trx_begin.
  2. With explicit table lock, MySQL execute start_stmt directly.
  We will use this function to indicate CTC that a new SQL statement has started.

  @details
  Called from ha_ctc::external_lock().
  Called from sql_base.cc by check_lock_and_start_stmt().

  @see
  sql_base.cc by check_lock_and_start_stmt().
*/
int ha_ctc::start_stmt(THD *thd, thr_lock_type) {
  DBUG_TRACE;

  trans_register_ha(thd, false, ht, nullptr); // register trans to STMT

  update_member_tch(m_tch, ctc_hton, thd, false);
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(ctc_hton, thd);
  if (sess_ctx == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  sess_ctx->thd_id = thd->thread_id();
  if (!thd->in_sub_stmt) {
    sess_ctx->sql_stat_start = 1;  // indicate cantian for a new sql border
    m_tch.sql_stat_start = 1;
  }

  // lock tables不开启事务
  if (thd->query_plan.get_command() == SQLCOM_LOCK_TABLES) {
    return 0;
  }

  // if session level transaction we only start one time
  if (sess_ctx->is_ctc_trx_begin) {
    assert(m_tch.sess_addr != INVALID_VALUE64);
    assert(m_tch.thd_id == thd->thread_id());
    return 0;
  }

  uint32_t lock_wait_timeout = THDVAR(thd, lock_wait_timeout);
  uint32_t autocommit = !thd->in_multi_stmt_transaction_mode();
  int isolation_level = isolation_level_to_cantian(thd_get_trx_isolation(thd));
  
  ctc_trx_context_t trx_context = {isolation_level, autocommit, lock_wait_timeout, m_select_lock == lock_mode::EXCLUSIVE_LOCK};
  
  bool is_mysql_local = (sess_ctx->set_flag & CTC_DDL_LOCAL_ENABLED);
  ct_errno_t ret = (ct_errno_t)ctc_trx_begin(&m_tch, trx_context, is_mysql_local);
  
  check_error_code_to_mysql(ha_thd(), &ret);

  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  
  if (ret != CT_SUCCESS) {
    ctc_log_error("start trx failed with error code: %d", ret);
    return convert_ctc_error_code_to_mysql(ret);
  }
  
  sess_ctx->is_ctc_trx_begin = 1;
  if (!autocommit) {
    trans_register_ha(thd, true, ht, nullptr);
  }
  return 0;
}

/** Return partitioning flags. */
static uint ctc_partition_flags() {
  return (HA_CANNOT_PARTITION_FK | HA_TRUNCATE_PARTITION_PRECLOSE);
}

struct st_mysql_storage_engine ctc_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static bool ctc_get_tablespace_statistics(
    const char *tablespace_name, const char *file_name,
    const dd::Properties &ts_se_private_data, ha_tablespace_statistics *stats) {
    UNUSED_PARAM(tablespace_name);
    UNUSED_PARAM(file_name);
    UNUSED_PARAM(ts_se_private_data);
    UNUSED_PARAM(stats);
    return true;
}

EXTER_ATTACK bool ctc_drop_database_with_err(handlerton *hton, char *path) {
  BEGIN_RECORD_STATS
  THD *thd = current_thd;
  assert(thd != nullptr);

  if (engine_ddl_passthru(thd)) {
    END_RECORD_STATS(EVENT_TYPE_DROP_DB)
    return false;
  }

  ctc_handler_t tch;
  int res = get_tch_in_handler_data(hton, thd, tch);
  if (res != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_DROP_DB)
    return true;
  }

  char db_name[SMALL_RECORD_SIZE] = { 0 };
  ctc_split_normalized_name(path, db_name, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
  int error_code = 0;
  char error_message[ERROR_MESSAGE_LEN] = {0};
  /* ctc_drop_tablespace_and_user接口内部新建session并自己释放 */
  string sql = string(thd->query().str).substr(0, thd->query().length);
  int ret = ctc_drop_tablespace_and_user(
      &tch, db_name, sql.c_str(),
      thd->m_main_security_ctx.priv_user().str,
      thd->m_main_security_ctx.priv_host().str, &error_code, error_message);
  update_sess_ctx_by_tch(tch, hton, thd);
  ctc_log_system("[CTC_DROP_DB]: ret:%d, database(%s), error_code:%d, error_message:%s",
    ret, db_name, error_code, error_message);
  if (ret != CT_SUCCESS) {
    ctc_log_error("drop database failed with error code: %d", convert_ctc_error_code_to_mysql((ct_errno_t)ret));
    END_RECORD_STATS(EVENT_TYPE_DROP_DB)
    return true;
  }
  END_RECORD_STATS(EVENT_TYPE_DROP_DB)
  return false;
}

EXTER_ATTACK void ctc_drop_database(handlerton *hton, char *path) {
  (void)ctc_drop_database_with_err(hton, path);
}

static int ctc_check_tx_isolation() {
  // 检查GLOBAL变量
  enum_tx_isolation tx_isol = (enum_tx_isolation)global_system_variables.transaction_isolation;
  if (tx_isol == ISO_SERIALIZABLE || tx_isol == ISO_READ_UNCOMMITTED) {
    ctc_log_error("CTC init failed. GLOBAL transaction isolation can not "
      "be SERIALIZABLE and READ-UNCOMMITTED. Please check system variable or my.cnf file.");
    return HA_ERR_INITIALIZATION;
  }

  // 检查SESSION 变量
  THD *thd = current_thd;
  if (thd && (thd->tx_isolation == ISO_SERIALIZABLE || thd->tx_isolation == ISO_READ_UNCOMMITTED)) {
    ctc_log_error("CTC init failed. SESSION transaction isolation can not "
      "be SERIALIZABLE and READ-UNCOMMITTED. Please check system variable or my.cnf file.");
    return HA_ERR_INITIALIZATION;
  }
  return 0;
}

static int ctc_create_db(THD *thd, handlerton *hton) {
  if (engine_skip_ddl(thd)) {
    return CT_SUCCESS;
  }
  ctc_handler_t tch;
  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));

  ctc_ddl_broadcast_request broadcast_req {{0}, {0}, {0}, {0}, 0, 0, 0, 0, {0}};

  DBUG_EXECUTE_IF("core_before_create_tablespace_and_db", { assert(0); });  // 有锁的问题
  
  string sql = string(thd->query().str).substr(0, thd->query().length);
  FILL_BROADCAST_BASE_REQ(broadcast_req, sql.c_str(), thd->m_main_security_ctx.priv_user().str,
    thd->m_main_security_ctx.priv_host().str, ctc_instance_id, tch.sql_command);
  broadcast_req.options &= (~CTC_NOT_NEED_CANTIAN_EXECUTE);
  int ret = ctc_execute_mysql_ddl_sql(&tch, &broadcast_req, false);

  DBUG_EXECUTE_IF("core_after_create_tablespace_and_db", { assert(0); });  // 元数据不一致的问题
  ctc_log_system("[CTC_BROARDCAST_CREATE_DB]:ret:%d, query:%s, user_name:%s, err_code:%d, broadcast_inst_id:%u, "
    "conn_id:%u, ctc_inst_id:%u", ret, broadcast_req.sql_str, broadcast_req.user_name,
    broadcast_req.err_code, broadcast_req.mysql_inst_id, tch.thd_id, tch.inst_id);
  update_sess_ctx_by_tch(tch, hton, thd);
  assert(ret == CT_SUCCESS);
  
  return ret;
}

bool ctc_binlog_log_query_with_err(handlerton *hton, THD *thd,
                                   enum_binlog_command binlog_command,
                                   const char *query, uint query_length,
                                   const char *db, const char *table_name) {
  UNUSED_PARAM(query);
  UNUSED_PARAM(query_length);
  UNUSED_PARAM(db);
  UNUSED_PARAM(table_name);
  if (engine_ddl_passthru(thd)) {
    return false;
  }
  if (binlog_command == LOGCOM_CREATE_DB) {
    return ctc_create_db(thd, hton);
  }
  return false;
}

void ctc_binlog_log_query(handlerton *hton, THD *thd,
                          enum_binlog_command binlog_command,
                          const char *query, uint query_length,
                          const char *db, const char *table_name) {
  (void)ctc_binlog_log_query_with_err(hton, thd, binlog_command, query, query_length, db, table_name);
}

/** Return 0 on success and non-zero on failure.
@param[in]	hton	the ctc handlerton
@param[in]	thd		the MySQL query thread of the caller
@param[in]	stat_print	print function
@param[in]	stat_type	status to show */
static bool ctc_show_status(handlerton *, THD *thd, stat_print_fn *stat_print, enum ha_stat_type stat_type) {
  if (stat_type == HA_ENGINE_STATUS) {
    ctc_stats::get_instance()->print_stats(thd, stat_print);
  }

  return false;
}

bool is_single_run_mode()
{
#ifndef WITH_CANTIAN
  return false;
#else
  return true;
#endif
}

void ctc_set_metadata_switch() { // MySQL为元数据归一版本
  lock_guard<mutex> lock(m_ctc_metadata_normalization_mutex);
  if (ctc_metadata_normalization != (int32_t)metadata_switchs::DEFAULT) {
    return;
  }
  bool cantian_metadata_switch = false;
  bool cantian_cluster_ready = false;
  int ret = ctc_search_metadata_status(&cantian_metadata_switch, &cantian_cluster_ready);
  if (ret != CT_SUCCESS || !cantian_cluster_ready) {
    ctc_metadata_normalization = (int32_t)metadata_switchs::CLUSTER_NOT_READY;
    ctc_log_error("[ctc_set_metadata_switch] ctc_search_metadata_status failed with error code: %d, cantian_cluster_ready: %d", ret, cantian_cluster_ready);
    return;
  }
  ctc_log_system("[ctc_set_metadata_switch] mysql_metadata_switch: 1, cantian_metadata_switch: %d, cantian_cluster_ready: %d", cantian_metadata_switch, cantian_cluster_ready);
  ctc_metadata_normalization = cantian_metadata_switch ? (int32_t)metadata_switchs::MATCH_META : (int32_t)metadata_switchs::NOT_MATCH;
}
 
int32_t ctc_get_metadata_switch() {
  if (ctc_metadata_normalization != (int32_t)metadata_switchs::DEFAULT) {
    return ctc_metadata_normalization;
  }

  lock_guard<mutex> lock(m_ctc_metadata_normalization_mutex);
  if (ctc_metadata_normalization != (int32_t)metadata_switchs::DEFAULT) {
    return ctc_metadata_normalization;
  }
  bool mysql_metadata_switch = CHECK_HAS_MEMBER(handlerton, get_metadata_switch);
  bool cantian_metadata_switch = false;
  bool cantian_cluster_ready = false;
  int ret = ctc_search_metadata_status(&cantian_metadata_switch, &cantian_cluster_ready);
  if (ret != CT_SUCCESS || !cantian_cluster_ready) {
    ctc_metadata_normalization = (int32_t)metadata_switchs::CLUSTER_NOT_READY;
    ctc_log_error("[ctc_get_metadata_switch] ctc_search_metadata_status failed with error code: %d, cantian_metadata_switch: %d, cantian_cluster_ready: %d", ret, cantian_metadata_switch, cantian_cluster_ready);
    return ctc_metadata_normalization;
  }
  ctc_log_system("[ctc_get_metadata_switch] cantian_metadata_switch: %d, cantian_cluster_ready: %d", cantian_metadata_switch, cantian_cluster_ready);
  ctc_metadata_normalization = (mysql_metadata_switch == cantian_metadata_switch) ? (mysql_metadata_switch ? (int32_t)metadata_switchs::MATCH_META : (int32_t)metadata_switchs::MATCH_NO_META) : (int32_t)metadata_switchs::NOT_MATCH;

  return ctc_metadata_normalization;
}


/**
  Check metadata init status in CTC.
*/
static int ctc_get_metadata_status() {
  DBUG_TRACE;

  bool is_exists;
  int ret = 0;
  ct_errno_t begin = (ct_errno_t)ctc_check_db_table_exists("mysql", "", &is_exists);
  if (begin != CT_SUCCESS) {
    ctc_log_error("check metadata init start failed with error code: %d", begin);
    return convert_ctc_error_code_to_mysql(begin);
  }
  ret = is_exists ? 1 : 0;

  ct_errno_t end = (ct_errno_t)ctc_check_db_table_exists("sys", "sys_config", &is_exists);
  if (end != CT_SUCCESS) {
    ctc_log_error("check metadata init end failed with error code: %d", end);
    return convert_ctc_error_code_to_mysql(end);
  }
  ret = is_exists ? 2 : ret;
  return ret;
}

static int ctc_init_tablespace(List<const Plugin_tablespace> *tablespaces)
{
  DBUG_TRACE;
  const size_t len = 30 + sizeof("id=;flags=;server_version=;space_version=;state=normal");
  const char *fmt = "id=%u;flags=%u;server_version=%u;space_version=%u;state=normal";
  static char se_private_data_dd[len];
  snprintf(se_private_data_dd, len, fmt, 8, 0, 0, 0);
 
  static Plugin_tablespace dd_space((const char *)"mysql", "", se_private_data_dd, "", (const char *)"CTC");
  static Plugin_tablespace::Plugin_tablespace_file dd_file((const char *)"mysql.ibd", "");
  dd_space.add_file(&dd_file);
  tablespaces->push_back(&dd_space);
  return 0;
}

static bool ctc_ddse_dict_init(
    dict_init_mode_t dict_init_mode, uint version,
    List<const dd::Object_table> *tables,
    List<const Plugin_tablespace> *tablespaces) {
  DBUG_TRACE;
  ctc_log_system("[CTC_INIT]: begin ctc_ddse_dict_init.");

  assert(tables && tables->is_empty());
  assert(tablespaces && tablespaces->is_empty());
  assert(dict_init_mode == DICT_INIT_CREATE_FILES || dict_init_mode == DICT_INIT_CHECK_FILES);
  assert(version < 1000000000);
  // valid value check
  if (!(tables && tables->is_empty()) || !(tablespaces && tablespaces->is_empty()) ||
      !(dict_init_mode == DICT_INIT_CREATE_FILES || dict_init_mode == DICT_INIT_CHECK_FILES) ||
      version >= 1000000000) {
    return true;
  }

  if (ctc_init_tablespace(tablespaces)) {
    return true;
  }

  /* Instantiate table defs only if we are successful so far. */
  dd::Object_table *innodb_dynamic_metadata =
      dd::Object_table::create_object_table();
  innodb_dynamic_metadata->set_hidden(true);
  dd::Object_table_definition *def =
      innodb_dynamic_metadata->target_table_definition();
  def->set_table_name("innodb_dynamic_metadata");
  def->add_field(0, "table_id", "table_id BIGINT UNSIGNED NOT NULL");
  def->add_field(1, "version", "version BIGINT UNSIGNED NOT NULL");
  def->add_field(2, "metadata", "metadata BLOB NOT NULL");
  def->add_index(0, "index_pk", "PRIMARY KEY (table_id)");
  /* Options and tablespace are set at the SQL layer. */

  /* Changing these values would change the specification of innodb statistics
  tables. */
  static constexpr size_t DB_NAME_FIELD_SIZE = 64;
  static constexpr size_t TABLE_NAME_FIELD_SIZE = 199;

  /* Set length for database name field. */
  std::ostringstream db_name_field;
  db_name_field << "database_name VARCHAR(" << DB_NAME_FIELD_SIZE
                << ") NOT NULL";
  std::string db_field = db_name_field.str();

  /* Set length for table name field. */
  std::ostringstream table_name_field;
  table_name_field << "table_name VARCHAR(" << TABLE_NAME_FIELD_SIZE
                   << ") NOT NULL";
  std::string table_field = table_name_field.str();

  dd::Object_table *innodb_table_stats =
      dd::Object_table::create_object_table();
  innodb_table_stats->set_hidden(false);
  def = innodb_table_stats->target_table_definition();
  def->set_table_name("innodb_table_stats");
  def->add_field(0, "database_name", db_field.c_str());
  def->add_field(1, "table_name", table_field.c_str());
  def->add_field(2, "last_update",
                 "last_update TIMESTAMP NOT NULL \n"
                 "  DEFAULT CURRENT_TIMESTAMP \n"
                 "  ON UPDATE CURRENT_TIMESTAMP");
  def->add_field(3, "n_rows", "n_rows BIGINT UNSIGNED NOT NULL");
  def->add_field(4, "clustered_index_size",
                 "clustered_index_size BIGINT UNSIGNED NOT NULL");
  def->add_field(5, "sum_of_other_index_sizes",
                 "sum_of_other_index_sizes BIGINT UNSIGNED NOT NULL");
  def->add_index(0, "index_pk", "PRIMARY KEY (database_name, table_name)");
  /* Options and tablespace are set at the SQL layer. */

  dd::Object_table *innodb_index_stats =
      dd::Object_table::create_object_table();
  innodb_index_stats->set_hidden(false);
  def = innodb_index_stats->target_table_definition();
  def->set_table_name("innodb_index_stats");
  def->add_field(0, "database_name", db_field.c_str());
  def->add_field(1, "table_name", table_field.c_str());
  def->add_field(2, "index_name", "index_name VARCHAR(64) NOT NULL");
  def->add_field(3, "last_update",
                 "last_update TIMESTAMP NOT NULL"
                 "  DEFAULT CURRENT_TIMESTAMP"
                 "  ON UPDATE CURRENT_TIMESTAMP");
  /*
          There are at least: stat_name='size'
                  stat_name='n_leaf_pages'
                  stat_name='n_diff_pfx%'
  */
  def->add_field(4, "stat_name", "stat_name VARCHAR(64) NOT NULL");
  def->add_field(5, "stat_value", "stat_value BIGINT UNSIGNED NOT NULL");
  def->add_field(6, "sample_size", "sample_size BIGINT UNSIGNED");
  def->add_field(7, "stat_description",
                 "stat_description VARCHAR(1024) NOT NULL");
  def->add_index(0, "index_pk",
                 "PRIMARY KEY (database_name, table_name, "
                 "index_name, stat_name)");
  /* Options and tablespace are set at the SQL layer. */

  dd::Object_table *innodb_ddl_log = dd::Object_table::create_object_table();
  innodb_ddl_log->set_hidden(true);
  def = innodb_ddl_log->target_table_definition();
  def->set_table_name("innodb_ddl_log");
  def->add_field(0, "id", "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT");
  def->add_field(1, "thread_id", "thread_id BIGINT UNSIGNED NOT NULL");
  def->add_field(2, "type", "type INT UNSIGNED NOT NULL");
  def->add_field(3, "space_id", "space_id INT UNSIGNED");
  def->add_field(4, "page_no", "page_no INT UNSIGNED");
  def->add_field(5, "index_id", "index_id BIGINT UNSIGNED");
  def->add_field(6, "table_id", "table_id BIGINT UNSIGNED");
  def->add_field(7, "old_file_path",
                 "old_file_path VARCHAR(512) COLLATE UTF8_BIN");
  def->add_field(8, "new_file_path",
                 "new_file_path VARCHAR(512) COLLATE UTF8_BIN");
  def->add_index(0, "index_pk", "PRIMARY KEY(id)");
  def->add_index(1, "index_k_thread_id", "KEY(thread_id)");
  /* Options and tablespace are set at the SQL layer. */

  tables->push_back(innodb_dynamic_metadata);
  tables->push_back(innodb_table_stats);
  tables->push_back(innodb_index_stats);
  tables->push_back(innodb_ddl_log);

  ctc_log_system("[CTC_INIT]:end init dict!");

  return false;
}

/** Set of ids of DD tables */
static set<dd::Object_id> s_dd_table_ids;
 
static bool is_dd_table_id(uint16_t id) {
  DBUG_TRACE;
  return (s_dd_table_ids.find(id) != s_dd_table_ids.end());
}

static void ctc_dict_register_dd_table_id(dd::Object_id dd_table_id) {
  DBUG_TRACE;
  s_dd_table_ids.insert(dd_table_id);
  return;
}
 
static bool ctc_dict_recover(dict_recovery_mode_t, uint){
  DBUG_TRACE;
  return false;
}
 
static bool ctc_dict_get_server_version(uint *version) {
  DBUG_TRACE;
  *version = MYSQL_VERSION_ID;
  return false;
}
 
static bool ctc_dict_set_server_version() {
  DBUG_TRACE;
  return false;
}
 
static void ctc_dict_cache_reset(const char *, const char *) {
  DBUG_TRACE;
  return;
}
 
static void ctc_dict_cache_reset_tables_and_tablespaces() {
  DBUG_TRACE;
  return;
}

static int ctc_op_before_load_meta(THD *thd) {
  bool need_forward = true;
  return ctc_check_lock_instance(thd, need_forward);
}
 
static int ctc_op_after_load_meta(THD *thd) {
  return ctc_check_unlock_instance(thd);
}
 
static bool ctc_dict_readonly() {
  return false;
}

template <typename T>
static typename std::enable_if<CHECK_HAS_MEMBER(T, get_inst_id)>::type set_hton_members(T *ctc_hton) {
  ctc_hton->get_inst_id = ha_ctc_get_inst_id;
  ctc_hton->get_metadata_switch = ctc_get_metadata_switch;
  ctc_hton->set_metadata_switch = ctc_set_metadata_switch;
  ctc_hton->get_metadata_status = ctc_get_metadata_status;
  ctc_hton->op_before_load_meta = ctc_op_before_load_meta;
  ctc_hton->op_after_load_meta = ctc_op_after_load_meta;
  ctc_hton->drop_database = ctc_drop_database_with_err;
  ctc_hton->binlog_log_query = ctc_binlog_log_query_with_err;
  ctc_hton->get_cluster_role = ctc_get_cluster_role;
  ctc_hton->update_sysvars = ctc_update_sysvars;
}

template <typename T>
static typename std::enable_if<!CHECK_HAS_MEMBER(T, get_inst_id)>::type set_hton_members(T *ctc_hton) {
  ctc_hton->drop_database = ctc_drop_database;
  ctc_hton->binlog_log_query = ctc_binlog_log_query;
}

#if FEATURE_FOR_EVERSQL
int ha_ctc_parallel_read_create_data_fetcher(parallel_read_create_data_fetcher_ctx_t &data_fetcher_ctx,
                                             void *&data_fetcher) {
  UNUSED_PARAM(data_fetcher_ctx);
  UNUSED_PARAM(data_fetcher);
  return 0;
}

int ha_ctc_parallel_read_destory_data_fetcher(void *&data_fetcher) {
  UNUSED_PARAM(data_fetcher);
  return 0;
}

int ha_ctc_parallel_read_start_data_fetch(parallel_read_start_data_fetch_ctx_t &start_data_fetch_ctx) {
  UNUSED_PARAM(start_data_fetch_ctx);
  return 0;
}

int ha_ctc_parallel_read_init_data_fetcher(parallel_read_init_data_fetcher_ctx_t &init_data_fetcher_ctx) {
  UNUSED_PARAM(init_data_fetcher_ctx);
  return 0;
}

int ha_ctc_parallel_read_add_target_to_data_fetcher(
  parallel_read_add_target_to_data_fetcher_ctx_t &target_to_data_fetcher_ctx) {
  UNUSED_PARAM(target_to_data_fetcher_ctx);
  return 0;
}

int ha_ctc_parallel_read_end_data_fetch(parallel_read_end_data_fetch_ctx_t &end_data_fetch_ctx) {
  UNUSED_PARAM(end_data_fetch_ctx);
  return 0;
}
#endif

extern int (*ctc_init)();
extern int (*ctc_deinit)();

int ctc_push_to_engine(THD *thd, AccessPath *root_path, JOIN *);

static int ctc_init_func(void *p) {
  DBUG_TRACE;
  ctc_hton = (handlerton *)p;
  ctc_hton->state = SHOW_OPTION_YES;
  ctc_hton->db_type = (legacy_db_type)30;
  ctc_hton->create = ctc_create_handler;
  ctc_hton->is_supported_system_table = ctc_is_supported_system_table;
  ctc_hton->check_fk_column_compat = ctc_check_fk_column_compat;
  ctc_hton->commit = ctc_commit;
  ctc_hton->rollback = ctc_rollback;
  ctc_hton->savepoint_set = ctc_set_savepoint;
  ctc_hton->savepoint_rollback = ctc_rollback_savepoint;
  ctc_hton->savepoint_release = ctc_release_savepoint;
  ctc_hton->close_connection = ctc_close_connect;
  ctc_hton->kill_connection = ctc_kill_connection;
  ctc_hton->notify_exclusive_mdl = ctc_notify_exclusive_mdl;
  ctc_hton->notify_alter_table = ctc_notify_alter_table;
  ctc_hton->start_consistent_snapshot = ctc_start_trx_and_assign_scn;
  ctc_hton->partition_flags = ctc_partition_flags;
  ctc_hton->flags = HTON_SUPPORTS_FOREIGN_KEYS | HTON_CAN_RECREATE | HTON_SUPPORTS_ATOMIC_DDL;
  // TODO: HTON_SUPPORTS_TABLE_ENCRYPTION 表空间 tablespace加密功能暂时不做支持，后面会考虑添加。
  ctc_hton->foreign_keys_flags = HTON_FKS_WITH_PREFIX_PARENT_KEYS |
      HTON_FKS_NEED_DIFFERENT_PARENT_AND_SUPPORTING_KEYS |
      HTON_FKS_WITH_EXTENDED_PARENT_KEYS;
  ctc_hton->alter_tablespace = ctcbase_alter_tablespace;
  ctc_hton->file_extensions = nullptr;
  ctc_hton->get_tablespace_statistics = ctc_get_tablespace_statistics;
  ctc_hton->show_status = ctc_show_status;
  ctc_hton->ddse_dict_init = ctc_ddse_dict_init;
  ctc_hton->dict_register_dd_table_id = ctc_dict_register_dd_table_id;
  ctc_hton->dict_recover = ctc_dict_recover;
  ctc_hton->dict_get_server_version = ctc_dict_get_server_version;
  ctc_hton->dict_set_server_version = ctc_dict_set_server_version;
  ctc_hton->dict_cache_reset = ctc_dict_cache_reset;
  ctc_hton->dict_cache_reset_tables_and_tablespaces = ctc_dict_cache_reset_tables_and_tablespaces;
  ctc_hton->is_dict_readonly = ctc_dict_readonly;
#ifdef FEATURE_X_FOR_MYSQL_32
  ctc_hton->push_to_engine = ctc_push_to_engine;
#endif

#ifdef FEATURE_X_FOR_EVERSQL
  ctc_hton->data_fetcher_interface.parallel_read_create_data_fetcher = ha_ctc_parallel_read_create_data_fetcher;
  ctc_hton->data_fetcher_interface.parallel_read_destory_data_fetcher = ha_ctc_parallel_read_destory_data_fetcher;
  ctc_hton->data_fetcher_interface.parallel_read_start_data_fetch = ha_ctc_parallel_read_start_data_fetch;
  ctc_hton->data_fetcher_interface.parallel_read_init_data_fetcher = ha_ctc_parallel_read_init_data_fetcher;
  ctc_hton->data_fetcher_interface.parallel_read_add_target_to_data_fetcher =
    ha_ctc_parallel_read_add_target_to_data_fetcher;
  ctc_hton->data_fetcher_interface.parallel_read_end_data_fetch = ha_ctc_parallel_read_end_data_fetch;
#endif
  set_hton_members(ctc_hton);
  int ret = ctc_init();
  if (ret != 0) {
    ctc_log_error("[CTC_INIT]: ctc storage engine plugin init failed:%d", ret);
    return HA_ERR_INITIALIZATION;
  }

  // 元数据归一流程初始化下发参天
  // 主干非initialize_insecure模式，需要注册共享内存接收线程并等待参天启动完成
  ret = srv_wait_instance_startuped();
  if (ret != 0) {
    ctc_log_error("wait cantian instance startuped failed:%d", ret);
    return HA_ERR_INITIALIZATION;
  }
  
  ret = ctc_reg_instance();
  if (ret != 0) {
    ctc_log_error("[CTC_INIT]:ctc_reg_instance failed:%d", ret);
    return HA_ERR_INITIALIZATION;
  }
  
  ret = ctc_check_tx_isolation();
  if (ret != 0) {
    ctc_log_error("[CTC_INIT]:ctc_check_tx_isolation failed:%d", ret);
    return HA_ERR_INITIALIZATION;
  }
  ctc_get_cluster_role();

  ctc_get_sample_size_value();

  ctc_log_system("[CTC_INIT]:SUCCESS!");
  return 0;
}

static int ctc_deinit_func(void *p) {
  // handler.cc:726 此处传的p固定为null, 不是handlerton，不能依赖这部分逻辑
  UNUSED_PARAM(p);
  ctc_log_system(
      "ctc_deinit_func ctc_ddl_req_msg_mem_use_heap_cnt:%u, "
      "ctc_ddl_req_msg_mem_max_size:%u.",
      (uint32_t)ctc_ddl_stack_mem::ctc_ddl_req_msg_mem_use_heap_cnt,
      (uint32_t)ctc_ddl_stack_mem::ctc_ddl_req_msg_mem_max_size);
  ctc_unreg_instance();
  return ctc_deinit();
}

static SHOW_VAR ctc_status[] = {
  {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

extern struct st_mysql_plugin g_ctc_ddl_rewriter_plugin;

const char *ctc_hton_name = "CTC";

#pragma GCC visibility push(default)

mysql_declare_plugin(ctc) g_ctc_ddl_rewriter_plugin,{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &ctc_storage_engine,
  ctc_hton_name,
  PLUGIN_AUTHOR_ORACLE,
  "Connector for Cantian storage engine",
  PLUGIN_LICENSE_GPL,
  ctc_init_func,
  nullptr,
  ctc_deinit_func,
  CTC_CLIENT_VERSION_NUMBER,
  ctc_status,
  ctc_system_variables,
  nullptr,
  PLUGIN_OPT_ALLOW_EARLY,
} mysql_declare_plugin_end;

#pragma GCC visibility pop

void ha_ctc::update_create_info(HA_CREATE_INFO *create_info) {
  if ((create_info->used_fields & HA_CREATE_USED_AUTO) || !table->found_next_number_field) {
    return;
  }

  THD* thd = ha_thd();
  if (engine_ddl_passthru(thd) && is_create_table_check(thd)) {
    return;
  }

  int ret = 0;
  if (m_tch.ctx_addr == INVALID_VALUE64) {
    char user_name[SMALL_RECORD_SIZE] = { 0 };
    ctc_split_normalized_name(table->s->normalized_path.str, user_name, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
    ctc_copy_name(user_name, user_name, SMALL_RECORD_SIZE);
    update_member_tch(m_tch, ctc_hton, thd);
    ret = ctc_open_table(&m_tch, table->s->table_name.str, user_name);
    update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
    if (ret != 0) {
      create_info->auto_increment_value = (ulonglong)0;
    }
  }

  uint64_t inc_value = 0;
  uint16_t auto_inc_step = thd->variables.auto_increment_increment;
  uint16_t auto_inc_offset = thd->variables.auto_increment_offset;
  update_member_tch(m_tch, ctc_hton, thd);
  dml_flag_t flag;
  flag.auto_inc_offset = auto_inc_offset;
  flag.auto_inc_step = auto_inc_step;
  flag.auto_increase = false;
  ret = ctc_get_serial_value(&m_tch, &inc_value, flag);
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  if (ret != 0) {
    create_info->auto_increment_value = (ulonglong)0;
  } else {
    create_info->auto_increment_value = (ulonglong)inc_value;
    stats.auto_increment_value = (ulonglong)inc_value;
  }
  
}

/**
  @brief
  Used to delete a table. By the time delete_table() has been called all
  opened references to this table will have been closed (and your globally
  shared references released). The variable name will just be the name of
  the table. You will need to remove any files you have created at this point.

  @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

  @see
  delete_table and ha_create_table() in handler.cc
*/
EXTER_ATTACK int ha_ctc::delete_table(const char *full_path_name, const dd::Table *table_def) {
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  ct_errno_t ret = CT_SUCCESS;

  if (engine_ddl_passthru(thd)) {
    if (thd->locked_tables_mode) {
      for (const dd::Foreign_key_parent *parent_fk : table_def->foreign_key_parents()) {
        close_all_tables_for_name(thd, parent_fk->child_schema_name().c_str(), parent_fk->child_table_name().c_str(), true);
      }
    }
    END_RECORD_STATS(EVENT_TYPE_DROP_TABLE)
    return ret;
  }

  /* 删除db时 会直接删除参天用户 所有表也会直接被删除 无需再次下发 */
  if (thd->lex->sql_command == SQLCOM_DROP_DB) {
    END_RECORD_STATS(EVENT_TYPE_DROP_TABLE)
    return ret;
  }

  if (table_def != nullptr && table_def->is_persistent()) {
     ctc_register_trx(ht, thd);
  }

  update_member_tch(m_tch, ctc_hton, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, m_tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  ctc_ddl_stack_mem stack_mem(0);
  int mysql_ret = fill_delete_table_req(full_path_name, table_def, thd, &ddl_ctrl, &stack_mem);
  if (mysql_ret != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_DROP_TABLE)
    return mysql_ret;
  }
  void *ctc_ddl_req_msg_mem = stack_mem.get_buf();
  if (ctc_ddl_req_msg_mem == nullptr) {
    END_RECORD_STATS(EVENT_TYPE_DROP_TABLE)
    return HA_ERR_OUT_OF_MEM;
  }
  ctc_log_note("ctc_drop_table enter");
  ret = (ct_errno_t)ctc_drop_table(ctc_ddl_req_msg_mem, &ddl_ctrl);
  ctc_log_note("ctc_drop_table finish");
  ctc_ddl_hook_cantian_error("ctc_drop_table_cantian_error", thd, &ddl_ctrl, &ret);
  m_tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  END_RECORD_STATS(EVENT_TYPE_DROP_TABLE)
  return ctc_ddl_handle_fault("ctc_drop_table", thd, &ddl_ctrl, ret, full_path_name, HA_ERR_WRONG_TABLE_NAME);
}
static map<const char *, set<ct_errno_t>>
    g_ctc_ddl_ignore_cantian_errors = {
        {"ctc_create_table_cantian_error", {ERR_DUPLICATE_TABLE}}, // 创建表，自动忽略参天表已经存在的错误
        {"ctc_drop_table_cantian_error", {ERR_TABLE_OR_VIEW_NOT_EXIST}}, // 删除表，自动忽略参天表不存在的错误
        {"ctc_rename_table_cantian_error", {ERR_TABLE_OR_VIEW_NOT_EXIST}}, // rename表，自动忽略参天表不存在的错误
        {"ctc_alter_table_cantian_error", {ERR_OBJECT_EXISTS,ERR_COLUMN_NOT_EXIST}}};

void ctc_ddl_hook_cantian_error(const char *tag, THD *thd, ddl_ctrl_t *ddl_ctrl,
                                ct_errno_t *ret) {
  bool ignore_error = false;

  DBUG_EXECUTE_IF(tag, {
    ignore_error = true;
    *ret = (ct_errno_t)(*ret == 0 ? -1 : *ret);
  });

  if (!ignore_error) {
    return;
  }

  auto st = g_ctc_ddl_ignore_cantian_errors.find(tag);
  if (*ret != 0 && st != g_ctc_ddl_ignore_cantian_errors.end() && st->second.count(*ret) > 0) {
    ctc_log_system(
        "tag:%s cantian ret:%d ignore by ignore_cantian_error_code, "
        "sql:%s, table_name:%s, error_message:%s",
        tag, *ret, thd->query().str, thd->lex->query_tables->table_name,
        ddl_ctrl->error_msg);
    *ret = (ct_errno_t)0;
  }
}
int ctc_ddl_handle_fault(const char *tag, const THD *thd,
                         const ddl_ctrl_t *ddl_ctrl, ct_errno_t ret,
                         const char *param, int fix_ret) {
  if (ret != CT_SUCCESS) {
    ctc_log_system("[CTC_DDL_RES]:tag ret:%d, msg_len:%u, sql:%s, param:%s, error_message:%s",
                   ret, (uint32_t)(ddl_ctrl->msg_len), thd->query().str, param == nullptr ? "" :
                   param, ddl_ctrl->error_msg);
    RETURN_IF_OOM(ret);
    int32_t error = convert_ctc_error_code_to_mysql(ret);
    if (error != HA_ERR_GENERIC) {
      return error;
    } else if (strlen(ddl_ctrl->error_msg) > 0) {
      ctc_print_cantian_err_msg(ddl_ctrl, ret);
    } else {
      my_error(ER_DISALLOWED_OPERATION, MYF(0), UN_SUPPORT_DDL, thd->query().str);
    }
    if (fix_ret != 0) {
      return fix_ret;
    }
    return error;
  } else {
    ctc_log_system("[CTC_DDL_RES]:%s success, ret: %d, sql:%s", tag, ret, thd->query().str);
    return ret;
  }
}

  /**
    @brief
    create() is called to create a table. The variable name will have the name
    of the table.

    @details
    When create() is called you do not need to worry about
    opening the table. Also, the .frm file will have already been
    created so adjusting create_info is not necessary. You can overwrite
    the .frm file at this point if you wish to change the table
    definition, but there are no methods currently provided for doing
    so.

    Called from handle.cc by ha_create_table().

    @see
    ha_create_table() in handle.cc
  */
  /**
    Create table (implementation).

    @param  [in]      name      Table name.
    @param  [in]      form      TABLE object describing the table to be
                                created.
    @param  [in]      info      HA_CREATE_INFO describing table.
    @param  [in,out]  table_def dd::Table object describing the table
                                to be created. This object can be
                                adjusted by storage engine if it
                                supports atomic DDL (i.e. has
                                HTON_SUPPORTS_ATOMIC_DDL flag set).
                                These changes will be persisted in the
                                data-dictionary. Can be NULL for
                                temporary tables created by optimizer.

    @retval  0      Success.
    @retval  non-0  Error.
  */
EXTER_ATTACK int ha_ctc::create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
                   dd::Table *table_def) {
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  ct_errno_t ret = CT_SUCCESS;
  if (check_unsupported_operation(thd, create_info)) {
    ctc_log_system("Unsupported operation. sql = %s", thd->query().str);
    END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
    return HA_ERR_WRONG_COMMAND;
  }

  /*
    copy algorithm is used when ha_create is called by mysql_alter_table
  */
  bool is_tmp_table = create_info->options & HA_LEX_CREATE_TMP_TABLE || ctc_is_temporary(table_def);
  if (thd->lex->sql_command != SQLCOM_CREATE_TABLE && thd->lex->sql_command != SQLCOM_CREATE_VIEW
      && thd->lex->alter_info) {
    if (is_tmp_table) {
      ctc_log_system("Unsupported operation. sql = %s", thd->query().str);
      END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
      return HA_ERR_NOT_ALLOWED_COMMAND;
    }
    // do not move this under engine_ddl_passthru(thd) function
    thd->lex->alter_info->requested_algorithm = Alter_info::ALTER_TABLE_ALGORITHM_COPY;
  }

  if (engine_skip_ddl(thd) || engine_ddl_passthru(thd)) {
    END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
    return ret;
  }

  char db_name[SMALL_RECORD_SIZE] = {0};
  char table_name[SMALL_RECORD_SIZE] = {0};
  ctc_split_normalized_name(name, db_name, SMALL_RECORD_SIZE, table_name, SMALL_RECORD_SIZE, &is_tmp_table);
  if (!is_tmp_table) {
    CTC_RETURN_IF_NOT_ZERO(check_ctc_identifier_name(table_def->name().c_str()));
    ctc_copy_name(table_name, const_cast<char *>(table_def->name().c_str()), SMALL_RECORD_SIZE);
  }

  if (!(create_info->options & HA_LEX_CREATE_TMP_TABLE)) {
    ctc_register_trx(ht, thd);
  }

  if (thd->lex->sql_command == SQLCOM_TRUNCATE) {
    int ret_status = ha_ctc_truncate_table(&m_tch, thd, db_name, table_name, is_tmp_table);
    END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
    return ret_status;
  }

  if (get_cantian_record_length(form) > CT_MAX_RECORD_LENGTH) {
    END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
    return HA_ERR_TOO_BIG_ROW;
  }

  ctc_ddl_stack_mem stack_mem(0);
  update_member_tch(m_tch, ctc_hton, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, m_tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);

  if (is_alter_table_copy(thd)) {
    ddl_ctrl.is_alter_copy = true;
  }

  uint32_t table_flags = 0;
  if (is_tmp_table) {
    table_flags |= CTC_FLAG_TMP_TABLE;
    if (create_info->options & HA_LEX_CREATE_INTERNAL_TMP_TABLE) {
      table_flags |= CTC_FLAG_INTERNAL_TMP_TABLE;
    }
    ddl_ctrl.table_flags = table_flags;
  }
  ret = (ct_errno_t)fill_create_table_req(create_info, table_def, db_name, table_name, form, thd, &ddl_ctrl, &stack_mem);
  if (ret != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
    return ret;
  }

  void *ctc_ddl_req_msg_mem = stack_mem.get_buf();
  if (ctc_ddl_req_msg_mem == nullptr) {
    END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
    return HA_ERR_OUT_OF_MEM;
  }

  ret = (ct_errno_t)ctc_create_table(ctc_ddl_req_msg_mem, &ddl_ctrl);
  m_tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  if (ret == ERR_FUNCTION_NOT_EXIST) {
    char *err_msg;
    char *field_name = strtok_r(ddl_ctrl.error_msg, ",", &err_msg);
    char *func_name = strtok_r(NULL, ",", &err_msg);
    if (func_name) {
      // func_name非空的情况对应default function
      my_error(ER_DEFAULT_VAL_GENERATED_NAMED_FUNCTION_IS_NOT_ALLOWED, MYF(0),
               field_name, func_name);
      END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
      return CT_ERROR;
    }
  }
  ctc_ddl_hook_cantian_error("ctc_create_table_cantian_error", thd, &ddl_ctrl, &ret);
  END_RECORD_STATS(EVENT_TYPE_CREATE_TABLE)
  return ctc_ddl_handle_fault("ctc_create_table", thd, &ddl_ctrl, ret);
}

/** Implementation of inplace_alter_table()
@tparam		Table		dd::Table or dd::Partition
@param[in]	altered_table	TABLE object for new version of table.
@param[in,out]	ha_alter_info	Structure describing changes to be done
                                by ALTER TABLE and holding data used
                                during in-place alter.
@param[in]	old_dd_tab	dd::Table object describing old version
                                of the table.
@param[in,out]	new_dd_tab	dd::Table object for the new version of the
                                table. Can be adjusted by this call.
                                Changes to the table definition will be
                                persisted in the data-dictionary at statement
                                commit time.
@retval true Failure
@retval false Success
*/
bool ha_ctc::inplace_alter_table(TABLE *altered_table,
                            Alter_inplace_info *ha_alter_info,
                            const dd::Table *old_table_def,
                            dd::Table *new_table_def)
{
  BEGIN_RECORD_STATS
  if (old_table_def == nullptr || new_table_def == nullptr) {
    ctc_log_error(
        "inplace_alter_table old_table_def:%p, or new_table_def:%p is NULL",
        old_table_def, new_table_def);
    END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
    return true;
  }

  THD *thd = ha_thd();
  Alter_info *alter_info = ha_alter_info->alter_info;
  ct_errno_t ret = CT_SUCCESS;

  if (check_unsupported_operation(thd, nullptr)) {
    ctc_log_system("Unsupported operation. sql = %s", thd->query().str);
    END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
    return true;
  }

  if (get_cantian_record_length(altered_table) > CT_MAX_RECORD_LENGTH) {
    END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
    return true;
  }
  /* Nothing to commit/rollback, mark all handlers committed! */
  ha_alter_info->group_commit_ctx = nullptr;

  if (engine_ddl_passthru(thd)) {
      END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
      return false;
  }

  ctc_ddl_stack_mem stack_mem(0);
  update_member_tch(m_tch, ctc_hton, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, m_tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  if (alter_info->flags & Alter_info::ALTER_RECREATE) {
      ret = (ct_errno_t)fill_rebuild_index_req(altered_table, thd, &ddl_ctrl, &stack_mem);
  } else {
      ret = (ct_errno_t)fill_alter_table_req(
          altered_table, ha_alter_info, old_table_def, new_table_def, thd,
          &ddl_ctrl, &stack_mem);
  }
  if (ret != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
    return true;
  }

  void *ctc_ddl_req_msg_mem = stack_mem.get_buf();
  if (ctc_ddl_req_msg_mem == nullptr) {
    END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
    return true;
  }
  ctc_register_trx(ht, thd);
  ret = (ct_errno_t)ctc_alter_table(ctc_ddl_req_msg_mem, &ddl_ctrl);
  ctc_ddl_hook_cantian_error("ctc_alter_table_cantian_error", thd, &ddl_ctrl, &ret);
  m_tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  ctc_ddl_handle_fault("ctc_alter_table", thd, &ddl_ctrl, ret);
  // 这个地方alter table需要特殊处理返回值
  if (ret != CT_SUCCESS) {
    ctc_alter_table_handle_fault(ret);
    END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
    return true;
  }

  END_RECORD_STATS(EVENT_TYPE_INPLACE_ALTER_TABLE)
  return false;
}

/**
  @brief

  Renames a table from one name to another via an alter table call.

  @details
  If you do not implement this, the default rename_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from sql_table.cc by mysql_rename_table().

  @see
  mysql_rename_table() in sql_table.cc
*/
EXTER_ATTACK int ha_ctc::rename_table(const char *from, const char *to,
                         const dd::Table *from_table_def,
                         dd::Table *to_table_def) {
  BEGIN_RECORD_STATS
  THD *thd = ha_thd();
  ct_errno_t ret = CT_SUCCESS;

  if (engine_ddl_passthru(thd)) {
    END_RECORD_STATS(EVENT_TYPE_RENAME_TABLE)
    return false;
  }

  if (is_dd_table_id(to_table_def->se_private_id())) {
    my_error(ER_NOT_ALLOWED_COMMAND, MYF(0));
    END_RECORD_STATS(EVENT_TYPE_RENAME_TABLE)
    return HA_ERR_UNSUPPORTED;
  }

  ctc_ddl_stack_mem stack_mem(0);
  update_member_tch(m_tch, ctc_hton, thd);
  ddl_ctrl_t ddl_ctrl = {{0}, {0}, {0}, 0, 0, m_tch, ctc_instance_id, false, 0};
  FILL_USER_INFO_WITH_THD(ddl_ctrl, thd);
  if (is_alter_table_copy(thd)) {
    ddl_ctrl.is_alter_copy = true;
  }

  ret = (ct_errno_t)fill_rename_table_req(from, to, from_table_def, to_table_def, thd, &ddl_ctrl, &stack_mem);
  if (ret != CT_SUCCESS) {
    END_RECORD_STATS(EVENT_TYPE_RENAME_TABLE)
    return ret;
  }

  void *ctc_ddl_req_msg_mem = stack_mem.get_buf();
  if(ctc_ddl_req_msg_mem == nullptr) {
    END_RECORD_STATS(EVENT_TYPE_RENAME_TABLE)
    return HA_ERR_OUT_OF_MEM;
  }
  ctc_register_trx(ht, thd);
  ret = (ct_errno_t)ctc_rename_table(ctc_ddl_req_msg_mem, &ddl_ctrl);
  ctc_ddl_hook_cantian_error("ctc_rename_table_cantian_error", thd, &ddl_ctrl, &ret);
  m_tch = ddl_ctrl.tch;
  update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
  END_RECORD_STATS(EVENT_TYPE_RENAME_TABLE)
  return ctc_ddl_handle_fault("ctc_rename_table", thd, &ddl_ctrl, ret, to);
}

int ha_ctc::check(THD *, HA_CHECK_OPT *)
{
  return HA_ADMIN_OK;
}

bool ha_ctc::get_error_message(int error, String *buf)
{
  if (error == HA_ERR_ROW_IS_REFERENCED) {
      buf->append(STRING_WITH_LEN("Record is referenced by child tables("));
      for (uint i = 0; i < table->s->foreign_key_parents; i++) {
      buf->append(table->s->foreign_key_parent[i].referencing_table_db);
      buf->append(STRING_WITH_LEN("."));
      buf->append(table->s->foreign_key_parent[i].referencing_table_name);
      if (i != table->s->foreign_key_parents - 1)
        buf->append(STRING_WITH_LEN(", "));
    }
    buf->append(STRING_WITH_LEN(")"));
    return false;
  }
  if (error == HA_ERR_NO_REFERENCED_ROW){
    buf->append(STRING_WITH_LEN("Referenced key value not found in parent tables("));
    for (uint i = 0; i < table->s->foreign_keys; i++) {
      buf->append(table->s->foreign_key[i].referenced_table_db);
      buf->append(STRING_WITH_LEN("."));
      buf->append(table->s->foreign_key[i].referenced_table_name);
      if (i != table->s->foreign_keys - 1)
        buf->append(STRING_WITH_LEN(", "));
    }
    buf->append(STRING_WITH_LEN(")"));
  }
  return false;
}

ctc_select_mode_t ha_ctc::get_select_mode()
{
  /* Set select mode for SKIP LOCKED / NOWAIT */
  if (table->pos_in_table_list == nullptr) {
    return SELECT_ORDINARY;
  }
  ctc_select_mode_t mode;
  switch (table->pos_in_table_list->lock_descriptor().action) {
    case THR_SKIP:
      mode = SELECT_SKIP_LOCKED;
      break;
    case THR_NOWAIT:
      mode = SELECT_NOWAIT;
      break;
    default:
      mode = SELECT_ORDINARY;
      break;
  }
  return mode;
}

int alloc_str_mysql_mem(ctc_cbo_stats_t *cbo_stats, uint32_t part_num, TABLE *table)
{
  uint32_t acc_gcol_num[CTC_MAX_COLUMNS] = {0};
  calc_accumulate_gcol_num(table->s->fields, table->s->field, acc_gcol_num);
  cbo_stats->col_type =(bool *)my_malloc(PSI_NOT_INSTRUMENTED, table->s->fields * sizeof(bool), MYF(MY_WME));
  if (cbo_stats->col_type == nullptr) {
    ctc_log_error("alloc shm mem failed, cbo_stats->col_type(%lu)", table->s->fields * sizeof(bool));
    return ERR_ALLOC_MEMORY;
  }
  memset(cbo_stats->col_type, 0, table->s->fields * sizeof(bool));
  cbo_stats->num_str_cols = 0;
  for (uint i = 0; i < table->s->fields; i++) {
    Field *field = table->field[i];
    if (field->is_virtual_gcol()) {
      continue;
    }
    uint32_t ct_col_id = i - acc_gcol_num[i];
    if (field->real_type() == MYSQL_TYPE_VARCHAR || field->real_type() == MYSQL_TYPE_VAR_STRING ||
        field->real_type() == MYSQL_TYPE_STRING) {
      cbo_stats->col_type[ct_col_id] = true;
      cbo_stats->num_str_cols++;
    }
  }
  uint32_t str_stats_mem_size = part_num * cbo_stats->num_str_cols * (STATS_HISTGRAM_MAX_SIZE + 2) * CBO_STRING_MAX_LEN;
  char *str_stats_mem = (char *)my_malloc(PSI_NOT_INSTRUMENTED, str_stats_mem_size, MYF(MY_WME));
  if (str_stats_mem == nullptr) {
    ctc_log_error("alloc shm mem failed, str_stats_mem size(%u)", str_stats_mem_size);
    return ERR_ALLOC_MEMORY;
  }
  memset(str_stats_mem, 0, str_stats_mem_size);
  for (uint i = 0; i < part_num; i++) {
    for (uint j = 0; j < table->s->fields; j++) {
      Field *field = table->field[j];
      if (field->is_virtual_gcol()) {
        continue;
      }
      uint32_t ct_col_id = j - acc_gcol_num[j];
      if (field->real_type() == MYSQL_TYPE_VARCHAR || field->real_type() == MYSQL_TYPE_VAR_STRING ||
          field->real_type() == MYSQL_TYPE_STRING) {
        cbo_stats->ctc_cbo_stats_table[i].columns[ct_col_id].high_value.v_str = str_stats_mem;
        cbo_stats->ctc_cbo_stats_table[i].columns[ct_col_id].low_value.v_str = str_stats_mem + CBO_STRING_MAX_LEN;
        str_stats_mem = str_stats_mem + CBO_STRING_MAX_LEN * 2;
        for (uint k = 0; k < STATS_HISTGRAM_MAX_SIZE; k++) {
          cbo_stats->ctc_cbo_stats_table[i].columns[ct_col_id].column_hist[k].ep_value.v_str = str_stats_mem;
          str_stats_mem = str_stats_mem + CBO_STRING_MAX_LEN;
        }
      }
    }
  }
  return CT_SUCCESS;
}

int ha_ctc::initialize_cbo_stats()
{
  if (!m_share || m_share->cbo_stats != nullptr) {
    return CT_SUCCESS;
  }
  BEGIN_RECORD_STATS
  m_share->cbo_stats = (ctc_cbo_stats_t*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ctc_cbo_stats_t), MYF(MY_WME));
  if (m_share->cbo_stats == nullptr) {
    ctc_log_error("alloc mem failed, m_share->cbo_stats size(%lu)", sizeof(ctc_cbo_stats_t));
    END_RECORD_STATS(EVENT_TYPE_INITIALIZE_DBO)
    return ERR_ALLOC_MEMORY;
  }
  *m_share->cbo_stats = {0, 0, 0, 0, 0, nullptr, 0, nullptr, nullptr};
  m_share->cbo_stats->ctc_cbo_stats_table =
        (ctc_cbo_stats_table_t*)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ctc_cbo_stats_table_t), MYF(MY_WME));
  if (m_share->cbo_stats->ctc_cbo_stats_table == nullptr) {
    ctc_log_error("alloc mem failed, m_share->cbo_stats->ctc_cbo_stats_table(%lu)", sizeof(ctc_cbo_stats_table_t));
    END_RECORD_STATS(EVENT_TYPE_INITIALIZE_DBO)
    return ERR_ALLOC_MEMORY;
  }
  memset(m_share->cbo_stats->ctc_cbo_stats_table, 0, sizeof(ctc_cbo_stats_table_t));

  m_share->cbo_stats->ctc_cbo_stats_table->columns =
    (ctc_cbo_stats_column_t*)my_malloc(PSI_NOT_INSTRUMENTED, table->s->fields * sizeof(ctc_cbo_stats_column_t), MYF(MY_WME));
  if (m_share->cbo_stats->ctc_cbo_stats_table->columns == nullptr) {
    ctc_log_error("alloc mem failed, m_share->cbo_stats->ctc_cbo_stats_table->columns size(%lu)", table->s->fields * sizeof(ctc_cbo_stats_column_t));
    END_RECORD_STATS(EVENT_TYPE_INITIALIZE_DBO)
    return ERR_ALLOC_MEMORY;
  }
  memset(m_share->cbo_stats->ctc_cbo_stats_table->columns, 0, table->s->fields * sizeof(ctc_cbo_stats_column_t));

  ct_errno_t ret = (ct_errno_t)alloc_str_mysql_mem(m_share->cbo_stats, 1, table);
  if (ret != CT_SUCCESS) {
    ctc_log_error("m_share:ctc alloc str mysql mem failed, ret:%d", ret);
  }

  m_share->cbo_stats->ndv_keys =
    (uint32_t*)my_malloc(PSI_NOT_INSTRUMENTED, table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS, MYF(MY_WME));
  if (m_share->cbo_stats->ndv_keys == nullptr) {
    ctc_log_error("alloc mem failed, m_share->cbo_stats->ndv_keys size(%lu)", table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS);
    END_RECORD_STATS(EVENT_TYPE_INITIALIZE_DBO)
    return ERR_ALLOC_MEMORY;
  }
  memset(m_share->cbo_stats->ndv_keys, 0, table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS);
  
  m_share->cbo_stats->msg_len = table->s->fields * sizeof(ctc_cbo_stats_column_t);
  m_share->cbo_stats->key_len = table->s->keys * sizeof(uint32_t) * MAX_KEY_COLUMNS;
  END_RECORD_STATS(EVENT_TYPE_INITIALIZE_DBO)
  return CT_SUCCESS;
}

int ha_ctc::get_cbo_stats_4share()
{
  THD *thd = ha_thd();
  int ret = CT_SUCCESS;
  time_t now = time(nullptr);
  if (m_share && (m_share->need_fetch_cbo || now - m_share->get_cbo_time > ctc_update_analyze_time)) {
    if (m_tch.ctx_addr == INVALID_VALUE64) {
      char user_name[SMALL_RECORD_SIZE] = { 0 };
      ctc_split_normalized_name(table->s->normalized_path.str, user_name, SMALL_RECORD_SIZE, nullptr, 0, nullptr);
      ctc_copy_name(user_name, user_name, SMALL_RECORD_SIZE);
      update_member_tch(m_tch, ctc_hton, thd);
      ret = ctc_open_table(&m_tch, table->s->table_name.str, user_name);
      update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
      if (ret != CT_SUCCESS) {
        return ret;
      }
    }
    update_member_tch(m_tch, ctc_hton, thd);
    ret = ctc_get_cbo_stats(&m_tch, m_share->cbo_stats, m_share->cbo_stats->ctc_cbo_stats_table, 0, 0);
    update_sess_ctx_by_tch(m_tch, ctc_hton, thd);
    if (ret == CT_SUCCESS && m_share->cbo_stats->is_updated) {
      m_share->need_fetch_cbo = false;
      ctc_index_stats_update(table, m_share->cbo_stats);
    }
    m_share->get_cbo_time = now;
  }

  return ret;
}

void free_columns_cbo_stats(ctc_cbo_stats_column_t *ctc_cbo_stats_columns, bool *is_str_first_addr, TABLE *table)
{
  uint32_t acc_gcol_num[CTC_MAX_COLUMNS] = {0};
  calc_accumulate_gcol_num(table->s->fields, table->s->field, acc_gcol_num);
  for (uint j = 0; j < table->s->fields; j++) {
    Field *field = table->field[j];
    uint32_t ct_col_id = j - acc_gcol_num[j];
    if (field->is_virtual_gcol()) {
      continue;
    }
    if (field->real_type() == MYSQL_TYPE_VARCHAR || field->real_type() == MYSQL_TYPE_VAR_STRING ||
        field->real_type() == MYSQL_TYPE_STRING) {
      if (*is_str_first_addr) {
        my_free(ctc_cbo_stats_columns[ct_col_id].high_value.v_str);
        *is_str_first_addr = false;
      }
      ctc_cbo_stats_columns[ct_col_id].high_value.v_str = nullptr;
      ctc_cbo_stats_columns[ct_col_id].low_value.v_str = nullptr;
      for (uint k = 0; k < STATS_HISTGRAM_MAX_SIZE; k++) {
        ctc_cbo_stats_columns[ct_col_id].column_hist[k].ep_value.v_str = nullptr;
      }
    }
  }
  my_free(ctc_cbo_stats_columns);
  ctc_cbo_stats_columns = nullptr;
}

void ha_ctc::free_cbo_stats()
{
  if (!m_share || m_share->cbo_stats == nullptr) {
    return;
  }
  BEGIN_RECORD_STATS
  my_free((m_share->cbo_stats->ndv_keys));
  m_share->cbo_stats->ndv_keys = nullptr;
  my_free((m_share->cbo_stats->col_type));
  m_share->cbo_stats->col_type = nullptr;

  bool is_str_first_addr = true;
  free_columns_cbo_stats(m_share->cbo_stats->ctc_cbo_stats_table->columns, &is_str_first_addr, table);

  my_free(m_share->cbo_stats->ctc_cbo_stats_table);
  m_share->cbo_stats->ctc_cbo_stats_table = nullptr;
  my_free((uchar *)(m_share->cbo_stats));
  m_share->cbo_stats = nullptr;
  END_RECORD_STATS(EVENT_TYPE_FREE_CBO)
}

/**
  Condition pushdown for update/delete
  @param cond          Condition to be pushed down.
  @param other_tbls_ok Are other tables allowed to be referred
                       from the condition terms pushed down.

  @retval Return the 'remainder' condition, consisting of the AND'ed
          sum of boolean terms which could not be pushed. A nullptr
          is returned if entire condition was supported.
*/
#ifdef FEATURE_X_FOR_MYSQL_32
const Item *ha_ctc::cond_push(const Item *cond) {
#elif defined(FEATURE_X_FOR_MYSQL_26)
const Item *ha_ctc::cond_push(const Item *cond, bool other_tbls_ok MY_ATTRIBUTE((unused))) {
#endif
  assert(m_cond == nullptr);
  assert(pushed_cond == nullptr);
  assert(cond != nullptr);
  const Item *remainder = cond;

  THD *const thd = table->in_use;
  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    return remainder;
  }
  if (thd->lex->all_query_blocks_list && thd->lex->all_query_blocks_list->is_recursive()) {
    return remainder;
  }

  prep_cond_push(cond);
  if (m_pushed_conds == nullptr) {
    return remainder;
  }

  m_cond = (ctc_conds *)ctc_alloc_buf(&m_tch, sizeof(ctc_conds));
  if (m_cond == nullptr) {
    ctc_log_error("alloc mem failed, m_cond size(%lu), pushdown cond is null.",  sizeof(ctc_conds));
    return remainder;
  }

  bool no_backslash = false;
  if (thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) {
    no_backslash = true;
  }
  Field **field = table->field;
  if (ctc_fill_conds(m_tch, m_pushed_conds, field, m_cond, no_backslash) != CT_SUCCESS) {
    free_m_cond(m_tch, m_cond);
    m_cond = nullptr;
    m_pushed_conds = nullptr;
    m_remainder_conds = nullptr;
    return remainder;
  }

  pushed_cond = m_pushed_conds;
  m_remainder_conds = const_cast<Item *>(cond);

  return remainder;
}

/**
  Condition pushdown
  Push a condition to ctc storage engine for evaluation
  during table and index scans. The conditions will be cleared
  by calling handler::extra(HA_EXTRA_RESET) or handler::reset().

  The current implementation supports arbitrary AND/OR nested conditions
  with comparisons between columns and constants (including constant
  expressions and function calls) and the following comparison operators:
  =, !=, >, >=, <, <=, "is null", and "is not null".

  If the condition consist of multiple AND/OR'ed 'boolean terms',
  parts of it may be pushed, and other parts will be returned as a
  'remainder condition', which the server has to evaluate.

  handler::pushed_cond will be assigned the (part of) the condition
  which we accepted to be pushed down.

  Note that this handler call has been partly deprecated by
  ::engine_push() which does both join- and condition pushdown.
  The only remaining intended usage for ::cond_push() is simple
  update and delete queries, where the join part is not relevant.
 * @param table_aqp The specific table in the join plan to examine.
 * @return Possible error code, '0' if no errors.
 */
#ifdef FEATURE_X_FOR_MYSQL_26
int ha_ctc::engine_push(AQP::Table_access *table_aqp)
{
  DBUG_TRACE;
  const Item *cond = table_aqp->get_condition();
  assert(m_cond == nullptr);

  THD *const thd = table->in_use;
  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    return 0;
  }
  
  if (thd->lex->all_query_blocks_list && thd->lex->all_query_blocks_list->is_recursive()) {
    return 0;
  }

  if (cond == nullptr) {
    return 0;
  }
  // Filter Multi-Table Queries
  const AQP::Join_plan *const plan = table_aqp->get_join_plan();
  if (plan->get_access_count() > 1) {
    return 0;
  }

  prep_cond_push(cond);
  if (m_pushed_conds == nullptr) {
    return 0;
  }

  m_cond = (ctc_conds *)ctc_alloc_buf(&m_tch, sizeof(ctc_conds));
  if (m_cond == nullptr) {
    ctc_log_error("alloc mem failed, m_cond size(%lu), pushdown cond is null.",  sizeof(ctc_conds));
    return 0;
  }

  bool no_backslash = false;
  if (thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) {
    no_backslash = true;
  }
  Field **field = table_aqp->get_table()->field;
  if (ctc_fill_conds(m_tch, m_pushed_conds, field, m_cond, no_backslash) != CT_SUCCESS) {
    free_m_cond(m_tch, m_cond);
    m_cond = nullptr;
    m_pushed_conds = nullptr;
    m_remainder_conds = nullptr;
    return 0;
  }

  pushed_cond = m_pushed_conds;
  m_remainder_conds = const_cast<Item *>(cond);
  table_aqp->set_condition(const_cast<Item *>(m_remainder_conds));
  return 0;
}
#endif


#ifdef FEATURE_X_FOR_MYSQL_32
TABLE *get_base_table(const AccessPath *path) {
  switch (path->type) {
    // Basic access paths (those with no children, at least nominally).
    case AccessPath::TABLE_SCAN:
      return path->table_scan().table;
    case AccessPath::INDEX_SCAN:
      return path->index_scan().table;
    case AccessPath::REF:
      return path->ref().table;
    case AccessPath::REF_OR_NULL:
      return path->ref_or_null().table;
    case AccessPath::EQ_REF:
      return path->eq_ref().table;
    case AccessPath::PUSHED_JOIN_REF:
      return path->pushed_join_ref().table;
    case AccessPath::FULL_TEXT_SEARCH:
      return path->full_text_search().table;
    case AccessPath::CONST_TABLE:
      return path->const_table().table;
    case AccessPath::MRR:
      return path->mrr().table;
    case AccessPath::FOLLOW_TAIL:
      return path->follow_tail().table;
    case AccessPath::INDEX_RANGE_SCAN:
      return path->index_range_scan().used_key_part[0].field->table;
    case AccessPath::INDEX_MERGE:
      return path->index_merge().table;
    case AccessPath::ROWID_INTERSECTION:
      return path->rowid_intersection().table;
    case AccessPath::ROWID_UNION:
      return path->rowid_union().table;
    case AccessPath::INDEX_SKIP_SCAN:
      return path->index_skip_scan().table;
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      return path->group_index_skip_scan().table;
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      return path->dynamic_index_range_scan().table;
    default:
      return nullptr;
  }
}

TABLE *ctc_get_basic_table(const AccessPath *path) {
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::FULL_TEXT_SEARCH:
    case AccessPath::CONST_TABLE:
    case AccessPath::MRR:
    case AccessPath::FOLLOW_TAIL:
    case AccessPath::INDEX_RANGE_SCAN:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
    case AccessPath::INDEX_MERGE:
      return get_base_table(path);
    case AccessPath::FILTER:
      return ctc_get_basic_table(path->filter().child);
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::MATERIALIZED_TABLE_FUNCTION:
    case AccessPath::UNQUALIFIED_COUNT:
    case AccessPath::NESTED_LOOP_JOIN:
    case AccessPath::BKA_JOIN:
    case AccessPath::HASH_JOIN:
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
    case AccessPath::SORT:
    case AccessPath::LIMIT_OFFSET:
    case AccessPath::AGGREGATE:
    case AccessPath::TEMPTABLE_AGGREGATE:
    case AccessPath::STREAM:
    case AccessPath::MATERIALIZE:
    case AccessPath::MATERIALIZE_INFORMATION_SCHEMA_TABLE:
    case AccessPath::APPEND:
    case AccessPath::WINDOW:
    case AccessPath::WEEDOUT:
    case AccessPath::REMOVE_DUPLICATES_ON_INDEX:
    case AccessPath::REMOVE_DUPLICATES:
    case AccessPath::ALTERNATIVE:
    case AccessPath::CACHE_INVALIDATOR:
    case AccessPath::DELETE_ROWS:
    case AccessPath::UPDATE_ROWS:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
      break;
    default:
      break;
  }
  return nullptr;
}

/**
 * Try to find parts of queries which can be pushed down to
 * storage engines for faster execution. This is typically
 * conditions which can filter out result rows on the SE,
 * and/or entire joins between tables.
 *
 * @param  thd         Thread context
 * @param  root_path   The AccessPath for the entire query.
 * @param  join        The JOIN struct built for the main query.
 *
 * @return Possible ret code, '0' if no errors.
 */
int ctc_push_to_engine(THD *thd, AccessPath *root_path, JOIN *) {
  DBUG_TRACE;

  if (!thd->optimizer_switch_flag(OPTIMIZER_SWITCH_ENGINE_CONDITION_PUSHDOWN)) {
    return 0;
  }
  
  if (thd->lex->all_query_blocks_list && thd->lex->all_query_blocks_list->is_recursive()) {
    return 0;
  }

  TABLE *table = ctc_get_basic_table(root_path);
  if (table == nullptr) {
    return 0;
  }
  
  const Item *cond = root_path->type == AccessPath::FILTER ? root_path->filter().condition : nullptr;
  if (cond == nullptr) {
        return 0;
  }

  ha_ctc *const ctc_handler = dynamic_cast<ha_ctc *>(table->file);
  if (ctc_handler == nullptr) {
    ctc_log_warning("[ctc_push_to_engine] ctc_handler is nullptr.");
    return 0;
  }

  ctc_handler->prep_cond_push(cond);
  if (ctc_handler->m_pushed_conds == nullptr) {
    return 0;
  }

  ctc_handler_t *tch = ctc_handler->get_m_tch();
  ctc_handler->m_cond = (ctc_conds *)ctc_alloc_buf(tch, sizeof(ctc_conds));
  if (ctc_handler->m_cond == nullptr) {
    ctc_log_warning("[ctc_push_to_engine] alloc mem failed, m_cond size(%lu), pushdown cond is null.",
                   sizeof(ctc_conds));
    return 0;
  }

  bool no_backslash = false;
  if (thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) {
    no_backslash = true;
  }
  Field **field = table->field;
  if (ctc_fill_conds(*tch, ctc_handler->m_pushed_conds, field, ctc_handler->m_cond, no_backslash) != CT_SUCCESS) {
    free_m_cond(*tch, ctc_handler->m_cond);
    ctc_handler->m_cond = nullptr;
    ctc_handler->m_pushed_conds = nullptr;
    ctc_handler->m_remainder_conds = nullptr;
    ctc_log_warning("[ctc_push_to_engine] ctc_fill_conds failed.");
    return 0;
  }

  ctc_handler->pushed_cond = ctc_handler->m_pushed_conds;
  ctc_handler->m_remainder_conds = const_cast<Item *>(cond);

  return 0;
}

const handlerton *ha_ctc::hton_supporting_engine_pushdown() {
  return ctc_hton;
}
#endif