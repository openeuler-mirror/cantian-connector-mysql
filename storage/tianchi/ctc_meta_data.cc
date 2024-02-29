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
#include "ctc_meta_data.h"
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "mysql.h"
#include "sql/mysqld.h" // mysql_port, my_localhost
#include "tse_log.h"
#include "tse_srv.h"
#include "tse_util.h"
#include "tse_proxy_util.h"
#include "sql/sql_table.h"
#include "my_dir.h"
#include "sql/sql_handler.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/dd/types/procedure.h"
#include "sql/dd/types/function.h"
#include "sql/dd/types/routine.h"
#include "sql/mdl.h"
#include "sql/dd/types/event.h"
#include "sql/dd/types/resource_group.h"
#include "sql/dd/types/trigger.h"
#include "sql/auth/auth_common.h"
#include "sql/sys_vars_shared.h"  // intern_find_sys_var
#include "sql/sql_lex.h"          // lex_start/lex_end
#include "sql/handler.h"          // ha_tse_commit
#include "sql/auth/auth_acls.h"
#include "sql/sp_cache.h"         // sp_cache_invalidate
#include "sql/sp_head.h"          // Stored_program_creation_ctx
#include "sql/sp_pcontext.h"      // sp_pcontext
#include "sql/dd/impl/cache/shared_dictionary_cache.h"
#include "sql/dd/impl/utils.h"
#include "sql/sql_reload.h"
#include "mysql_com.h"

using namespace std;

extern uint32_t tse_instance_id;
static map<uint64_t, THD*> g_tse_mdl_thd_map;
static mutex m_tse_mdl_thd_mutex;
static map<THD *, map<string, MDL_ticket *> *> g_tse_mdl_ticket_maps;
static mutex m_tse_mdl_ticket_mutex;
bool no_create_dir = true;

class Release_all_ctc_explicit_locks : public MDL_release_locks_visitor {
 public:
  bool release(MDL_ticket *ticket) override {
    const MDL_key *mdl_key = ticket->get_key();
    tse_log_system("[TSE_MDL_THD]: Release ticket info, db_name:%s, name:%s, namespace:%d, ticket_type:%d",
      mdl_key->db_name(), mdl_key->name(), mdl_key->mdl_namespace(), ticket->get_type());
    return ticket->get_type() == MDL_EXCLUSIVE;
  }
};
 
static void release_ctc_thd_and_explicit_locks(THD *thd) {
  my_thread_init();
  thd->store_globals();
 
  Release_all_ctc_explicit_locks lock_visitor;
  thd->mdl_context.release_locks(&lock_visitor);
 
  thd->release_resources();
  delete thd;
 
  my_thread_end();
}

static void release_ctc_thd_tickets(THD *thd) {
  lock_guard<mutex> lock(m_tse_mdl_ticket_mutex);
  auto ticket_map_iter = g_tse_mdl_ticket_maps.find(thd);
  if (ticket_map_iter == g_tse_mdl_ticket_maps.end()) {
    return;
  }

  map<string, MDL_ticket *> *tse_mdl_ticket_map = ticket_map_iter->second;
  if (tse_mdl_ticket_map == nullptr) {
    g_tse_mdl_ticket_maps.erase(thd);
    return;
  }

  tse_mdl_ticket_map->clear();
  delete tse_mdl_ticket_map;
  g_tse_mdl_ticket_maps.erase(thd);
}

static void release_tse_mdl_thd_by_key(uint64_t mdl_thd_key) {
  /* 存在并发场景 map操作加锁 */
  lock_guard<mutex> lock(m_tse_mdl_thd_mutex);
  auto iter = g_tse_mdl_thd_map.find(mdl_thd_key);
  if (iter == g_tse_mdl_thd_map.end()) {
    return;
  }

  THD *thd = g_tse_mdl_thd_map[mdl_thd_key];
  assert(thd);
  release_ctc_thd_tickets(thd);
  release_ctc_thd_and_explicit_locks(thd);
 
  tse_log_system("[TSE_MDL_THD]: Close mdl thd by key=%lu", mdl_thd_key);
 
  g_tse_mdl_thd_map.erase(mdl_thd_key);
}

