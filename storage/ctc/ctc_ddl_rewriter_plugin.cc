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

#include <string>
#include <string_view>
#include <functional>
#include <ctype.h>
#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>
#include <mysql/psi/mysql_memory.h>
#include <mysql/service_mysql_alloc.h>
#include <regex>
#include <unordered_map>
#include <algorithm>
#include "my_inttypes.h"
#include "my_psi_config.h"
#include "my_thread.h"  // my_thread_handle needed by mysql_memory.h
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_error.h"
#include "ctc_log.h"
#include "ctc_srv.h"
#include "ctc_util.h"
#include "ctc_proxy_util.h"
#include "ctc_error.h"
#include "ha_ctc.h"
#include "ha_ctc_ddl.h"
#include "sql/sql_initialize.h"  // opt_initialize_insecure
#include "sql/sql_list.h"
#include "sql/set_var.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/lock.h"
#include "sql/auth/auth_common.h"
#include <queue>
#include <mutex>
#include "sql/sql_tablespace.h"
#include "sql/sql_lex.h"
#include "sql/sql_db.h"  // check_schema_readonly
#include "sql/sql_backup_lock.h"
#include "mysql/plugin_auth.h"
#include "sql/auth/sql_auth_cache.h"
#include "sql/auth/auth_internal.h"
#include "sql/sql_parse.h"
#ifdef FEATURE_X_FOR_MYSQL_32
#include "sql/sys_vars_shared.h"  // intern_find_sys_var
#endif

using namespace std;

static SYS_VAR *ctc_rewriter_system_variables[] = {
  nullptr
};

extern uint32_t ctc_instance_id;

static bool is_current_system_var(set_var *setvar) {
  Item_func_get_system_var *itemFunc = dynamic_cast<Item_func_get_system_var *>(setvar->value);
  if (setvar->value == nullptr || itemFunc) {
    return true;
  }
  return false;
}

typedef int (*check_variable_fn)(set_var *setvar, bool &need_forward, string user_val_str);

int check_default_engine(set_var *setvar, bool &need_forward MY_ATTRIBUTE((unused)), string user_val_str) {
  if (is_current_system_var(setvar)) {
      return 0;
  }

  if (setvar->value->item_name.ptr() == nullptr) {
    if (user_val_str == "") {
      return 0;
    }

    transform(user_val_str.begin(), user_val_str.end(), user_val_str.begin(), ::tolower);
    if (user_val_str == "ctc" || user_val_str == "default") {
      return 0;
    }
    
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
      "Once the CTC is loaded, it must be set as the default engine. To modify the setting, uninstall the CTC first.");
    return -1;
  }

  if (strcasecmp(setvar->value->item_name.ptr(), ctc_hton_name) != 0) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
      "Once the CTC is loaded, it must be set as the default engine. To modify the setting, uninstall the CTC first.");
    return -1;
  }
  return 0;
}

int check_session_pool_volume(set_var *setvar, bool &need_forward MY_ATTRIBUTE((unused)), string user_val_str) {
    if (is_current_system_var(setvar)) {
      return 0;
    }

    uint max_sessions;
    if (ctc_get_max_sessions_per_node(&max_sessions)) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "Get max connections in Cantian failed");
      return -1;
    }

    if (setvar->value->item_name.ptr() == nullptr) {
      if (user_val_str == "") {
        return 0;
      }

      if (!isdigit(*user_val_str.c_str())) {
        my_printf_error(ER_DISALLOWED_OPERATION, "[CTC]:Please make sure value is digits.", MYF(0));
        return -1;
      }

      int tmp_max_connection = atoi(user_val_str.c_str());
      if (tmp_max_connection > (int)max_sessions) {
        my_printf_error(ER_DISALLOWED_OPERATION, "Current SE can only provide %d connections for one mysql-server", MYF(0), max_sessions);
        return -1;
      } else if  (tmp_max_connection < 1) {
        my_printf_error(ER_DISALLOWED_OPERATION, "Current SE cannot provide less than one connection.", MYF(0));
        return -1;
      }
      return 0;
    }

    int num_max_conns = atoi(setvar->value->item_name.ptr());
    if (num_max_conns > (int)max_sessions) {
      my_printf_error(ER_DISALLOWED_OPERATION, "Current SE can only provide %d connections for one mysql-server", MYF(0), max_sessions);
      return -1;
    } else if  (num_max_conns < 1) {
      my_printf_error(ER_DISALLOWED_OPERATION, "Current SE cannot provide less than one connection.", MYF(0));
      return -1;
    }
    return 0;
}

int not_allow_modify(set_var *setvar, bool &need_forward MY_ATTRIBUTE((unused)),
                     string user_val_str MY_ATTRIBUTE((unused))) {
  if (is_current_system_var(setvar)) {
    return 0;
  }

  my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "CTC doesn't support modifying the variable");
  return -1;
}

/*
  参考 Sys_var_transaction_isolation::session_update 和 Sys_var_typelib::do_check().
  transaction_isolation 只支持设置为READ_COMMITTED
*/
int unsupport_tx_isolation_level(set_var *setvar, bool &need_forward MY_ATTRIBUTE((unused)), string user_val_str) {
  if (is_current_system_var(setvar)) {
    return 0;
  }
  
  if (setvar->value->result_type() == STRING_RESULT) {
    // 对应 SET @@global.transaction_isolation = @global_start_value;的写法
    if (setvar->value->item_name.ptr() == nullptr) {
      transform(user_val_str.begin(), user_val_str.end(), user_val_str.begin(), ::tolower);
      if (user_val_str == "read-committed" || user_val_str == "1") {
        return 0;
      } else if (user_val_str == "repeatable-read" || user_val_str == "2") {
        push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_DISALLOWED_OPERATION,
                            "CTC: The Function of REPEATABLE READ transaction isolation is in progress.");
        return 0;
      }

      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
        "CTC STORAGE ENGINE ONLY SUPPORT READ_COMMITTED TRANSACTION ISOLATION LEVEL.");
      return -1;
    }

    // 对应set transaction_isolation='read-committed' 写法
    if (strcasecmp(setvar->value->item_name.ptr(), "read-committed") == 0) {
      return 0;
    } else if (strcasecmp(setvar->value->item_name.ptr(), "repeatable-read") == 0) {
      push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_DISALLOWED_OPERATION,
                        "CTC: The Function of REPEATABLE READ transaction isolation is in progress.");
      return 0;
    }
  } else {
    // 对应SET TRANSACTION ISOLATION LEVEL READ COMMITTED 写法
    enum_tx_isolation tx_isol = (enum_tx_isolation)setvar->value->val_int();
    if (tx_isol == ISO_READ_COMMITTED) {
      return 0;
    } else if (tx_isol == ISO_REPEATABLE_READ) {
      push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_DISALLOWED_OPERATION,
                        "CTC: The Function of REPEATABLE READ transaction isolation is in progress.");
      return 0;
    }
  }

  my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
      "CTC STORAGE ENGINE ONLY SUPPORT READ_COMMITTED TRANSACTION ISOLATION LEVEL.");
  return -1;
}

int ctc_check_opt_forward(set_var *setvar MY_ATTRIBUTE((unused)), bool &need_forward,
  string user_val_str MY_ATTRIBUTE((unused))) {
  need_forward = false;
  push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_DISALLOWED_OPERATION,
                        "CTC: This parameter will not be broadcast to other nodes.");
  return 0;
}

static std::unordered_map<std::string, check_variable_fn> set_variable_rules_map = {
  {"default_storage_engine",            check_default_engine},
  {"max_connections",                   check_session_pool_volume},
  {"transaction_isolation",             unsupport_tx_isolation_level},
  {"read_only",                         ctc_check_opt_forward},
  {"super_read_only",                   ctc_check_opt_forward},
  {"offline_mode",                      ctc_check_opt_forward},
  {"gtid_next",                         ctc_check_opt_forward}
};