static void ctc_reload_acl_caches() {
  THD *reload_acl_thd = new (std::nothrow) THD;
  my_thread_init();
  reload_acl_thd->set_new_thread_id();
  reload_acl_thd->thread_stack = (char *)&reload_acl_thd;
  reload_acl_thd->store_globals();
  reload_acl_thd->set_query("tse_mdl_thd_notify", 18);

  reload_acl_caches(reload_acl_thd, false);
  tse_log_system("[TSE_RELOAD_ACL]:reload acl caches thd_id:%u", reload_acl_thd->thread_id());

  reload_acl_thd->release_resources();
  delete reload_acl_thd;

  my_thread_end();
}

static void release_tse_mdl_thd_by_cantian_id(uint16_t cantian_inst_id) {
  ctc_reload_acl_caches();

  lock_guard<mutex> lock(m_tse_mdl_thd_mutex);
  for (auto iter = g_tse_mdl_thd_map.begin(); iter != g_tse_mdl_thd_map.end(); ) {
    if (tse_get_cantian_id_from_conn_key(iter->first) == cantian_inst_id) {
      THD *thd = iter->second;
      assert(thd);
      release_ctc_thd_tickets(thd);
      release_ctc_thd_and_explicit_locks(thd);
      tse_log_system("[TSE_MDL_THD]: Close mdl thd by cantian_id:%u", cantian_inst_id);
      iter = g_tse_mdl_thd_map.erase(iter);
    } else {
      ++iter;
    }
  }
}

static void release_tse_mdl_thd_by_inst_id(uint32_t mysql_inst_id) {
  lock_guard<mutex> lock(m_tse_mdl_thd_mutex);
  for (auto iter = g_tse_mdl_thd_map.begin(); iter != g_tse_mdl_thd_map.end(); ) {
    if (tse_get_inst_id_from_conn_key(iter->first) == mysql_inst_id) {
      THD *thd = iter->second;
      assert(thd);
      release_ctc_thd_tickets(thd);
      release_ctc_thd_and_explicit_locks(thd);
      tse_log_system("[TSE_MDL_THD]: Close mdl thd by inst_id:%u", mysql_inst_id);
      iter = g_tse_mdl_thd_map.erase(iter);
    } else {
      ++iter;
    }
  }
}

static void ctc_init_thd(THD **thd, uint64_t thd_key) {
  lock_guard<mutex> lock(m_tse_mdl_thd_mutex);
  uint64_t bypass_key = tse_get_conn_key((uint32)0xFFFFFFFF - 1, (uint32)0xFFFFFFFF - 1, true);
  if (g_tse_mdl_thd_map.find(thd_key) != g_tse_mdl_thd_map.end() && thd_key != bypass_key) {
    (*thd) = g_tse_mdl_thd_map[thd_key];
    my_thread_init();
    (*thd)->store_globals();
  } else {
    THD* new_thd = new (std::nothrow) THD;
    my_thread_init();
    new_thd->set_new_thread_id();
    new_thd->thread_stack = (char *)&new_thd;
    new_thd->store_globals();
    new_thd->set_query("tse_mdl_thd_notify", 18);
    g_tse_mdl_thd_map[thd_key] = new_thd;
    (*thd) = new_thd;
  }
}

template <typename T>
typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_routine(T *thd, const char *schema_name, const char *routine_name, uint8 type) {
  MDL_ticket *schema_ticket = nullptr;
  MDL_ticket *routine_ticket = nullptr;
 
  tse_log_system("[INVALIDATE_ROUTINE]: enter invalidate_routine. schema_name:%s, routine_name:%s", schema_name, routine_name);
  if(!thd->mdl_context.owns_equal_or_stronger_lock(
        MDL_key::SCHEMA, schema_name, "", MDL_INTENTION_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, schema_name, "",
                MDL_INTENTION_EXCLUSIVE, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
      assert(thd->killed || thd->is_error());
      return true;
    }
    schema_ticket = mdl_request.ticket;
  }

  enum_sp_type sptype;
  MDL_key mdl_key;
  if (type == T::OBJ_RT_PROCEDURE) {
    dd::Routine::create_mdl_key(dd::Routine::RT_PROCEDURE, schema_name, routine_name, &mdl_key);
    sptype = enum_sp_type::PROCEDURE;
  } else {
    dd::Routine::create_mdl_key(dd::Routine::RT_FUNCTION, schema_name, routine_name, &mdl_key);
    sptype = enum_sp_type::FUNCTION;
  }

  if(!thd->mdl_context.owns_equal_or_stronger_lock(&mdl_key, MDL_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT_BY_KEY(&mdl_request, &mdl_key, MDL_EXCLUSIVE, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
      assert(thd->killed || thd->is_error());
      tse_log_error("[INVALIDATE_ROUTINE]: acquire routine/procedure mdl fail. schema_name:%s, routine_name:%s", schema_name, routine_name);
      return true;
    }
    routine_ticket = mdl_request.ticket;
  }
 
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Routine *routine = nullptr;
  bool ignored MY_ATTRIBUTE((unused));
  if (type == T::OBJ_RT_PROCEDURE) {
    ignored = thd->dd_client()->template acquire<dd::Procedure>(schema_name, routine_name, &routine);
  } else {
    ignored = thd->dd_client()->template acquire<dd::Function>(schema_name, routine_name, &routine);
  }
  if (routine) {
    thd->dd_client()->invalidate(routine);
  }
  
  // Invalidate routine cache.
  LEX_STRING lex_routine_name = { const_cast<char *>(routine_name), strlen(routine_name) };
  sp_name *name = new (thd->mem_root) sp_name(to_lex_cstring(schema_name), lex_routine_name, true);
  sp_cache_invalidate();
  sp_head *sp;
  sp_cache **spc = (sptype == enum_sp_type::FUNCTION ? &thd->sp_func_cache
                                                     : &thd->sp_proc_cache);
  sp = sp_cache_lookup(spc, name);
  if (sp) {
    sp_cache_flush_obsolete(spc, &sp);
  } 

  if (routine_ticket) thd->mdl_context.release_lock(routine_ticket);
  if (schema_ticket) thd->mdl_context.release_lock(schema_ticket);
 
  tse_log_system("[INVALIDATE_ROUTINE]: leave invalidate_routine. schema_name:%s, routine_name:%s", schema_name, routine_name);
  return false;
}

template <typename T>
typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_routine(T *thd MY_ATTRIBUTE((unused)), const char *schema_name MY_ATTRIBUTE((unused)),
                     const char *routine_name MY_ATTRIBUTE((unused)), uint8 type MY_ATTRIBUTE((unused))) {
  return false;
}

template <typename T>
typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_table(T *thd, const char *schema_name, const char *table_name) {
  MDL_ticket *schema_ticket = nullptr;
  MDL_ticket *table_ticket = nullptr;
 
  tse_log_system("[INVALIDATE_TABLE]: enter invalidate_table, schema:%s, table:%s", schema_name, table_name);
  if(!thd->mdl_context.owns_equal_or_stronger_lock(
        MDL_key::SCHEMA, schema_name, "", MDL_INTENTION_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, schema_name, "",
                MDL_INTENTION_EXCLUSIVE, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
      assert(thd->killed || thd->is_error());
      tse_log_error("[INVALIDATE_TABLE]: acquire schema mdl lock fail. schema:%s, table:%s ", schema_name, table_name);
      return true;
    }
    schema_ticket = mdl_request.ticket;
  }
  tse_log_system("[INVALIDATE_TABLE]: enter invalidate_table table, schema:%s, table:%s", schema_name, table_name);
  if(!thd->mdl_context.owns_equal_or_stronger_lock(
        MDL_key::TABLE, schema_name, table_name, MDL_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, schema_name, table_name,
                MDL_EXCLUSIVE, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
      assert(thd->killed || thd->is_error());
      tse_log_error("[INVALIDATE_TABLE]: acquire table mdl lock fail. schema:%s, table:%s ", schema_name, table_name);
      return true;
    }
    table_ticket = mdl_request.ticket;
  }
 
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  tdc_remove_table(thd, TDC_RT_REMOVE_NOT_OWN, schema_name, table_name, false);
 
  bool ignored MY_ATTRIBUTE((unused));
  ignored = thd->dd_client()->invalidate(schema_name, table_name);
 
  if (schema_ticket) thd->mdl_context.release_lock(schema_ticket);
  if (table_ticket) thd->mdl_context.release_lock(table_ticket);
 
  tse_log_system("[INVALIDATE_TABLE]: leave invalidate_table, schema:%s, table:%s", schema_name, table_name);
  return false;
}

template <typename T>
typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_table(T *thd MY_ATTRIBUTE((unused)), const char *schema_name MY_ATTRIBUTE((unused)),
                   const char *table_name MY_ATTRIBUTE((unused))) {
  return false;
}

template <typename T>
typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_schema(T *thd, const char *schema_name) {
  MDL_ticket *schema_ticket = nullptr;
 
  tse_log_system("[INVALIDATE_SCHEMA]:enter invalidate_schema, schema:%s", schema_name);
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, schema_name, "",
              MDL_EXCLUSIVE, MDL_EXPLICIT);
 
  if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
    assert(thd->killed || thd->is_error());
    tse_log_error("[INVALIDATE_SCHEMA]: acquire schema(%s) mdl lock fail.", schema_name);
    return true;
  }
  schema_ticket = mdl_request.ticket;
 
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
 
  const dd::Schema *schema = nullptr;
  bool ignored MY_ATTRIBUTE((unused));
  ignored = thd->dd_client()->acquire(schema_name, &schema);
  if (schema) {
    thd->dd_client()->invalidate(schema);
  }
 
  if (schema_ticket) thd->mdl_context.release_lock(schema_ticket);
 
  tse_log_system("leave invalidate_schema, schema:%s", schema_name);
  return false;
}