static int ctc_get_user_var_string(MYSQL_THD thd, Item_func_get_user_var *itemFunc, string &user_val_str) {
  mysql_mutex_lock(&thd->LOCK_thd_data);

  String str;
  user_var_entry *var_entry;
  var_entry = find_or_nullptr(thd->user_vars, itemFunc->name.ptr());
  if (var_entry == nullptr) {
    ctc_log_system("user var:%s have no value. no need to broadcast.", itemFunc->name.ptr());
    my_printf_error(ER_DISALLOWED_OPERATION, "[CTC]:Please make sure %s has value in it.", MYF(0), itemFunc->name.ptr());
    mysql_mutex_unlock(&thd->LOCK_thd_data);
    return -1;
  }
  
  bool is_var_null;
  String *var_value = var_entry->val_str(&is_var_null, &str, DECIMAL_NOT_SPECIFIED);

  mysql_mutex_unlock(&thd->LOCK_thd_data);

  if (!is_var_null) {
    user_val_str = string(var_value->c_ptr_safe());
  }

  return 0;
}

static int allow_sqlcmd(MYSQL_THD thd, string session_var_name) {
  String str;
  user_var_entry *var_entry = find_or_nullptr(thd->user_vars, session_var_name);
  if(var_entry == nullptr || var_entry->ptr() == nullptr) {
    return 0;
  }
  bool is_var_null;
  longlong var_value = var_entry->val_int(&is_var_null);
  String *var_value_str = var_entry->val_str(&is_var_null, &str, DECIMAL_NOT_SPECIFIED);
  string var_str = var_value_str->c_ptr_safe();
  transform(var_str.begin(), var_str.end(), var_str.begin(), ::tolower);
  if (!is_var_null && (var_value == 1L || (var_str == "true"))) {
    return -1;
  }
  return 0;
}

static int ctc_check_dcl(string &, MYSQL_THD thd, bool &need_forward) {
  if (check_readonly(thd, false) ||
      (thd->lex->query_tables != nullptr &&
       check_schema_readonly(thd, thd->lex->query_tables->table_name))) {
    need_forward = false;
  }
  if (allow_sqlcmd(thd, "ctc_dcl_disabled") != 0) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "DCL query is not allowed (ctc_dcl_disabled = true)");
    return -1;
  }
  return 0;
}

// reference for 'validate_password_require_current' function
int ctc_verify_password4existed_user(MYSQL_THD thd, const LEX_USER *existed_user, bool &res) {
  ACL_USER *acl_user = nullptr;
  plugin_ref plugin = nullptr;
  int is_error = 0;
  Acl_cache_lock_guard acl_cache_lock(thd, Acl_cache_lock_mode::WRITE_MODE);
  if (!acl_cache_lock.lock()) {
    ctc_log_error("ctc_verify_password failed, lock acl cache failed");
    return -1;
  }
  acl_user = find_acl_user(existed_user->host.str, existed_user->user.str, true);
  if (!acl_user) {
    ctc_log_error("ctc_verify_password failed, find acl user failed");
    return -1;
  }
  plugin = my_plugin_lock_by_name(nullptr, acl_user->plugin, MYSQL_AUTHENTICATION_PLUGIN);
  if (!plugin) {
    ctc_log_error("ctc_verify_password failed, lock plugin %s failed", acl_user->plugin.str ? acl_user->plugin.str : "");
    return -1;
  }
  st_mysql_auth *auth = (st_mysql_auth *)plugin_decl(plugin)->info;

  if (acl_user->credentials[PRIMARY_CRED].m_auth_string.length == 0 && existed_user->current_auth.length > 0) {
    res = false;
  } else if ((auth->authentication_flags & AUTH_FLAG_USES_INTERNAL_STORAGE) && auth->compare_password_with_hash &&
              auth->compare_password_with_hash(acl_user->credentials[PRIMARY_CRED].m_auth_string.str,
              (unsigned long)acl_user->credentials[PRIMARY_CRED].m_auth_string.length, existed_user->current_auth.str,
              (unsigned long)existed_user->current_auth.length, &is_error) && !is_error) {
    res = false;
  }
  res = true;
  plugin_unlock(nullptr, plugin);
  return 0;
}

void ctc_remove_replace_clause4sql(string &sql_str) {
  // match: replace "xxx" | replace 'xxx'
  regex replace_pattern(" \\s*replace \\s*(\".*\"|'.*')", std::regex_constants::icase);
  sql_str = regex_replace(sql_str, replace_pattern, " ");
}

/*
  to adapt CTC broadcast:
    1. alter user current_user() -> alter user 'user'@'host'
    2. alter user current user && replace 'old password', we need check the old password to remove the 'replace clause'
*/
static int ctc_rewrite_alter_user4update_passwd(MYSQL_THD thd, string &sql_str) {
  List_iterator<LEX_USER> user_list(thd->lex->users_list);
  LEX_USER *tmp_user;
  LEX_USER *user;
  bool existed_other_user_with_replace = false;
  string rw_query_sql = sql_str;
  Security_context *sctx = thd->security_context();
  while ((tmp_user = user_list++)) {
    /* If it is an empty lex_user update it with current user */
    if (!tmp_user->host.str && !tmp_user->user.str) {
      assert(sctx->priv_host().str);
      tmp_user->host.str = sctx->priv_host().str;
      tmp_user->host.length = strlen(sctx->priv_host().str);
      assert(sctx->user().str);
      tmp_user->user.str = sctx->user().str;
      tmp_user->user.length = strlen(sctx->user().str);
    }
    user = get_current_user(thd, tmp_user);
    bool is_self = !strcmp(sctx->user().length ? sctx->user().str : "", user->user.str) &&
                   !my_strcasecmp(&my_charset_latin1, user->host.str, sctx->priv_host().str);
    if (user->uses_replace_clause) {
      if (is_self) {
        bool is_password_matched = false;
        if (ctc_verify_password4existed_user(thd, user, is_password_matched)) {
          return -1;
        }
        if (!is_password_matched) {
          my_error(ER_INCORRECT_CURRENT_PASSWORD, MYF(0));
          return -1;
        }
      } else {
        existed_other_user_with_replace = true;
      }
    }
  }
  if (!existed_other_user_with_replace) {
    ctc_remove_replace_clause4sql(rw_query_sql);
  }
  regex current_user_pattern(" \\s*current_user[(][)] ", regex_constants::icase);
  string current_user_name(sctx->user().str);
  current_user_name = ctc_deserilize_username_with_single_quotation(current_user_name);
  string user2host("");
  user2host = " '" + current_user_name + "'@'" + string(sctx->priv_host().str) + "'";
  rw_query_sql = regex_replace(rw_query_sql, current_user_pattern, user2host.c_str());
  sql_str = rw_query_sql;
  return 0;
}

static int ctc_check_alter_user(string &sql_str, MYSQL_THD thd, bool &need_forward) {
  if (ctc_check_dcl(sql_str, thd, need_forward) != 0) {
    return -1;
  }

  return ctc_rewrite_alter_user4update_passwd(thd, sql_str);
}

static int ctc_rewrite_setpasswd(MYSQL_THD thd, string &sql_str) {
  // match: set password = | set password to | set password for current_user()，but 'to' and '=' dont match for replacing
  regex add_or_rewrite_for_pattern("^set \\s*password\\s*((?=to|=)|for \\s*current_user[(][)])", regex_constants::icase);
  string rw_query_sql = sql_str;
  string user2host("");

  List<set_var_base> *lex_var_list = &thd->lex->var_list;
  assert(lex_var_list->elements == 1);
  List_iterator_fast<set_var_base> it(*lex_var_list);
  set_var_base *var;
  while ((var = it++)) {
    set_var_password *set_passwd = static_cast<set_var_password *>(var);
    const LEX_USER *user_for_setpasswd = set_passwd->get_user();
    string username(user_for_setpasswd->user.str);
    username = ctc_deserilize_username_with_single_quotation(username);
    user2host = "SET PASSWORD FOR '" + username + "'@'" + string(user_for_setpasswd->host.str) + "' ";
    rw_query_sql = regex_replace(rw_query_sql, add_or_rewrite_for_pattern, user2host.c_str());

    // 为当前用户设置密码，为不报错不加replace
    if (user_for_setpasswd->uses_replace_clause && 
        !strcmp(thd->m_main_security_ctx.priv_user().str, user_for_setpasswd->user.str)) {
      // check replacing old password is correct or not 
      bool is_password_matched = false;
      if (ctc_verify_password4existed_user(thd, user_for_setpasswd, is_password_matched)) {
        return -1;
      }
      if (is_password_matched) {
        ctc_remove_replace_clause4sql(rw_query_sql);
      } else {
        my_error(ER_INCORRECT_CURRENT_PASSWORD, MYF(0));
        return -1;
      }
    }
  }
  sql_str = rw_query_sql;
  return 0;
}