template <typename T>
typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_schema(T *thd, const char *schema_name) {
  return false;
}

template <typename T>
typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_tablespace(T *thd, const char *tablespace_name) {
  MDL_ticket *tablespace_ticket = nullptr;
 
  tse_log_system("[INVALIDATE_TABLESPACE]: enter invalidate_tablespace, tablespace_name:%s", tablespace_name);
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLESPACE, tablespace_name, "",
              MDL_EXCLUSIVE, MDL_EXPLICIT);
 
  if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
    assert(thd->killed || thd->is_error());
    tse_log_error("[INVALIDATE_TABLESPACE]: acquire tablespace mdl lock fail. tablespace_name:%s", tablespace_name);
    return true;
  }
  tablespace_ticket = mdl_request.ticket;
 
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
 
  const dd::Tablespace *tablespace = nullptr;
  bool ignored MY_ATTRIBUTE((unused));
  ignored = thd->dd_client()->acquire(tablespace_name, &tablespace);
  if (tablespace) {
    thd->dd_client()->invalidate(tablespace);
  }
  thd->dd_client()->template dump<dd::Tablespace>();
 
  if (tablespace_ticket) thd->mdl_context.release_lock(tablespace_ticket);
 
  tse_log_system("[INVALIDATE_TABLESPACE]:leave invalidate_tablespace, tablespace_name:%s", tablespace_name);
  return false;
}

template <typename T>
typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_tablespace(T *thd MY_ATTRIBUTE((unused)), const char *tablespace_name MY_ATTRIBUTE((unused))) {
  return false;
}

int tse_update_mysql_dd_cache(char *sql_str) {
  // 判断一下是不是slave—cluster，并且是归一。
  if ((!IS_METADATA_NORMALIZATION())) {
    tse_log_error("Do not try to invalidate all local dd cache in metadata_normalization mode or primary_cluster.");
    return -1;
  }
  // 获取一个thd
  THD *thd = nullptr;
  uint64_t thd_key = tse_get_conn_key((uint32)0xFFFFFFFF - 1, (uint32)0xFFFFFFFF - 1, true);
  ctc_init_thd(&thd, thd_key);

  //清空dcl的cache
  reload_acl_caches(thd, false);

  // 拿到Client dd cache, 清空
  // 拿到Shared dd cache，清空
  int ret = dd::execute_query(thd, dd::String_type((const char*)sql_str));
  tse_log_system("Tse update mysql dd cache, the sql: %s, ret:%d", sql_str, ret);
  return false;
}