static int ctc_check_set_password(SENSI_INFO string &sql_str, MYSQL_THD thd, bool &need_forward) {
  if (ctc_check_dcl(sql_str, thd, need_forward) != 0) {
    return -1;
  }

  return ctc_rewrite_setpasswd(thd, sql_str);
}

static int ctc_check_flush(string &, MYSQL_THD thd, bool &need_forward) {
  need_forward = thd->lex->type & (REFRESH_FOR_EXPORT | REFRESH_READ_LOCK | REFRESH_GRANT);
  return 0;
}

static uint32_t ctc_set_var_option(bool is_null_value, bool is_set_default_value,
                                   set_var *setvar) {
  uint32_t options = 0;
  if (is_null_value) {
    options |= CTC_SET_VARIABLE_TO_NULL;
  }
  if (is_set_default_value) {
    options |= CTC_SET_VARIABLE_TO_DEFAULT;
  }
  if (setvar->type == OPT_PERSIST_ONLY) {
    options |= CTC_SET_VARIABLE_PERSIST_ONLY;
  }
  if (setvar->type == OPT_PERSIST) {
    options |= CTC_SET_VARIABLE_PERSIST;
  }
  return options;
}

static int ctc_set_var_meta(MYSQL_THD thd, uint32_t options, const char* base_name,
                            string var_name, string var_value, bool var_real_type) {
  ctc_handler_t tch;
  tch.inst_id = ctc_instance_id;
  handlerton* hton = get_ctc_hton();

  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));

  ctc_ddl_broadcast_request broadcast_req {{0}, {0}, {0}, {0}, 0, 0, 0, 0, {0}};
  broadcast_req.options |= CTC_NOT_NEED_CANTIAN_EXECUTE;
  broadcast_req.options |= (thd->lex->contains_plaintext_password ? CTC_CURRENT_SQL_CONTAIN_PLAINTEXT_PASSWORD : 0);
  string sql = string(thd->query().str).substr(0, thd->query().length);
  if (var_real_type) {
    // actual value of the variable type int
    broadcast_req.user_ip[0] |= 1;
  }
  // user_name存变量名，user_ip存变量值
  FILL_BROADCAST_BASE_REQ(broadcast_req, var_value.c_str(), var_name.c_str(),
                          broadcast_req.user_ip, ctc_instance_id, SQLCOM_SET_OPTION);
  if(base_name != nullptr) {
    strncpy(broadcast_req.db_name, base_name, SMALL_RECORD_SIZE - 1);
  }
  broadcast_req.options |= options;
  int ret = ctc_execute_mysql_ddl_sql(&tch, &broadcast_req, true);
  update_sess_ctx_by_tch(tch, hton, thd);
  return ret;
}

static int ctc_get_variables_value_string(MYSQL_THD thd, string &sql_str, set_var* setvar, string& val_str,
                                          bool& is_null_value, bool &need_forward) {
  Item_func_get_user_var *itemFunc = dynamic_cast<Item_func_get_user_var *>(setvar->value);
  Item_func_get_system_var *itemFuncSys = dynamic_cast<Item_func_get_system_var *>(setvar->value);

  if (setvar->value->fix_fields(thd, &(setvar->value))) {
    thd->clear_error();
    need_forward = false;
    return 0;  // ctc返回，交由MySQL判断报错
  }

  if (itemFunc) {
    // 从临时变量取值
    ctc_log_system("[CTC_DDL_REWRITE]:get user var value. %s", sql_str.c_str());
    int ret = ctc_get_user_var_string(thd, itemFunc, val_str);
    if (ret != 0) {
      need_forward = false;
      return -1;
    }
  } else if (itemFuncSys) {
    // 从系统变量取值
    String* new_str;
    String str;
    ctc_log_system("[CTC_DDL_REWRITE]:get system var value. %s", sql_str.c_str());
#ifdef FEATURE_X_FOR_MYSQL_26
    if (itemFuncSys->bind(thd)) {
      need_forward = false;
      return -1;
    }
#endif
    itemFuncSys->fixed = true;
    new_str = itemFuncSys->val_str(&str);
    if (!new_str) {
      is_null_value = true;
      val_str = "null";
    } else if (new_str == itemFuncSys->error_str()) {
      need_forward = false;
      return -1;
    } else {
      val_str = new_str->c_ptr();
    }
  } else {
    // 其他变量类型
    String* new_str;
    String str;
    if (!(new_str = setvar->value->val_str(&str))) {
      is_null_value = true;
      val_str = "null";
    } else {
      val_str = new_str->c_ptr();
    }
  }
  return 0;
}

static int ctc_check_set_opt_rule(set_var *setvar, string& name_str, string& user_val_str, bool& need_forward) {
  int ret = 0;
  transform(name_str.begin(), name_str.end(), name_str.begin(), ::tolower);
  auto it = set_variable_rules_map.find(name_str);
  if (it != set_variable_rules_map.end()) {
    int rule_res = it->second(setvar, need_forward, user_val_str);
    if (rule_res == -1) {
      need_forward = false;
    }
    ret |= rule_res;
  }
  return ret;
}

static int ctc_set_user_var_flag(MYSQL_THD thd, string name, string value) {
  handlerton* hton = get_ctc_hton();
  thd_sess_ctx_s *sess_ctx = get_or_init_sess_ctx(hton, thd);
  if (sess_ctx == nullptr) {
    return HA_ERR_OUT_OF_MEM;
  }
  bool is_flag_set = (value == "1") || (value == "true");
  bool is_flag_unset = (value == "0") || (value == "false") || (value == "NULL");
  auto it = user_var_flag_map.find(name);
  if (it != user_var_flag_map.end()) {
    if (is_flag_set) {
      sess_ctx->set_flag |= it->second;
    } else if (is_flag_unset) {
      sess_ctx->set_flag &= ~it->second;
    } else {
      my_printf_error(ER_UNKNOWN_COM_ERROR, "Invalid variable value for '%s': '%s'", MYF(0),
                      name.c_str(), value.c_str());
      return -1;
    }
  }
  return 0;
}

static int check_non_system_var(set_var_base *var, bool& need_forward, MYSQL_THD thd) {
  need_forward = false;
  if (typeid(*var) != typeid(set_var_user)) {
    return 0;
  }

  set_var_user *setvar_user = dynamic_cast<set_var_user *>(var);
  String set_str;
  string var_name;
  string var_value;
  // 参考set_var.cc: set_var_user::print
  // set_str由print函数追加"@"和":="生成
  setvar_user->print(thd, &set_str);
  if (set_str.ptr() == nullptr || set_str.length() == 0) {
    my_printf_error(ER_NOT_ALLOWED_COMMAND, "%s", MYF(0), "The used command is not allowed");
    return -1;
  }
  string str(set_str.ptr(), set_str.length());
  size_t pos = str.find("@");
  if (pos != str.npos) {
      size_t end_pos = str.find(":=", pos);
      if (end_pos != str.npos) {
          size_t name_start = pos + 1;
          size_t name_len = end_pos - name_start;
          var_name = str.substr(name_start, name_len);
      }
  }
  size_t value_pos = str.find(":=");
  if (value_pos != str.npos) {
      size_t value_start = value_pos + 2;
      size_t value_len = str.length() - value_start;
      var_value = str.substr(value_start, value_len);
  }

  return ctc_set_user_var_flag(thd, var_name, var_value);
}