template <typename T>
static typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), int>::type
  tse_invalidate_mysql_dd_cache_impl(tianchi_handler_t *tch, tse_invalidate_broadcast_request *broadcast_req, int *err_code) {
  UNUSED_PARAM(err_code);
  // 相同节点不用执行
  if(broadcast_req->mysql_inst_id == tse_instance_id) {
    tse_log_note("tse_invalidate_mysql_dd_cache curnode not need execute,mysql_inst_id:%u", broadcast_req->mysql_inst_id);
    return 0;
  }
 
  bool error = false;
  T *thd = nullptr;
  uint64_t thd_key = tse_get_conn_key(tch->inst_id, tch->thd_id, true);
  ctc_init_thd(&thd, thd_key);
 
  if (broadcast_req->is_dcl == true) {
    reload_acl_caches(thd, false);
    tse_log_system("[TSE_INVALID_DD]: remote invalidate acl cache, mysql_inst_id=%u", broadcast_req->mysql_inst_id);
  } else {
    invalidate_obj_entry_t *obj = NULL;
    uint32_t offset = 0;
    char *buff = broadcast_req->buff;
    uint32_t buff_len = broadcast_req->buff_len;

    vector<invalidate_obj_entry_t *> invalidate_routine_list;
    vector<invalidate_obj_entry_t *> invalidate_schema_list;
    while (offset < buff_len) {
      obj = (invalidate_obj_entry_t *)(buff + offset);
      printf("\n\n\ntype: %u, first: %s, second: %s\n\n\n", obj->type, obj->first, obj->second);
      switch (obj->type) {
        case T::OBJ_ABSTRACT_TABLE:
            error = invalidate_table(thd, obj->first, obj->second);
            break;
        case T::OBJ_RT_PROCEDURE:
        case T::OBJ_RT_FUNCTION:
            invalidate_routine_list.emplace_back(obj);
            break;
        case T::OBJ_SCHEMA:
            invalidate_schema_list.emplace_back(obj);
            break;
        case T::OBJ_TABLESPACE:
            error = invalidate_tablespace(thd, obj->first);
            break;
        case T::OBJ_EVENT:
        case T::OBJ_COLUMN_STATISTICS:
        case T::OBJ_RESOURCE_GROUP:
        case T::OBJ_SPATIAL_REFERENCE_SYSTEM:
        case T::OBJ_CHARSET:
        case T::OBJ_COLLATION:
        default:
            break;
      }
      offset += sizeof(invalidate_obj_entry_t);
    }
    for (auto invalidate_it : invalidate_routine_list) {
      error = invalidate_routine(thd, invalidate_it->first, invalidate_it->second, invalidate_it->type);
    }
    for (auto invalidate_it : invalidate_schema_list) {
      error = invalidate_schema(thd, invalidate_it->first);
    }
  }
 
  thd->restore_globals();
  my_thread_end();
 
  tse_log_system("[TSE_INVALID_DD]: remote invalidate dd cache success, mysql_inst_id=%u", broadcast_req->mysql_inst_id);
  return error;
}


template <typename T>
static typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), int>::type
  tse_invalidate_mysql_dd_cache_impl(tianchi_handler_t *tch MY_ATTRIBUTE((unused)),
                                     tse_invalidate_broadcast_request *broadcast_req MY_ATTRIBUTE((unused)),
                                     int *err_code MY_ATTRIBUTE((unused))) {
  return 0;
}

int tse_invalidate_all_dd_cache() {
  THD *thd = nullptr;
  uint64_t thd_key = tse_get_conn_key((uint32)0xFFFFFFFF - 1, (uint32)0xFFFFFFFF - 1, true);
  ctc_init_thd(&thd, thd_key);

#ifdef METADATA_NORMALIZED
  dd::cache::Shared_dictionary_cache::instance()->reset(true);
  int not_used;
  handle_reload_request(thd, (REFRESH_TABLES | REFRESH_FAST | REFRESH_THREADS), nullptr, &not_used);
  tse_log_system("[zzh debug] handle_reload_request finished.");
#endif
  return 0;
}

int tse_invalidate_mysql_dd_cache(tianchi_handler_t *tch, tse_invalidate_broadcast_request *broadcast_req, int *err_code) {
  return (int)tse_invalidate_mysql_dd_cache_impl<THD>(tch, broadcast_req, err_code);
}