static int check_system_var(set_var_base *var, string &sql_str, MYSQL_THD thd,
                            bool& need_forward, bool& contain_subselect) {
  set_var *setvar = dynamic_cast<set_var *>(var);
  bool is_set_default_value = false;
  bool is_null_value = false;
  int ret = 0;
  string name_str;
  string val_str;
#ifdef FEATURE_X_FOR_MYSQL_32
    if (setvar) {
      std::function<bool(const System_variable_tracker &, sys_var *)> f = [&thd, &need_forward, setvar]
      (const System_variable_tracker &, sys_var *system_var) {
        if (system_var) {
          need_forward = !system_var->is_readonly() && setvar->is_global_persist();
        }
        return true;
      };
      setvar->m_var_tracker.access_system_variable<bool>(thd, f).value_or(true);
      name_str = setvar->m_var_tracker.get_var_name();
#elif defined(FEATURE_X_FOR_MYSQL_26)
    if (setvar && setvar->var) {
      need_forward = !setvar->var->is_readonly() && setvar->is_global_persist()
	      && setvar->var->check_scope(OPT_GLOBAL);
      name_str = setvar->var->name.str;
#endif
      
      if (!contain_subselect) {
        /* get user value (@xxxxx) as string */
        if (!setvar->value) {
          is_set_default_value = true;
          val_str = "";
        } else {
          ret = ctc_get_variables_value_string(thd, sql_str, setvar, val_str, is_null_value, need_forward);
        }
        ret |= ctc_check_set_opt_rule(setvar, name_str, val_str, need_forward);
      }
    }
    if (need_forward && allow_sqlcmd(thd, "ctc_setopt_disabled") != 0) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
                      "Set global variable query is not allowed (ctc_setopt_disabled = true)");
      return -1;
    }

    if (IS_METADATA_NORMALIZATION() && !contain_subselect && need_forward && setvar) {
      if (setvar->check(thd) == 0) {
        bool var_real_type = false;
        if (setvar->value && setvar->value->result_type() == INT_RESULT) {
          var_real_type = true;
        }
        uint32_t options = ctc_set_var_option(is_null_value, is_set_default_value, setvar);
#ifdef FEATURE_X_FOR_MYSQL_26
        ret = ctc_set_var_meta(thd, options, setvar->base.str, name_str, val_str, var_real_type);
#elif defined(FEATURE_X_FOR_MYSQL_32)
        ret = ctc_set_var_meta(thd, options, setvar->m_var_tracker.get_var_name(),
		       	name_str, val_str, var_real_type);
#endif
      } else {
        thd->clear_error();
        need_forward = false;  // 值校验失败, ctc不进行广播并返回成功, 后续报错由MySQL完成
      }
    }
    return ret;
}

/* 参考set_var.cc: sql_set_variables */
static int ctc_check_set_opt(string &sql_str, MYSQL_THD thd, bool &need_forward) {
  List_iterator_fast<set_var_base> var_it(thd->lex->var_list);

  set_var_base *var = nullptr;
  int ret = 0;

  // broadcast SET_OPTION query with subselect item
  bool contain_subselect = false;
  if (thd->lex->query_tables) {
    contain_subselect = true;
  }
  var_it.rewind();
  while ((var = var_it++)) {
    if (typeid(*var) != typeid(set_var)) {
      ret = check_non_system_var(var, need_forward, thd);
    } else {
      ret = check_system_var(var, sql_str, thd, need_forward, contain_subselect);
    }
    ctc_log_debug("set option %s, need_forward: %d", sql_str.c_str(), need_forward);
  }
  if (IS_METADATA_NORMALIZATION() && !contain_subselect) {
    need_forward = false;
  }
  return ret;
}

static int is_system_db(const char *ddl_db) {
  if (mysql_system_db.find(ddl_db) != mysql_system_db.end()) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
      "Once the CTC is loaded, it must be used as the default engine. To specify other engine for table, uninstall the CTC first.");
    return -1;
  }
  return 0;
}

static int ctc_check_ddl_engine(string &, MYSQL_THD thd, bool &need_forward) {
  need_forward = false; // broadcast by storage engine
  LEX_CSTRING ctc_name;
  ctc_name.str = ctc_hton_name;
  ctc_name.length = strlen(ctc_hton_name);
  handlerton *ctc_handlerton = nullptr;
  // 获取CTC引擎handlerton指针，如果thd->lex->create_info->db_type和CTC引擎指针不相等，那么必然不是CTC引擎
  plugin_ref plugin = ha_resolve_by_name(thd, &ctc_name, false);
  if (plugin) {
    ctc_handlerton = plugin_data<handlerton *>(plugin);
  }

  // 检查ddl语句是否显示指定非CTC
  if (thd->lex->create_info != nullptr &&
      thd->lex->create_info->db_type != nullptr &&
      thd->lex->create_info->db_type != ctc_handlerton &&
      !(thd->lex->create_info->options & HA_LEX_CREATE_TMP_TABLE)) {
    my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
      "Once the CTC is loaded, it must be used as the default engine. To specify other engine for table, uninstall the CTC first.");
    return -1;
  }

  if (!IS_METADATA_NORMALIZATION()) {
    // create like table 检查是否是系统库
    if (thd->lex->query_tables != nullptr &&
        thd->lex->query_tables->next_global != nullptr &&
        thd->lex->create_info != nullptr &&
        thd->lex->create_info->options & HA_LEX_CREATE_TABLE_LIKE &&
        !(thd->lex->create_info->options & HA_LEX_CREATE_TMP_TABLE) &&
        !thd->lex->drop_temporary) {
      const char *ddl_db = thd->lex->query_tables->next_global->db;
      return is_system_db(ddl_db);
    }
  }

  // create tablespace 检查是否为engine=Innodb情况
  if (thd->lex->sql_command == SQLCOM_ALTER_TABLESPACE) {
    const Sql_cmd_tablespace *sct = dynamic_cast<const Sql_cmd_create_tablespace *>(thd->lex->m_sql_cmd);
    if (sct != nullptr &&
        sct->get_options().engine_name.str != nullptr &&
        strcmp(sct->get_options().engine_name.str, ctc_name.str) != 0) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0),
        "Once the CTC is loaded, it must be used as the default engine. To specify other engine for table, uninstall the CTC first.");
      return -1;
    }
  }

  if (!IS_METADATA_NORMALIZATION()) {
    // create表 && drop表/库 (检查是否是系统库上ddl)
    if (thd->lex->query_tables != nullptr &&
        (thd->lex->create_info != nullptr && !(thd->lex->create_info->options & HA_LEX_CREATE_TMP_TABLE)) &&
        !thd->lex->drop_temporary) {
      const char *ddl_db = thd->lex->query_tables->db;
      return is_system_db(ddl_db);
    }
  }

  return 0;
}

static int ctc_check_ddl(string &, MYSQL_THD, bool &need_forward) {
  need_forward = false; // broadcast by storage engine
  return 0;
}

static int ctc_check_unspport_ddl(string &, MYSQL_THD, bool &) {
  my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "Cantian doesn't support current operation");
  return -1;
}

static int ctc_read_only_ddl(string &, MYSQL_THD thd, bool &need_forward) {
  if (check_readonly(thd, true) ||
      (thd->lex->query_tables != nullptr &&
       check_schema_readonly(thd, thd->lex->query_tables->table_name))) {
    need_forward = false;
  }
  return 0;
}