static void tse_init_mdl_request(tse_lock_table_info *lock_info, MDL_request *mdl_request) {
  MDL_key mdl_key;
  dd::String_type schema_name = dd::String_type(lock_info->db_name);
  dd::String_type name = dd::String_type(lock_info->table_name);
  MDL_key::enum_mdl_namespace tse_mdl_namespace = (MDL_key::enum_mdl_namespace)lock_info->mdl_namespace;
  
  switch (tse_mdl_namespace) {
    case MDL_key::FUNCTION:
      dd::Function::create_mdl_key(schema_name, name, &mdl_key);
      MDL_REQUEST_INIT_BY_KEY(mdl_request, &mdl_key, MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
    case MDL_key::PROCEDURE:
      dd::Procedure::create_mdl_key(schema_name, name, &mdl_key);
      MDL_REQUEST_INIT_BY_KEY(mdl_request, &mdl_key, MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
    case MDL_key::EVENT:
      dd::Event::create_mdl_key(schema_name, name, &mdl_key);
      MDL_REQUEST_INIT_BY_KEY(mdl_request, &mdl_key, MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
    case MDL_key::RESOURCE_GROUPS:
      dd::Resource_group::create_mdl_key(schema_name, &mdl_key);
      MDL_REQUEST_INIT_BY_KEY(mdl_request, &mdl_key, MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
    case MDL_key::TRIGGER:
      dd::Trigger::create_mdl_key(schema_name, name, &mdl_key);
      MDL_REQUEST_INIT_BY_KEY(mdl_request, &mdl_key, MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
    default:
      MDL_REQUEST_INIT(mdl_request, tse_mdl_namespace, lock_info->db_name, lock_info->table_name,
                       MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
  }
  return;
}
 
int tse_mdl_lock_thd(tianchi_handler_t *tch, tse_lock_table_info *lock_info, int *err_code) {
  bool is_same_node = (tch->inst_id == tse_instance_id);
  uint64_t mdl_thd_key = tse_get_conn_key(tch->inst_id, tch->thd_id, true);
 
  if (is_same_node) {
    return false;
  }
 
  THD *thd = nullptr;
  ctc_init_thd(&thd, mdl_thd_key);
 
  MDL_request tse_mdl_request;
  tse_init_mdl_request(lock_info, &tse_mdl_request);
 
  if (thd->mdl_context.acquire_lock(&tse_mdl_request, 10)) {
    *err_code = ER_LOCK_WAIT_TIMEOUT;
    tse_log_error("[TSE_MDL_LOCK]:Get mdl lock fail. namespace:%d, db_name:%s, table_name:%s",
                  lock_info->mdl_namespace, lock_info->db_name, lock_info->table_name);
    return true;
  }

  lock_guard<mutex> lock(m_tse_mdl_ticket_mutex);
  auto iter = g_tse_mdl_ticket_maps.find(thd);
  map<string, MDL_ticket *> *tse_mdl_ticket_map = nullptr;
  if (iter == g_tse_mdl_ticket_maps.end()) {
    tse_mdl_ticket_map = new map<string, MDL_ticket *>;
    g_tse_mdl_ticket_maps[thd] = tse_mdl_ticket_map;
  } else {
    tse_mdl_ticket_map = g_tse_mdl_ticket_maps[thd];
  }
  string mdl_ticket_key;
  mdl_ticket_key.assign(((const char*)(tse_mdl_request.key.ptr())), tse_mdl_request.key.length());
  assert(mdl_ticket_key.length() > 0);
  tse_mdl_ticket_map->insert(map<string, MDL_ticket *>::value_type(mdl_ticket_key, tse_mdl_request.ticket));

  thd->restore_globals();
  my_thread_end();

  return false;
}

void ctc_mdl_unlock_thd_by_ticket(THD* thd, MDL_request *tse_release_request) {
  lock_guard<mutex> lock(m_tse_mdl_ticket_mutex);
  auto ticket_map_iter = g_tse_mdl_ticket_maps.find(thd);
  if (ticket_map_iter == g_tse_mdl_ticket_maps.end()) {
    return;
  }

  map<string, MDL_ticket *> *tse_mdl_ticket_map = ticket_map_iter->second;
  if (tse_mdl_ticket_map == nullptr) {
    g_tse_mdl_ticket_maps.erase(thd);
    return;
  }

  string mdl_ticket_key;
  mdl_ticket_key.assign(((const char*)(tse_release_request->key.ptr())), tse_release_request->key.length());
  auto ticket_iter = tse_mdl_ticket_map->find(mdl_ticket_key);
  if (ticket_iter == tse_mdl_ticket_map->end()) {
    return;
  }

  MDL_ticket *ticket = ticket_iter->second;
  thd->mdl_context.release_all_locks_for_name(ticket);
  tse_mdl_ticket_map->erase(mdl_ticket_key);
  if (tse_mdl_ticket_map->empty()) {
    delete tse_mdl_ticket_map;
    g_tse_mdl_ticket_maps.erase(thd);
  }
}

void tse_mdl_unlock_thd(tianchi_handler_t *tch, tse_lock_table_info *lock_info) {
  bool is_same_node = (tch->inst_id == tse_instance_id);
  uint64_t mdl_thd_key = tse_get_conn_key(tch->inst_id, tch->thd_id, true);

  if (is_same_node) {
    return;
  }

  auto iter = g_tse_mdl_thd_map.find(mdl_thd_key);
  if (iter == g_tse_mdl_thd_map.end()) {
    return;
  }
  THD* thd = iter->second;
  assert(thd);
  my_thread_init();
  thd->store_globals();

  MDL_request tse_release_request;
  tse_init_mdl_request(lock_info, &tse_release_request);
  ctc_mdl_unlock_thd_by_ticket(thd, &tse_release_request);

  thd->restore_globals();
  my_thread_end();
  return;
}

int close_tse_mdl_thd(uint32_t thd_id, uint32_t mysql_inst_id) {
  if (thd_id == 0) {
    if ((uint16_t)mysql_inst_id == (uint16_t)CANTIAN_DOWN_MASK) {
      /* 清理整个参天节点相关的THD */
      tse_log_system("[TSE_MDL_THD]:Close All MDL THD on bad node by cantian_instance_id:%u",
        (uint16_t)(mysql_inst_id >> 16));
      release_tse_mdl_thd_by_cantian_id((uint16_t)(mysql_inst_id >> 16));
    } else {
      /* 清理整个mysqld节点相关的THD */
      tse_log_system("[TSE_MDL_THD]:Close All MDL THD by tse_instance_id:%u", mysql_inst_id);
      release_tse_mdl_thd_by_inst_id(mysql_inst_id);
    }
  } else {
    /* 通过把mysql_inst_id左移32位 与 thd_id拼接在一起 用来唯一标识一个THD */
    uint64_t mdl_thd_key = tse_get_conn_key(mysql_inst_id, thd_id, true);
    tse_log_note("[TSE_MDL_THD]: Close THD by conn_id=%u, tse_instance_id=%u, proxy_conn_map_key=%lu",
                   thd_id, mysql_inst_id, mdl_thd_key);
    release_tse_mdl_thd_by_key(mdl_thd_key);
  }
  return 0;
}

static void ctc_get_set_var_item(THD* new_thd, sys_var* sysvar, Item** res MY_ATTRIBUTE((unused)), string& var_value,
                                 bool is_null_value) {
  switch (sysvar->show_type()) {
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
      *res = new (new_thd->mem_root)
          Item_uint(var_value.c_str(), (uint)var_value.length());
      break;
    case SHOW_SIGNED_INT:
    case SHOW_SIGNED_LONG:
    case SHOW_SIGNED_LONGLONG:
      *res = new (new_thd->mem_root)
        Item_int(var_value.c_str(), (uint)var_value.length());
      break;
    case SHOW_BOOL:
    case SHOW_MY_BOOL:
      if(var_value == "1" || var_value == "0") {
        *res = new (new_thd->mem_root)
          Item_int(var_value.c_str(), (uint)var_value.length());
      } else {
        *res = new (new_thd->mem_root)
            Item_string(var_value.c_str(), var_value.length(),
                        &my_charset_utf8mb4_bin);
      }
      break;
    case SHOW_CHAR:
    case SHOW_LEX_STRING:
      *res = new (new_thd->mem_root)
          Item_string(var_value.c_str(), var_value.length(),
                      &my_charset_utf8mb4_bin);
      break;
    case SHOW_CHAR_PTR:
      if (is_null_value)
        *res = new (new_thd->mem_root) Item_null();
      else
        *res = new (new_thd->mem_root)
            Item_string(var_value.c_str(), var_value.length(),
                        &my_charset_utf8mb4_bin);
      break;
    case SHOW_DOUBLE:
      *res = new (new_thd->mem_root)
          Item_float(var_value.c_str(), (uint)var_value.length());
      break;
    default:
      my_error(ER_UNKNOWN_SYSTEM_VARIABLE, MYF(0), sysvar->name.str);
  }
}

static void ctc_set_var_type(uint32_t option, set_var *var) {
  if ((option & TSE_SET_VARIABLE_PERSIST) > 0) {
    var->type = OPT_PERSIST;
  }
  if ((option & TSE_SET_VARIABLE_PERSIST_ONLY) > 0) {
    var->type = OPT_PERSIST_ONLY;
  }
}

static void ctc_init_thd_priv(THD** thd, Sctx_ptr<Security_context> *ctx) {
  my_thread_init();
  THD* new_thd = new (std::nothrow) THD;
  new_thd->set_new_thread_id();
  new_thd->thread_stack = (char *)&new_thd;
  new_thd->store_globals();
  lex_start(new_thd);
  new_thd->security_context()->skip_grants();
  new_thd->set_query("tse_mdl_thd_notify", 18);
 
  const std::vector<std::string> priv_list = {
      "ENCRYPTION_KEY_ADMIN", "ROLE_ADMIN", "SYSTEM_VARIABLES_ADMIN",
      "AUDIT_ADMIN"};
  const ulong static_priv_list = (SUPER_ACL | FILE_ACL);
 
  Security_context_factory default_factory(
    new_thd, "bootstrap", "localhost", Default_local_authid(new_thd),
    Grant_temporary_dynamic_privileges(new_thd, priv_list),
    Grant_temporary_static_privileges(new_thd, static_priv_list),
    Drop_temporary_dynamic_privileges(priv_list));
 
  (*ctx) = default_factory.create();
  /* attach this auth id to current security_context */
  new_thd->set_security_context(ctx->get());
  new_thd->real_id = my_thread_self();
  (*thd) = new_thd;
}

int ctc_set_sys_var(tse_ddl_broadcast_request *broadcast_req) {
  Sctx_ptr<Security_context> ctx;
  THD *new_thd = nullptr;
  ctc_init_thd_priv(&new_thd, &ctx);

  Item *res = nullptr;
  set_var *var = nullptr;
  sys_var *sysvar = nullptr;
  string base_name_src = broadcast_req->db_name;
  string var_name = broadcast_req->user_name;
  string var_value = broadcast_req->user_ip;
  List<set_var_base> tmp_var_list;
  
  bool is_default_value = ((broadcast_req->options) & (TSE_SET_VARIABLE_TO_DEFAULT)) > 0;
  bool is_null_value = ((broadcast_req->options) & (TSE_SET_VARIABLE_TO_NULL)) > 0;

  LEX_CSTRING base_name = {nullptr, 0};
  if (strlen(base_name_src.c_str())) {
    base_name = {base_name_src.c_str(), strlen(base_name_src.c_str())};
  }

  sysvar = intern_find_sys_var(var_name.c_str(), var_name.length());
  if (sysvar == nullptr) {
    assert(0);
  }
  ctc_get_set_var_item(new_thd, sysvar, &res, var_value, is_null_value);
  
  if (is_default_value) {
    if (res) {
      res->cleanup();
    }
    res = nullptr;
  }

  var = new (new_thd->mem_root) set_var(OPT_GLOBAL, sysvar, base_name, res);
  ctc_set_var_type(broadcast_req->options, var);
  tmp_var_list.push_back(var);
  int ret = sql_set_variables(new_thd, &tmp_var_list, false);
  if (ret != 0) {
    tse_log_error("ctc_set_sys_var failed in sql_set_variables, error_code:%d", ret);
    if (ret != -1) {
      assert(ret == 0);
    }
  }

  tmp_var_list.clear();
  if (res) {
    res->cleanup();
  }
  new_thd->free_items();
  lex_end(new_thd->lex);
  new_thd->release_resources();
  delete new_thd;

  my_thread_end();
  
  return ret;
}