static int ctc_lock_tables_ddl(string &, MYSQL_THD thd, bool &) {
  int ret = 0;
  vector<MDL_ticket*> ticket_list;
  int pre_lock_ret = ctc_lock_table_pre(thd, ticket_list, MDL_SHARED_NO_READ_WRITE);
  if (pre_lock_ret != 0) {
    ctc_lock_table_post(thd, ticket_list);
    my_printf_error(ER_LOCK_WAIT_TIMEOUT, "[CTC_DDL_REWRITE]: LOCK TABLE FAILED", MYF(0));
    return ER_LOCK_WAIT_TIMEOUT;
  }
#ifdef FEATURE_X_FOR_MYSQL_32
  Table_ref *tables = thd->lex->query_tables;
  for (Table_ref *table = tables; table != NULL; table = table->next_global) {
#elif defined(FEATURE_X_FOR_MYSQL_26)
  TABLE_LIST *tables = thd->lex->query_tables;
  for (TABLE_LIST *table = tables; table != NULL; table = table->next_global) {
#endif
    ctc_handler_t tch;
    tch.inst_id = ctc_instance_id;
    handlerton* hton = get_ctc_hton();

    CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
    int32_t mdl_type = 0;
    auto desc_type = table->lock_descriptor().type;
    if (desc_type >= TL_READ_DEFAULT && desc_type <= TL_READ_NO_INSERT) {
      mdl_type = (int32_t)MDL_SHARED_READ_ONLY;
    } else if (desc_type >= TL_WRITE_ALLOW_WRITE && desc_type <= TL_WRITE_ONLY) {
      mdl_type = (int32_t)MDL_SHARED_NO_READ_WRITE;
    } else {
      continue;
    }
    ctc_lock_table_info lock_info = {{0}, {0}, {0}, {0}, SQLCOM_LOCK_TABLES, mdl_type};
    FILL_USER_INFO_WITH_THD(lock_info, thd);
    strncpy(lock_info.db_name, table->db, SMALL_RECORD_SIZE - 1);
    strncpy(lock_info.table_name, table->table_name, SMALL_RECORD_SIZE - 1);
    int err_code = 0;
    ret = ctc_lock_table(&tch, lock_info.db_name, &lock_info, &err_code);
    if (ret != 0) {
      break;
    }
  }

  ctc_lock_table_post(thd, ticket_list);

  if (ret != 0) {
#ifdef FEATURE_X_FOR_MYSQL_32
  for (Table_ref *table = tables; table != NULL; table = table->next_global) {
#elif defined(FEATURE_X_FOR_MYSQL_26)
  for (TABLE_LIST *table = tables; table != NULL; table = table->next_global) {
#endif
      ctc_handler_t tch;
      tch.inst_id = ctc_instance_id;
      handlerton* hton = get_ctc_hton();

      CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));
      ctc_lock_table_info lock_info = {{0}, {0}, {0}, {0}, SQLCOM_LOCK_TABLES, (int32_t)TL_UNLOCK};
      FILL_USER_INFO_WITH_THD(lock_info, thd);
      strncpy(lock_info.db_name, table->db, SMALL_RECORD_SIZE - 1);
      strncpy(lock_info.table_name, table->table_name, SMALL_RECORD_SIZE - 1);
      ret = ctc_unlock_table(&tch, ctc_instance_id, &lock_info);
      if (ret != 0) {
        ctc_log_error("[CTC_DDL_REWRITE]:unlock table failed, table:%s.%s", lock_info.db_name, lock_info.table_name);
      }
    }
  }
  return ret;
}

static int ctc_unlock_tables_ddl(string &, MYSQL_THD thd, bool &) {
  int ret = 0;

  ctc_handler_t tch;
  tch.inst_id = ctc_instance_id;
  handlerton* hton = get_ctc_hton();

  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));

  ctc_lock_table_info lock_info = {{0}, {0}, {0}, {0}, SQLCOM_UNLOCK_TABLES, 0};

  FILL_USER_INFO_WITH_THD(lock_info, thd);

  ret = ctc_unlock_table(&tch, ctc_instance_id, &lock_info);

  return ret;
}

typedef struct ddl_broadcast_cmd_s
{
  bool need_select_db;  // 需要指定数据库
  int (*pre_func)(string &sql_str, MYSQL_THD thd, bool &need_forward); //转发之前的预处理函数，为空则不调用
} ddl_broadcast_cmd;

static unordered_map<enum enum_sql_command, ddl_broadcast_cmd>
  ddl_cmds = {
    // DCL，broadcast on !ctc_dcl_disabled
    {SQLCOM_GRANT, {true, ctc_check_dcl}},
    {SQLCOM_REVOKE, {false, ctc_check_dcl}},
    {SQLCOM_CREATE_USER, {false, ctc_check_dcl}},
    {SQLCOM_DROP_USER, {false, ctc_check_dcl}},
    {SQLCOM_RENAME_USER, {false, ctc_check_dcl}},
    {SQLCOM_REVOKE_ALL, {false, ctc_check_dcl}},
    {SQLCOM_ALTER_USER, {false, ctc_check_alter_user}},
    {SQLCOM_ALTER_USER_DEFAULT_ROLE, {false, ctc_check_dcl}},
    {SQLCOM_CREATE_ROLE, {false, ctc_check_dcl}},
    {SQLCOM_DROP_ROLE, {false, ctc_check_dcl}},
    {SQLCOM_SET_ROLE, {false, ctc_check_dcl}},
    {SQLCOM_GRANT_ROLE, {false, ctc_check_dcl}},
    {SQLCOM_REVOKE_ROLE, {false, ctc_check_dcl}},
    {SQLCOM_SET_PASSWORD, {false, ctc_check_set_password}},

    // prepare statement
    {SQLCOM_PREPARE, {false, ctc_check_ddl}},
    {SQLCOM_EXECUTE, {false, ctc_check_ddl}},

    // 存储过程，broadcast
    {SQLCOM_CREATE_PROCEDURE, {true, ctc_read_only_ddl}},
    {SQLCOM_DROP_PROCEDURE, {true, ctc_read_only_ddl}},
    {SQLCOM_ALTER_PROCEDURE, {true, ctc_read_only_ddl}},
    {SQLCOM_ALTER_FUNCTION, {true, ctc_read_only_ddl}},
    {SQLCOM_CREATE_SPFUNCTION, {true, ctc_read_only_ddl}},
    {SQLCOM_DROP_FUNCTION, {true, ctc_read_only_ddl}},

    // 触发器 & 视图，broadcast
    {SQLCOM_CREATE_VIEW, {true, ctc_read_only_ddl}},
    {SQLCOM_DROP_VIEW, {true, ctc_read_only_ddl}},
    {SQLCOM_CREATE_TRIGGER, {true, ctc_read_only_ddl}},
    {SQLCOM_DROP_TRIGGER, {true, ctc_read_only_ddl}},

    // set Variable, check var map
    // SET_OPTION query with subselect item needs specific db
    {SQLCOM_SET_OPTION, {true, ctc_check_set_opt}},

    // Locking, broadcast
    {SQLCOM_LOCK_TABLES, {true, ctc_lock_tables_ddl}},
    {SQLCOM_UNLOCK_TABLES, {true, ctc_unlock_tables_ddl}},
    {SQLCOM_LOCK_INSTANCE, {false, NULL}},
    {SQLCOM_UNLOCK_INSTANCE, {false, NULL}},

    // analyze broardcast for share cbo
    {SQLCOM_ANALYZE, {true, NULL}},

    // Flush, only broadcast for REFRESH_READ_LOCK
    {SQLCOM_FLUSH, {false, ctc_check_flush}},

    // table & tablespace operations, do not broadcast in rewriter
    {SQLCOM_CREATE_TABLE, {false, ctc_check_ddl_engine}},
    {SQLCOM_ALTER_TABLE, {false, ctc_check_ddl_engine}},
    {SQLCOM_CREATE_INDEX, {false, ctc_check_ddl}},
    {SQLCOM_DROP_INDEX, {false, ctc_check_ddl}},
    {SQLCOM_REPAIR, {false, ctc_check_ddl}},
    {SQLCOM_OPTIMIZE, {false, ctc_check_ddl}},
    {SQLCOM_CHECK, {false, ctc_check_ddl}},
    {SQLCOM_RENAME_TABLE, {false, ctc_check_ddl}},
    {SQLCOM_ALTER_TABLESPACE, {false, ctc_check_ddl_engine}},

    // drop table operations, do not broadcast in rewriter
    {SQLCOM_DROP_TABLE, {false, ctc_check_ddl_engine}},

    // database operations, do not broadcast in rewriter

    {SQLCOM_CHANGE_DB, {false, ctc_check_ddl}},
    {SQLCOM_CREATE_DB, {false, ctc_check_ddl}},
    {SQLCOM_DROP_DB, {false, ctc_check_ddl_engine}},

    // alter database broadcast for recording logical logs
    {SQLCOM_ALTER_DB, {false, NULL}},

    // 不支持创建,修改，删除EVENT
    {SQLCOM_CREATE_EVENT, {false, ctc_check_unspport_ddl}},
    {SQLCOM_ALTER_EVENT, {false, ctc_check_unspport_ddl}},
    {SQLCOM_DROP_EVENT, {false, ctc_check_unspport_ddl}},

    // Replication operations - unsupported
    {SQLCOM_CREATE_SERVER, {false, ctc_check_unspport_ddl}},
    {SQLCOM_DROP_SERVER, {false, ctc_check_unspport_ddl}},
    {SQLCOM_ALTER_SERVER, {false, ctc_check_unspport_ddl}},

    // 不支持alter instance
    {SQLCOM_ALTER_INSTANCE, {false, ctc_check_unspport_ddl}},

    // 不支持import table
    {SQLCOM_IMPORT, {false, ctc_check_unspport_ddl}},

    // 不支持创建，删除SRS
    {SQLCOM_CREATE_SRS, {false, ctc_check_unspport_ddl}},
    {SQLCOM_DROP_SRS, {false, ctc_check_unspport_ddl}},

    // 不支持创建，修改，删除，设置资源组
    {SQLCOM_CREATE_RESOURCE_GROUP, {false, ctc_check_unspport_ddl}},
    {SQLCOM_ALTER_RESOURCE_GROUP, {false, ctc_check_unspport_ddl}},
    {SQLCOM_DROP_RESOURCE_GROUP, {false, ctc_check_unspport_ddl}},
    {SQLCOM_SET_RESOURCE_GROUP, {false, ctc_check_unspport_ddl}},

    // XA operations - unsupported
    {SQLCOM_XA_START, {false, ctc_check_unspport_ddl}},
    {SQLCOM_XA_END, {false, ctc_check_unspport_ddl}},
    {SQLCOM_XA_PREPARE, {false, ctc_check_unspport_ddl}},
    {SQLCOM_XA_COMMIT, {false, ctc_check_unspport_ddl}},
    {SQLCOM_XA_ROLLBACK, {false, ctc_check_unspport_ddl}},
    {SQLCOM_XA_RECOVER, {false, ctc_check_unspport_ddl}},

    // handler operations - supported
    {SQLCOM_HA_OPEN, {false, ctc_check_ddl}},
    {SQLCOM_HA_CLOSE, {false, ctc_check_ddl}},
    {SQLCOM_HA_READ, {false, ctc_check_ddl}},

};

bool is_ddl_sql_cmd(enum_sql_command sql_cmd) {
  if (ddl_cmds.find(sql_cmd) != ddl_cmds.end()) {
    return true;
  }
  return false;
}

bool is_dcl_sql_cmd(enum_sql_command sql_cmd) {

  if (sql_cmd == SQLCOM_GRANT || sql_cmd == SQLCOM_REVOKE ||
      sql_cmd == SQLCOM_CREATE_USER || sql_cmd == SQLCOM_DROP_USER || 
      sql_cmd == SQLCOM_RENAME_USER || sql_cmd == SQLCOM_REVOKE_ALL || 
      sql_cmd == SQLCOM_ALTER_USER || sql_cmd == SQLCOM_ALTER_USER_DEFAULT_ROLE || 
      sql_cmd == SQLCOM_CREATE_ROLE || sql_cmd == SQLCOM_DROP_ROLE ||
      sql_cmd == SQLCOM_SET_ROLE || sql_cmd ==SQLCOM_GRANT_ROLE || 
      sql_cmd == SQLCOM_REVOKE_ROLE) {
    return true;
  }

  return false;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_memory_key key_memory_ctc_ddl_rewriter;

static PSI_memory_info all_rewrite_memory[] = {
    {&key_memory_ctc_ddl_rewriter, "ctc_ddl_rewriter", 0, 0, PSI_DOCUMENT_ME}};

static int plugin_init(MYSQL_PLUGIN) {
  const char *category = "rewriter";
  int count = static_cast<int>(array_elements(all_rewrite_memory));
  mysql_memory_register(category, all_rewrite_memory, count);
  ctc_log_system("plugin_init called");
  return 0; /* success */
}
#else
#define plugin_init nullptr
#define key_memory_ctc_ddl_rewriter PSI_NOT_INSTRUMENTED
#endif /* HAVE_PSI_INTERFACE */

static void ctc_ddl_rewrite_handle_error(MYSQL_THD thd, int ret, ctc_ddl_broadcast_request &broadcast_req, uint8_t sql_cmd) {
  if (ret == CTC_DDL_VERSION_NOT_MATCH) {
    broadcast_req.err_code = ER_DISALLOWED_OPERATION;
    my_printf_error(ER_DISALLOWED_OPERATION, "Version not match. Please make sure cluster on the same version.", MYF(0));
    ctc_log_system("[CTC_DDL_REWRITE]: Version not match, sql=%s", sql_without_plaintext_password(&broadcast_req).c_str());
    return;
  }

  my_printf_error(broadcast_req.err_code, "Got error(err_code:%d, err_msg:%s) on remote mysql.", MYF(0),
    broadcast_req.err_code, broadcast_req.err_msg);

  ctc_log_error("[CTC_DDL_REWRITE]:Got error on remote mysql, query:%s, user_name:%s, err_code:%d, err_msg:%s",
    sql_without_plaintext_password(&broadcast_req).c_str(), broadcast_req.user_name, broadcast_req.err_code, broadcast_req.err_msg);

  // unlock when lock instance failed
  if (sql_cmd == SQLCOM_LOCK_INSTANCE) {
    ctc_check_unlock_instance(thd);
  }

  return;
}

int ddl_broadcast_and_wait(MYSQL_THD thd, string &query_str, 
                                  uint8_t sql_cmd, ddl_broadcast_cmd &broadcast_cmd) {
  ctc_handler_t tch;
  memset(&tch, 0, sizeof(tch));
  tch.inst_id = ctc_instance_id;
  handlerton *hton = get_ctc_hton();
  update_member_tch(tch, hton, thd);

  ctc_ddl_broadcast_request broadcast_req {{0}, {0}, {0}, {0}, 0, 0, 0, 0, {0}};

  if (thd->db().str != NULL && strlen(thd->db().str) > 0 &&
      broadcast_cmd.need_select_db) {
    strncpy(broadcast_req.db_name, thd->db().str, SMALL_RECORD_SIZE - 1);
  }

  if (sql_cmd == SQLCOM_SET_OPTION) {
    // Use it to mark SET_OPTION query with subselect item
    broadcast_req.options |= CTC_SET_VARIABLE_WITH_SUBSELECT;
  }
  broadcast_req.options |= CTC_NOT_NEED_CANTIAN_EXECUTE;
  broadcast_req.options |= (thd->lex->contains_plaintext_password ? CTC_CURRENT_SQL_CONTAIN_PLAINTEXT_PASSWORD : 0);
  FILL_BROADCAST_BASE_REQ(broadcast_req, query_str.c_str(), thd->m_main_security_ctx.priv_user().str,
    thd->m_main_security_ctx.priv_host().str, ctc_instance_id, sql_cmd);
  
  vector<MDL_ticket*> ticket_list;
  if (sql_cmd == SQLCOM_LOCK_TABLES) {
    int pre_lock_ret = ctc_lock_table_pre(thd, ticket_list, MDL_SHARED_NO_READ_WRITE);
    if (pre_lock_ret != 0) {
      ctc_lock_table_post(thd, ticket_list);
      my_printf_error(ER_LOCK_WAIT_TIMEOUT, "[CTC_DDL_REWRITE]: LOCK TABLE FAILED", MYF(0));
      return ER_LOCK_WAIT_TIMEOUT;
    }
  }

  // 全局创建连接成功后执行sql语句
  int ret = ctc_broadcast_rewrite_sql(&tch, &broadcast_req, true);

  if (sql_cmd == SQLCOM_LOCK_TABLES) {
    ctc_lock_table_post(thd, ticket_list);
  }

  DBUG_EXECUTE_IF("ctc_ddl_rewrite_broadcast_fail", { ret = CTC_DDL_VERSION_NOT_MATCH;broadcast_req.err_code = ER_DISALLOWED_OPERATION; });
  if (ret != 0 && broadcast_req.err_code != 0) {
    ctc_ddl_rewrite_handle_error(thd, ret, broadcast_req, sql_cmd);
    return broadcast_req.err_code;
  }

  ctc_log_system("[CTC_DDL_REWRITE]:ret:%d, query:%s, user_name:%s, err_code:%d, broadcast_inst_id:%u, "
    "conn_id:%u, ctc_inst_id:%u", ret, sql_without_plaintext_password(&broadcast_req).c_str(), broadcast_req.user_name,
    broadcast_req.err_code, broadcast_req.mysql_inst_id, tch.thd_id, tch.inst_id);

  update_sess_ctx_by_tch(tch, hton, thd);
  return convert_ctc_error_code_to_mysql((ct_errno_t)ret);
}

bool plugin_ddl_passthru(MYSQL_THD thd,
                         unordered_map<enum enum_sql_command, ddl_broadcast_cmd>::iterator &it) {
  if (it == ddl_cmds.end()) {
    return true;
  }

  if (engine_ddl_passthru(thd)) {
    return true;
  }

#ifdef NDEBUG
  const char *engine_str = thd->variables.table_plugin->name.str;
#else
  const char *engine_str = (*thd->variables.table_plugin)->name.str;
#endif
  if (strcasecmp(engine_str, ctc_hton_name) != 0) {
    return true;
  }

  return false;
}

bool check_agent_connection(MYSQL_THD thd) {
  // Only user from localhost/127.0.0.1 or % can be proxied remotely
  if (strcmp(thd->m_main_security_ctx.priv_host().str, my_localhost) != 0 &&
      strcmp(thd->m_main_security_ctx.priv_host().str, "127.0.0.1") != 0 &&
      strcmp(thd->m_main_security_ctx.priv_host().str, "%") != 0 &&
      strcmp(thd->m_main_security_ctx.priv_host().str, "skip-grants host") != 0) {
    my_printf_error(ER_DISALLOWED_OPERATION,
                    "%s@%s is not allowed for DDL remote execution!", MYF(0),
                    thd->m_main_security_ctx.priv_user().str,
                    thd->m_main_security_ctx.priv_host().str);
    return true;
  }

  MYSQL *agent_conn = NULL;
  // 连接mysql server失败，不允许执行ddl操作
  if (ctc_init_agent_client(agent_conn) != 0) {
    my_printf_error(ER_DISALLOWED_OPERATION,
                    "Failed to establish connection for DDL remote execution!", MYF(0));
    ctc_close_mysql_conn(&agent_conn);
    return true;
  }

  ctc_close_mysql_conn(&agent_conn);
  return false;
}

int ctc_record_sql(MYSQL_THD thd, bool need_select_db) {
  ctc_handler_t tch;
  tch.inst_id = ctc_instance_id;
  handlerton* hton = get_ctc_hton();

  CTC_RETURN_IF_NOT_ZERO(get_tch_in_handler_data(hton, thd, tch));

  ctc_ddl_broadcast_request broadcast_req {{0}, {0}, {0}, {0}, 0, 0, 0, 0, {0}};

  if (thd->db().str != NULL && strlen(thd->db().str) > 0 && need_select_db) {
    strncpy(broadcast_req.db_name, thd->db().str, SMALL_RECORD_SIZE - 1);
  }

  broadcast_req.options |= CTC_NOT_NEED_CANTIAN_EXECUTE;
  broadcast_req.options |= (thd->lex->contains_plaintext_password ? CTC_CURRENT_SQL_CONTAIN_PLAINTEXT_PASSWORD : 0);
  string sql = string(thd->query().str).substr(0, thd->query().length);

  FILL_BROADCAST_BASE_REQ(broadcast_req, sql.c_str(), thd->m_main_security_ctx.priv_user().str,
    thd->m_main_security_ctx.priv_host().str, ctc_instance_id, (uint8_t)thd->lex->sql_command);
  
  int ret = ctc_record_sql_for_cantian(&tch, &broadcast_req, false);
  update_sess_ctx_by_tch(tch, hton, thd);

  ctc_log_system("[CTC_REWRITE_META]:ret:%d, query:%s", ret, sql_without_plaintext_password(&broadcast_req).c_str());

  return ret;
}

bool plugin_ddl_block(MYSQL_THD thd, 
                      unordered_map<enum enum_sql_command, ddl_broadcast_cmd>::iterator &it,
                      string &query_str,
                      bool &need_forward) {
  ddl_broadcast_cmd broadcast_cmd = it->second;
  if (broadcast_cmd.pre_func != NULL) {
    int ret = broadcast_cmd.pre_func(query_str, thd, need_forward);
    if (ret != 0) {
      ctc_log_system("pre_func execute failed,ret:%d,cmd:%d, sql:%s", 
                   ret, it->first, query_str.c_str());
      return true;
    }
  }

  if (ctc_check_ddl_sql_length(query_str)) {
    return true;
  }

  if (!need_forward) {
    return false;
  }

  if (IS_METADATA_NORMALIZATION() && !is_dcl_sql_cmd(thd->lex->sql_command)) {
    if (ctc_record_sql(thd, broadcast_cmd.need_select_db)) {
      ctc_log_error("[CTC_META_SQL]:record sql str failed. sql:%s", query_str.c_str());
      return true;
    }
  }

  if (!IS_METADATA_NORMALIZATION()) {
    if (engine_skip_ddl(thd)) {
      ctc_log_warning("[CTC_NOMETA_SQL]:record sql str only generate metadata. sql:%s", query_str.c_str());
      return false;
    }
    // disallow ddl query if ctc_concurrent_ddl=OFF and ctc_enable_ddl not set
    if (!ddl_enabled_normal(thd)) {
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "DDL not allowed in this mode, Please check the value of @@ctc_concurrent_ddl.");
      return true;
    }

    return check_agent_connection(thd);
  }

  return false;
}

// due to MDL_key::BACKUP_LOCK`s MDL_INTENTION_EXCLUSIVE comflicts with MDL_key::BACKUP_LOCK`s MDL_SHARED (user execute STMT `lock instance for backup`)
static bool ctc_is_instance_locked_by_backup(MYSQL_THD thd) {
  MDL_request mdl_request;
  MDL_key key(MDL_key::BACKUP_LOCK, "", "");
  // check this conn whether has backup S lock
  if (thd->mdl_context.owns_equal_or_stronger_lock(&key, MDL_SHARED)) {
          return true;
  }
  // check other conn whether has backup S lock
  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP_LOCK, "", "", MDL_INTENTION_EXCLUSIVE, MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&mdl_request, 0)) {
    thd->clear_error(); // clear lock failed error
    return true;
  } else {
    thd->mdl_context.release_lock(mdl_request.ticket); // MDL_EXPLICIT need us to release when locked succeed
    return false;
  }
}

static bool ctc_is_have_global_read_lock(MYSQL_THD thd) {
  // check if current connetion hold global read lock, let it go
  if (thd->global_read_lock.is_acquired()) {
    return false;
  }

  // block other connections
  if (Global_read_lock::global_read_lock_active()) {
    return true;
  }

  return false;
}

static inline bool ctc_is_broadcast_by_storage_engine(ddl_broadcast_cmd broadcast_cmd) {
  return broadcast_cmd.pre_func == ctc_check_ddl || broadcast_cmd.pre_func == ctc_check_ddl_engine;
}

static bool ctc_is_set_session_var(MYSQL_THD thd, string &query_str) {
  if (thd->lex->sql_command != SQLCOM_SET_OPTION) {
    return false;
  }

  set_var_base *var = nullptr;
  List_iterator_fast<set_var_base> var_it(thd->lex->var_list);
  
  while ((var = var_it++)) {
    // identify SET statement other than GLOBAL scop
    set_var *setvar = dynamic_cast<set_var *>(var);
    if (setvar && setvar->type != OPT_GLOBAL) {
      ctc_log_system("[CTC_DDL_REWRITE]:let non global scop sql pass. sql_str:%s", query_str.c_str());
      return true;
    }

    // identify "set names utf8" sql str
    set_var_collation_client *set_var_collation = dynamic_cast<set_var_collation_client *>(var);
    if (set_var_collation) {
      ctc_log_system("[CTC_DDL_REWRITE]:let set names xxx pass. sql_str:%s", query_str.c_str());
      return true;
    }
  }

  return false;
}

static int ctc_check_metadata_switch() {
  metadata_switchs metadata_switch = (metadata_switchs)ctc_get_metadata_switch();
  switch (metadata_switch) {
    case metadata_switchs::MATCH_META: {
      return 0;
    }
    case metadata_switchs::MATCH_NO_META:
      return 1;
    case metadata_switchs::CLUSTER_NOT_READY:
      ctc_log_error("[CTC_META]: Cantian cluster not ready");
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "CANTIAN cluster not read.");
      return -1;
    case metadata_switchs::NOT_MATCH:
      ctc_log_error("[CTC_META]: The metadata switch of CTC and CANTIAN not match");
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "metadata switch not match.");
      return -1;
    default:
      ctc_log_error("[CTC_META]: ctc_get_metadata_switch fail");
      my_printf_error(ER_DISALLOWED_OPERATION, "%s", MYF(0), "CTC get metadata switch status fail.");
      return -1;
  }
}

static int ctc_ddl_rewrite(MYSQL_THD thd, mysql_event_class_t event_class,
                           const void *event) {
  if (is_meta_version_initialize()) {
    return 0;
  }

  /* We can exit early if this is not a pre-parse event. */
  const struct mysql_event_parse *event_parse =
      static_cast<const struct mysql_event_parse *>(event);
  assert(event_class == MYSQL_AUDIT_PARSE_CLASS &&
         event_parse->event_subclass == MYSQL_AUDIT_PARSE_POSTPARSE);
  if (event_class != MYSQL_AUDIT_PARSE_CLASS || event_parse->event_subclass != MYSQL_AUDIT_PARSE_POSTPARSE) {
    return 1;
  }
  enum enum_sql_command sql_cmd = thd->lex->sql_command;
  auto it = ddl_cmds.find(sql_cmd);

  bool need_forward = !engine_skip_ddl(thd);
  string query_str = string(event_parse->query.str).substr(0, event_parse->query.length);

  if (plugin_ddl_passthru(thd, it)) {
    return 0;
  }

  if (plugin_ddl_block(thd, it, query_str, need_forward)) {
    return -1;
  }
  
  int check_metadata_switch_result = ctc_check_metadata_switch();
  // for non-metadata-normalization's gate test
  DBUG_EXECUTE_IF("non_metadata_normalization", { check_metadata_switch_result = 1; });
  // broadcast SET_OPTION query with subselect item
  if (check_metadata_switch_result != 1 && !(need_forward && sql_cmd == SQLCOM_SET_OPTION)) {
    return check_metadata_switch_result;
  }
  
  if (sql_cmd == SQLCOM_LOCK_INSTANCE) {
    if (ctc_check_lock_instance(thd, need_forward)) {
      return -1;
    }
  } else if (sql_cmd == SQLCOM_UNLOCK_INSTANCE) {
    ctc_check_unlock_instance(thd);
  } else if (!IS_METADATA_NORMALIZATION() && (need_forward || ctc_is_broadcast_by_storage_engine(it->second))) {
    // block ddl when instance has exclusive backup lock (LOCK INSTANCE FOR BACKUP), ref sql_backup_lock.cc
    if (ctc_is_instance_locked_by_backup(thd)) {

      // don't block SET session variable after "lock instance for backup"
      if (ctc_is_set_session_var(thd, query_str)) {
        return 0;
      }

      my_printf_error(ER_DISALLOWED_OPERATION, "Instance has been locked, disallow this operation", MYF(0));
      ctc_log_system("[CTC_DDL_REWRITE]: Instance has been locked, disallow sql=%s", query_str.c_str());
      return -1;
    }

    if (ctc_is_have_global_read_lock(thd)) {
      my_error(ER_CANT_UPDATE_WITH_READLOCK, MYF(0));
      ctc_log_error("[CTC_DDL_REWRITE]: Instance have global read lock, disallow sql=%s", query_str.c_str());
      return -1;
    }
  }

  ddl_broadcast_cmd broadcast_cmd = it->second;
  return need_forward && ddl_broadcast_and_wait(thd, query_str, (uint8_t)sql_cmd, broadcast_cmd);  // 0: success other: fail
}

/* Audit plugin descriptor. */
static struct st_mysql_audit ctc_ddl_rewriter_descriptor = {
  MYSQL_AUDIT_INTERFACE_VERSION, /* interface version */
  nullptr,                       /* release_thd()     */
  ctc_ddl_rewrite,               /* event_notify()    */
  {
    0,
    0,
    (unsigned long)MYSQL_AUDIT_PARSE_POSTPARSE,
  }                              /* class mask        */
};

#if !defined __STRICT_ANSI__ && defined __GNUC__ && !defined __clang__
#define STRUCT_FLD(name, value) \
  name:                         \
  value
#else
#define STRUCT_FLD(name, value) value
#endif
struct st_mysql_plugin g_ctc_ddl_rewriter_plugin = {
    /* the plugin type (a MYSQL_XXX_PLUGIN value) */
    /* int */
    STRUCT_FLD(type, MYSQL_AUDIT_PLUGIN),

    /* pointer to type-specific plugin descriptor */
    /* void* */
    STRUCT_FLD(info, &ctc_ddl_rewriter_descriptor),

    /* plugin name */
    /* const char* */
    STRUCT_FLD(name, "ctc_ddl_rewriter"),

    /* plugin author (for SHOW PLUGINS) */
    /* const char* */
    STRUCT_FLD(author, "HUAWEI-CTC"),

    /* general descriptive text (for SHOW PLUGINS) */
    /* const char* */
    STRUCT_FLD(descr, "Rewrite of DDL statements."),

    /* the plugin license (PLUGIN_LICENSE_XXX) */
    /* int */
    STRUCT_FLD(license, PLUGIN_LICENSE_GPL),

    /* the function to invoke when plugin is loaded */
    /* int (*)(void*); */
    STRUCT_FLD(init, plugin_init),

    /* the function to invoke when plugin is un installed */
    /* int (*)(void*); */
    nullptr,

    /* the function to invoke when plugin is unloaded */
    /* int (*)(void*); */
    STRUCT_FLD(deinit, nullptr),

    /* plugin version (for SHOW PLUGINS) */
    /* unsigned int */
    STRUCT_FLD(version, CTC_CLIENT_VERSION_NUMBER),

    /* SHOW_VAR* */
    STRUCT_FLD(status_vars, nullptr),

    /* SYS_VAR** */
    STRUCT_FLD(system_vars, ctc_rewriter_system_variables),

    /* reserved for dependency checking */
    /* void* */
    STRUCT_FLD(__reserved1, nullptr),

    /* Plugin flags */
    /* unsigned long */
    STRUCT_FLD(flags, PLUGIN_OPT_ALLOW_EARLY),
};
