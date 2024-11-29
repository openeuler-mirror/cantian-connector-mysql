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
#include "ctc_log.h"
#include "ctc_srv.h"
#include "ctc_util.h"
#include "ctc_proxy_util.h"
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
#include "sql/handler.h"          // ha_ctc_commit
#include "sql/auth/auth_acls.h"
#include "sql/sp_cache.h"         // sp_cache_invalidate
#include "sql/sp_head.h"          // Stored_program_creation_ctx
#include "sql/sp_pcontext.h"      // sp_pcontext
#include "sql/dd/impl/cache/shared_dictionary_cache.h"
#include "sql/dd/impl/utils.h"
#include "sql/sql_reload.h"
#include "sql/sql_plugin.h"
#include "mysql_com.h"

#include "sql/transaction.h"

using namespace std;

extern uint32_t ctc_instance_id;
static map<uint64_t, THD*> g_ctc_mdl_thd_map;
static mutex m_ctc_mdl_thd_mutex;
static map<THD *, map<string, MDL_ticket *> *> g_ctc_mdl_ticket_maps;
static map<THD *, vector<invalidate_obj_entry_t *> *> g_ctc_invalidate_routine_maps;
static map<THD *, vector<invalidate_obj_entry_t *> *> g_ctc_invalidate_schema_maps;
static mutex m_ctc_mdl_ticket_mutex;
static mutex m_ctc_invalidate_dd_cache_mutex;
bool no_create_dir = true;

class Release_all_ctc_explicit_locks : public MDL_release_locks_visitor {
 public:
  bool release(MDL_ticket *ticket) override {
    const MDL_key *mdl_key = ticket->get_key();
    ctc_log_system("[CTC_MDL_THD]: Release ticket info, db_name:%s, name:%s, namespace:%d, ticket_type:%d",
      mdl_key->db_name(), mdl_key->name(), mdl_key->mdl_namespace(), ticket->get_type());
    return ticket->get_type() == MDL_EXCLUSIVE ||
           ((ticket->get_type() == MDL_SHARED_READ_ONLY || ticket->get_type() == MDL_SHARED_NO_READ_WRITE) &&
           mdl_key->mdl_namespace() == MDL_key::TABLE);
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

static void release_routine_and_schema(THD *thd, bool *error);

static void release_ctc_thd_tickets(THD *thd) {
  lock_guard<mutex> lock(m_ctc_mdl_ticket_mutex);
  auto ticket_map_iter = g_ctc_mdl_ticket_maps.find(thd);
  if (ticket_map_iter == g_ctc_mdl_ticket_maps.end()) {
    return;
  }

  map<string, MDL_ticket *> *ctc_mdl_ticket_map = ticket_map_iter->second;
  if (ctc_mdl_ticket_map == nullptr) {
    g_ctc_mdl_ticket_maps.erase(thd);
    return;
  }

  ctc_mdl_ticket_map->clear();
  delete ctc_mdl_ticket_map;
  g_ctc_mdl_ticket_maps.erase(thd);
}

static void release_ctc_mdl_thd_by_key(uint64_t mdl_thd_key) {
  /* 存在并发场景 map操作加锁 */
  lock_guard<mutex> lock(m_ctc_mdl_thd_mutex);
  auto iter = g_ctc_mdl_thd_map.find(mdl_thd_key);
  if (iter == g_ctc_mdl_thd_map.end()) {
    return;
  }

  THD *thd = g_ctc_mdl_thd_map[mdl_thd_key];
  assert(thd);
  bool error = 0;
  release_routine_and_schema(thd, &error);
  release_ctc_thd_tickets(thd);
  release_ctc_thd_and_explicit_locks(thd);
 
  ctc_log_system("[CTC_MDL_THD]: Close mdl thd by key=%lu", mdl_thd_key);
 
  g_ctc_mdl_thd_map.erase(mdl_thd_key);
}

static void release_ctc_mdl_thd_by_cantian_id(uint16_t cantian_inst_id) {
  lock_guard<mutex> lock(m_ctc_mdl_thd_mutex);
  for (auto iter = g_ctc_mdl_thd_map.begin(); iter != g_ctc_mdl_thd_map.end(); ) {
    if (ctc_get_cantian_id_from_conn_key(iter->first) == cantian_inst_id) {
      THD *thd = iter->second;
      assert(thd);
      bool error = 0;
      release_routine_and_schema(thd, &error);
      release_ctc_thd_tickets(thd);
      release_ctc_thd_and_explicit_locks(thd);
      ctc_log_system("[CTC_MDL_THD]: Close mdl thd by cantian_id:%u", cantian_inst_id);
      iter = g_ctc_mdl_thd_map.erase(iter);
    } else {
      ++iter;
    }
  }
}

static void release_ctc_mdl_thd_by_inst_id(uint32_t mysql_inst_id) {
  lock_guard<mutex> lock(m_ctc_mdl_thd_mutex);
  for (auto iter = g_ctc_mdl_thd_map.begin(); iter != g_ctc_mdl_thd_map.end(); ) {
    if (ctc_get_inst_id_from_conn_key(iter->first) == mysql_inst_id) {
      THD *thd = iter->second;
      assert(thd);
      bool error = 0;
      release_routine_and_schema(thd, &error);
      release_ctc_thd_tickets(thd);
      release_ctc_thd_and_explicit_locks(thd);
      ctc_log_system("[CTC_MDL_THD]: Close mdl thd by inst_id:%u", mysql_inst_id);
      iter = g_ctc_mdl_thd_map.erase(iter);
    } else {
      ++iter;
    }
  }
}

static void ctc_init_thd(THD **thd, uint64_t thd_key) {
  lock_guard<mutex> lock(m_ctc_mdl_thd_mutex);
  if (g_ctc_mdl_thd_map.find(thd_key) != g_ctc_mdl_thd_map.end()) {
    (*thd) = g_ctc_mdl_thd_map[thd_key];
    my_thread_init();
    (*thd)->thread_stack = (char *)thd;
    (*thd)->store_globals();
  } else {
    THD* new_thd = new (std::nothrow) THD;
    my_thread_init();
    new_thd->set_new_thread_id();
    new_thd->thread_stack = (char *)thd;
    new_thd->store_globals();
    new_thd->set_query("ctc_mdl_thd_notify", 18);
    g_ctc_mdl_thd_map[thd_key] = new_thd;
    (*thd) = new_thd;
  }
}

#ifdef METADATA_NORMALIZED
static void ctc_insert_schema(THD *thd, invalidate_obj_entry_t *obj) {
  lock_guard<mutex> lock(m_ctc_invalidate_dd_cache_mutex);
  auto it = g_ctc_invalidate_schema_maps.find(thd);
  vector<invalidate_obj_entry_t *> *invalidate_schema_list = nullptr;
  if (it == g_ctc_invalidate_schema_maps.end()) {
    invalidate_schema_list = new vector<invalidate_obj_entry_t *>;
    assert(invalidate_schema_list);
    g_ctc_invalidate_schema_maps[thd] = invalidate_schema_list;
  }
  g_ctc_invalidate_schema_maps[thd]->emplace_back(obj);
}
#endif

#ifdef METADATA_NORMALIZED
static void ctc_insert_routine(THD *thd, invalidate_obj_entry_t *obj) {
  lock_guard<mutex> lock(m_ctc_invalidate_dd_cache_mutex);
  auto it = g_ctc_invalidate_routine_maps.find(thd);
  vector<invalidate_obj_entry_t *> *invalidate_routine_list = nullptr;
  if (it == g_ctc_invalidate_routine_maps.end()) {
    invalidate_routine_list = new vector<invalidate_obj_entry_t *>;
    assert(invalidate_routine_list);
    g_ctc_invalidate_routine_maps[thd] = invalidate_routine_list;
  }
  g_ctc_invalidate_routine_maps[thd]->emplace_back(obj);
}
#endif

template <typename T>
typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_routine(T *thd, const char *schema_name, const char *routine_name, uint8 type) {
  MDL_ticket *schema_ticket = nullptr;
  MDL_ticket *routine_ticket = nullptr;
 
  ctc_log_system("[INVALIDATE_ROUTINE]: enter invalidate_routine. schema_name:%s, routine_name:%s", schema_name, routine_name);
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
      ctc_log_error("[INVALIDATE_ROUTINE]: Failed to acquire routine/procedure mdl of schema_name:%s and routine_name:%s", schema_name, routine_name);
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
 
  ctc_log_system("[INVALIDATE_ROUTINE]: leave invalidate_routine. schema_name:%s, routine_name:%s", schema_name, routine_name);
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
 
  ctc_log_system("[INVALIDATE_TABLE]: enter invalidate_table, schema:%s, table:%s", schema_name, table_name);
  if(!thd->mdl_context.owns_equal_or_stronger_lock(
        MDL_key::SCHEMA, schema_name, "", MDL_INTENTION_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, schema_name, "",
                MDL_INTENTION_EXCLUSIVE, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
      assert(thd->killed || thd->is_error());
      ctc_log_error("[INVALIDATE_TABLE]: Failed to acquire schema mdl lock of schema:%s and table:%s ", schema_name, table_name);
      return true;
    }
    schema_ticket = mdl_request.ticket;
  }
  ctc_log_system("[INVALIDATE_TABLE]: enter invalidate_table table, schema:%s, table:%s", schema_name, table_name);
  if(!thd->mdl_context.owns_equal_or_stronger_lock(
        MDL_key::TABLE, schema_name, table_name, MDL_EXCLUSIVE)) {
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, schema_name, table_name,
                MDL_EXCLUSIVE, MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
      assert(thd->killed || thd->is_error());
      ctc_log_error("[INVALIDATE_TABLE]: Failed to acquire table mdl lock of schema:%s and table:%s ", schema_name, table_name);
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
 
  ctc_log_system("[INVALIDATE_TABLE]: leave invalidate_table, schema:%s, table:%s", schema_name, table_name);
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
 
  ctc_log_system("[INVALIDATE_SCHEMA]:enter invalidate_schema, schema:%s", schema_name);
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, schema_name, "",
              MDL_EXCLUSIVE, MDL_EXPLICIT);
 
  if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
    assert(thd->killed || thd->is_error());
    ctc_log_error("[INVALIDATE_SCHEMA]: Failed to acquire schema(%s) mdl lock .", schema_name);
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
 
  ctc_log_system("leave invalidate_schema, schema:%s", schema_name);
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
 
  ctc_log_system("[INVALIDATE_TABLESPACE]: enter invalidate_tablespace, tablespace_name:%s", tablespace_name);
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLESPACE, tablespace_name, "",
              MDL_EXCLUSIVE, MDL_EXPLICIT);
 
  if (thd->mdl_context.acquire_lock(&mdl_request, CTC_MDL_TIMEOUT)) {
    assert(thd->killed || thd->is_error());
    ctc_log_error("[INVALIDATE_TABLESPACE]: Failed to acquire tablespace mdl lock of tablespace_name:%s", tablespace_name);
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
 
  ctc_log_system("[INVALIDATE_TABLESPACE]:leave invalidate_tablespace, tablespace_name:%s", tablespace_name);
  return false;
}

template <typename T>
typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), bool>::type
  invalidate_tablespace(T *thd MY_ATTRIBUTE((unused)), const char *tablespace_name MY_ATTRIBUTE((unused))) {
  return false;
}

template <typename T>
static typename std::enable_if<CHECK_HAS_MEMBER_FUNC(T, invalidates), int>::type
  ctc_invalidate_mysql_dd_cache_impl(ctc_handler_t *tch, ctc_invalidate_broadcast_request *broadcast_req, int *err_code) {
  UNUSED_PARAM(err_code);
  // 相同节点不用执行
  if(broadcast_req->mysql_inst_id == ctc_instance_id) {
    ctc_log_note("ctc_invalidate_mysql_dd_cache curnode not need execute,mysql_inst_id:%u", broadcast_req->mysql_inst_id);
    return 0;
  }
 
  bool error = false;
  T *thd = nullptr;
  uint64_t thd_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, true);
  ctc_init_thd(&thd, thd_key);
 
  if (broadcast_req->is_dcl == true) {
    error = reload_acl_caches(thd, false);
    ctc_log_system("[CTC_INVALID_DD]: remote invalidate acl cache, mysql_inst_id=%u", broadcast_req->mysql_inst_id);
  } else {
    invalidate_obj_entry_t *obj = NULL;
    uint32_t offset = 1;
    char *buff = broadcast_req->buff;
    uint32_t buff_len = broadcast_req->buff_len;
    bool is_end = (buff[0] == '1');
    while (offset < buff_len) {
      obj = (invalidate_obj_entry_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(invalidate_obj_entry_t), MYF(MY_WME));
      assert(obj);
      memcpy(obj, buff + offset, sizeof(invalidate_obj_entry_t));
      printf("\n[ctc_invalidate_mysql_dd_cache_impl] type: %u, first: %s, second: %s\n", obj->type, obj->first, obj->second); fflush(stdout);
      switch (obj->type) {
        case T::OBJ_ABSTRACT_TABLE:
            error = invalidate_table(thd, obj->first, obj->second);
            my_free(obj);
            break;
        case T::OBJ_RT_PROCEDURE:
        case T::OBJ_RT_FUNCTION:
            ctc_insert_routine(thd, obj);
            break;
        case T::OBJ_SCHEMA:
            ctc_insert_schema(thd, obj);
            break;
        case T::OBJ_TABLESPACE:
            error = invalidate_tablespace(thd, obj->first);
            my_free(obj);
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

    if (is_end) {
      release_routine_and_schema(thd, &error);
    }
  }
 
  thd->restore_globals();
  my_thread_end();
 
  ctc_log_system("[CTC_INVALID_DD]: remote invalidate dd cache success, mysql_inst_id=%u", broadcast_req->mysql_inst_id);
  return error;
}

#ifndef METADATA_NORMALIZED
static void release_routine_and_schema(THD *thd MY_ATTRIBUTE((unused)), bool *error MY_ATTRIBUTE((unused))) {
  return;
}
#endif

#ifdef METADATA_NORMALIZED
static void release_routine_and_schema(THD *thd, bool *error) {
  lock_guard<mutex> lock(m_ctc_invalidate_dd_cache_mutex);
  auto it_routine = g_ctc_invalidate_routine_maps.find(thd);
  auto it_schema = g_ctc_invalidate_schema_maps.find(thd);
  if (it_routine != g_ctc_invalidate_routine_maps.end()) {
    vector<invalidate_obj_entry_t *> *invalidate_routine_list = it_routine->second;
    if (invalidate_routine_list != nullptr) {
      for (auto invalidate_it : *invalidate_routine_list) {
        *error = invalidate_routine(thd, invalidate_it->first, invalidate_it->second, invalidate_it->type);
        if (*error) {
          printf("\n[invalidate_routine] error.type: %u, first: %s, second: %s\n", invalidate_it->type, invalidate_it->first, invalidate_it->second); fflush(stdout);
        }
        my_free(invalidate_it);
        invalidate_it = nullptr;
      }
      invalidate_routine_list->clear();
      delete invalidate_routine_list;
      invalidate_routine_list = nullptr;
    }
    g_ctc_invalidate_routine_maps.erase(thd);
  }
  
  if (it_schema != g_ctc_invalidate_schema_maps.end()) {
    vector<invalidate_obj_entry_t *> *invalidate_schema_list = it_schema->second;
    if (invalidate_schema_list != nullptr) {
      for (auto invalidate_it : *invalidate_schema_list) {
        *error = invalidate_schema(thd, invalidate_it->first);
        if (*error) {
          printf("\n[invalidate_schema] error.type: %u, first: %s, second: %s\n", invalidate_it->type, invalidate_it->first, invalidate_it->second); fflush(stdout);
        }
        my_free(invalidate_it);
        invalidate_it = nullptr;
      }
      invalidate_schema_list->clear();
      delete invalidate_schema_list;
      invalidate_schema_list = nullptr;
    }
    g_ctc_invalidate_schema_maps.erase(thd);
  }
}
#endif

template <typename T>
static typename std::enable_if<!CHECK_HAS_MEMBER_FUNC(T, invalidates), int>::type
  ctc_invalidate_mysql_dd_cache_impl(ctc_handler_t *tch MY_ATTRIBUTE((unused)),
                                     ctc_invalidate_broadcast_request *broadcast_req MY_ATTRIBUTE((unused)),
                                     int *err_code MY_ATTRIBUTE((unused))) {
  return 0;
}

int ctc_invalidate_mysql_dd_cache(ctc_handler_t *tch, ctc_invalidate_broadcast_request *broadcast_req, int *err_code) {
  return (int)ctc_invalidate_mysql_dd_cache_impl<THD>(tch, broadcast_req, err_code);
}

static void ctc_init_mdl_request(ctc_lock_table_info *lock_info, MDL_request *mdl_request) {
  MDL_key mdl_key;
  dd::String_type schema_name = dd::String_type(lock_info->db_name);
  dd::String_type name = dd::String_type(lock_info->table_name);
  MDL_key::enum_mdl_namespace ctc_mdl_namespace = (MDL_key::enum_mdl_namespace)lock_info->mdl_namespace;
  
  switch (ctc_mdl_namespace) {
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
    case MDL_key::BACKUP_LOCK:
      MDL_REQUEST_INIT(mdl_request, ctc_mdl_namespace, lock_info->db_name, lock_info->table_name,
                       MDL_SHARED, MDL_EXPLICIT);
      break;
    default:
      MDL_REQUEST_INIT(mdl_request, ctc_mdl_namespace, lock_info->db_name, lock_info->table_name,
                       MDL_EXCLUSIVE, MDL_EXPLICIT);
      break;
  }
  return;
}

static void ctc_init_mdl_request(ctc_lock_table_info *lock_info, MDL_request *mdl_request, MDL_key::enum_mdl_namespace ctc_mdl_namespace) {
  MDL_key mdl_key;
  dd::String_type schema_name = dd::String_type(lock_info->db_name);
  dd::String_type name = dd::String_type(lock_info->table_name);
  MDL_REQUEST_INIT(mdl_request, ctc_mdl_namespace, lock_info->db_name, lock_info->table_name,
                   (enum_mdl_type)lock_info->mdl_namespace, MDL_EXPLICIT);
  return;
}
 
int ctc_mdl_lock_thd(ctc_handler_t *tch, ctc_lock_table_info *lock_info, int *err_code) {
  bool is_same_node = (tch->inst_id == ctc_instance_id);
  uint64_t mdl_thd_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, true);
 
  if (is_same_node) {
    return false;
  }
 
  THD *thd = nullptr;
  ctc_init_thd(&thd, mdl_thd_key);
 
  MDL_request ctc_mdl_request;
  ctc_init_mdl_request(lock_info, &ctc_mdl_request);
 
  if (thd->mdl_context.acquire_lock(&ctc_mdl_request, 10)) {
    *err_code = ER_LOCK_WAIT_TIMEOUT;
    ctc_log_error("[CTC_MDL_LOCK]:Failed to get mdl lock of namespace:%d , db_name:%s and table_name:%s",
                  lock_info->mdl_namespace, lock_info->db_name, lock_info->table_name);
    return true;
  }

  lock_guard<mutex> lock(m_ctc_mdl_ticket_mutex);
  auto iter = g_ctc_mdl_ticket_maps.find(thd);
  map<string, MDL_ticket *> *ctc_mdl_ticket_map = nullptr;
  if (iter == g_ctc_mdl_ticket_maps.end()) {
    ctc_mdl_ticket_map = new map<string, MDL_ticket *>;
    g_ctc_mdl_ticket_maps[thd] = ctc_mdl_ticket_map;
  } else {
    ctc_mdl_ticket_map = g_ctc_mdl_ticket_maps[thd];
  }
  string mdl_ticket_key;
  mdl_ticket_key.assign(((const char*)(ctc_mdl_request.key.ptr())), ctc_mdl_request.key.length());
  assert(mdl_ticket_key.length() > 0);
  ctc_mdl_ticket_map->insert(map<string, MDL_ticket *>::value_type(mdl_ticket_key, ctc_mdl_request.ticket));

  thd->restore_globals();
  my_thread_end();

  return false;
}

void ctc_mdl_unlock_thd_by_ticket(THD* thd, MDL_request *ctc_release_request) {
  lock_guard<mutex> lock(m_ctc_mdl_ticket_mutex);
  auto ticket_map_iter = g_ctc_mdl_ticket_maps.find(thd);
  if (ticket_map_iter == g_ctc_mdl_ticket_maps.end()) {
    return;
  }

  map<string, MDL_ticket *> *ctc_mdl_ticket_map = ticket_map_iter->second;
  if (ctc_mdl_ticket_map == nullptr) {
    g_ctc_mdl_ticket_maps.erase(thd);
    return;
  }

  string mdl_ticket_key;
  mdl_ticket_key.assign(((const char*)(ctc_release_request->key.ptr())), ctc_release_request->key.length());
  auto ticket_iter = ctc_mdl_ticket_map->find(mdl_ticket_key);
  if (ticket_iter == ctc_mdl_ticket_map->end()) {
    return;
  }

  MDL_ticket *ticket = ticket_iter->second;
  thd->mdl_context.release_all_locks_for_name(ticket);
  ctc_mdl_ticket_map->erase(mdl_ticket_key);
  if (ctc_mdl_ticket_map->empty()) {
    delete ctc_mdl_ticket_map;
    g_ctc_mdl_ticket_maps.erase(thd);
  }
}

void ctc_mdl_unlock_tables_thd(ctc_handler_t *tch) {
  bool is_same_node = (tch->inst_id == ctc_instance_id);
  uint64_t mdl_thd_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, true);

  if (is_same_node) {
    return;
  }

  auto iter = g_ctc_mdl_thd_map.find(mdl_thd_key);
  if (iter == g_ctc_mdl_thd_map.end()) {
    return;
  }
  THD* thd = iter->second;
  assert(thd);

  my_thread_init();
  thd->store_globals();

  lock_guard<mutex> lock(m_ctc_mdl_ticket_mutex);
  auto ticket_map_iter = g_ctc_mdl_ticket_maps.find(thd);
  if (ticket_map_iter == g_ctc_mdl_ticket_maps.end()) {
    return;
  }

  map<string, MDL_ticket *> *ctc_mdl_ticket_map = ticket_map_iter->second;
  if (ctc_mdl_ticket_map == nullptr) {
    g_ctc_mdl_ticket_maps.erase(thd);
    return;
  }

  for (auto iter = ctc_mdl_ticket_map->begin(); iter != ctc_mdl_ticket_map->end();) {
    MDL_ticket *ticket = iter->second;
    if (ticket->get_type() == MDL_SHARED_READ_ONLY || ticket->get_type() == MDL_SHARED_NO_READ_WRITE) {
      thd->mdl_context.release_lock(ticket);
      ctc_mdl_ticket_map->erase(iter++);
    } else {
      iter++;
    }
  }

  if (ctc_mdl_ticket_map->empty()) {
    delete ctc_mdl_ticket_map;
    g_ctc_mdl_ticket_maps.erase(thd);
  }

  thd->restore_globals();
  my_thread_end();

  return;
}

void ctc_mdl_unlock_thd(ctc_handler_t *tch, ctc_lock_table_info *lock_info) {
  bool is_same_node = (tch->inst_id == ctc_instance_id);
  uint64_t mdl_thd_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, true);

  if (is_same_node) {
    return;
  }

  auto iter = g_ctc_mdl_thd_map.find(mdl_thd_key);
  if (iter == g_ctc_mdl_thd_map.end()) {
    return;
  }
  THD* thd = iter->second;
  assert(thd);
  my_thread_init();
  thd->store_globals();

  MDL_request ctc_release_request;
  ctc_init_mdl_request(lock_info, &ctc_release_request);
  ctc_mdl_unlock_thd_by_ticket(thd, &ctc_release_request);

  thd->restore_globals();
  my_thread_end();
  return;
}

int close_ctc_mdl_thd(uint32_t thd_id, uint32_t mysql_inst_id) {
  if (thd_id == 0) {
    if ((uint16_t)mysql_inst_id == (uint16_t)CANTIAN_DOWN_MASK) {
      /* 清理整个参天节点相关的THD */
      ctc_log_system("[CTC_MDL_THD]:Close All MDL THD on bad node by cantian_instance_id:%u",
        (uint16_t)(mysql_inst_id >> 16));
      release_ctc_mdl_thd_by_cantian_id((uint16_t)(mysql_inst_id >> 16));
    } else {
      /* 清理整个mysqld节点相关的THD */
      ctc_log_system("[CTC_MDL_THD]:Close All MDL THD by ctc_instance_id:%u", mysql_inst_id);
      release_ctc_mdl_thd_by_inst_id(mysql_inst_id);
    }
  } else {
    /* 通过把mysql_inst_id左移32位 与 thd_id拼接在一起 用来唯一标识一个THD */
    uint64_t mdl_thd_key = ctc_get_conn_key(mysql_inst_id, thd_id, true);
    ctc_log_note("[CTC_MDL_THD]: Close THD by conn_id=%u, ctc_instance_id=%u, proxy_conn_map_key=%lu",
                   thd_id, mysql_inst_id, mdl_thd_key);
    release_ctc_mdl_thd_by_key(mdl_thd_key);
  }
  return 0;
}

static void ctc_get_set_var_item(THD* new_thd, sys_var* sysvar, Item** res MY_ATTRIBUTE((unused)), string& var_value,
                                 bool is_null_value, bool var_is_int) {
  switch (sysvar->show_type()) {
    case SHOW_INT:
    case SHOW_LONG:
    case SHOW_LONGLONG:
    case SHOW_HA_ROWS:
      if (var_value.c_str()[0] != '-') {
        *res = new (new_thd->mem_root)
          Item_uint(var_value.c_str(), (uint)var_value.length());
        break;
      }
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
      if (var_is_int) {
	*res = new (new_thd->mem_root)
          Item_int(var_value.c_str(), (uint)var_value.length());
      } else {
        *res = new (new_thd->mem_root)
          Item_string(var_value.c_str(), var_value.length(),
                      &my_charset_utf8mb4_bin);
      }
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

#ifdef FEATURE_X_FOR_MYSQL_26
static void ctc_set_var_type(uint32_t option, set_var *var) {
  if ((option & CTC_SET_VARIABLE_PERSIST) > 0) {
    var->type = OPT_PERSIST;
  }
  if ((option & CTC_SET_VARIABLE_PERSIST_ONLY) > 0) {
    var->type = OPT_PERSIST_ONLY;
  }
}
#endif

static int ctc_init_thd_priv(THD** thd, Sctx_ptr<Security_context> *ctx) {
  my_thread_init();
  THD* new_thd = new (std::nothrow) THD;
  if (new_thd == nullptr) {
    return ERR_ALLOC_MEMORY;
  }
  new_thd->set_new_thread_id();
  new_thd->thread_stack = (char *)&new_thd;
  new_thd->store_globals();
  lex_start(new_thd);
  new_thd->security_context()->skip_grants();
  new_thd->set_query("ctc_mdl_thd_notify", 18);
 
  const std::vector<std::string> priv_list = {
      "ENCRYPTION_KEY_ADMIN", "ROLE_ADMIN", "SYSTEM_VARIABLES_ADMIN",
      "AUDIT_ADMIN", "PERSIST_RO_VARIABLES_ADMIN"};
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
  // Initializing session system variables.
  alloc_and_copy_thd_dynamic_variables(new_thd, true);
  (*thd) = new_thd;
  return 0;
}

int ctc_get_sys_var(THD *new_thd, ctc_set_opt_request *broadcast_req,
                    set_opt_info_t *set_opt_info,
                    List<set_var_base> &tmp_var_list,
                    Item *res) {
  set_var *var = nullptr;
  sys_var *sysvar = nullptr;
  string var_name = set_opt_info->var_name;
  string var_value = set_opt_info->var_value;

  sysvar = intern_find_sys_var(var_name.c_str(), var_name.length());
  if (sysvar == nullptr) {
    my_error(ER_UNKNOWN_SYSTEM_VARIABLE, MYF(0), var_name.c_str());
    strncpy(broadcast_req->err_msg, new_thd->get_stmt_da()->message_text(), ERROR_MESSAGE_LEN - 1);
    ctc_log_error("[ctc_set_sys_var]:sysvar is nullptr and var_name : %s", var_name.c_str());
    return ER_UNKNOWN_SYSTEM_VARIABLE;
  }

  string base_name_src = set_opt_info->base_name;
  LEX_CSTRING base_name = {nullptr, 0};
  if (strlen(base_name_src.c_str())) {
    base_name = {base_name_src.c_str(), strlen(base_name_src.c_str())};
  }
  bool is_default_value = ((set_opt_info->options) & (CTC_SET_VARIABLE_TO_DEFAULT)) != 0;
  bool is_null_value = ((set_opt_info->options) & (CTC_SET_VARIABLE_TO_NULL)) != 0;
  bool var_is_int = set_opt_info->var_is_int;
  enum_var_type type = OPT_GLOBAL;
  if (set_opt_info->options & CTC_SET_VARIABLE_PERSIST) {
    type = OPT_PERSIST;
  }
  if (set_opt_info->options & CTC_SET_VARIABLE_PERSIST_ONLY) {
    type = OPT_PERSIST_ONLY;
  }
  ctc_get_set_var_item(new_thd, sysvar, &res, var_value, is_null_value, var_is_int);
  
  if (is_default_value) {
    if (res) {
      res->cleanup();
    }
    res = nullptr;
  }
#ifdef FEATURE_X_FOR_MYSQL_26
  var = new (new_thd->mem_root) set_var(type, sysvar, base_name, res);
  ctc_set_var_type(set_opt_info->options, var);
#elif defined(FEATURE_X_FOR_MYSQL_32)
  System_variable_tracker var_tracker =
      System_variable_tracker::make_tracker(var_name.c_str());
  var = new (new_thd->mem_root) set_var(type, var_tracker, res);
#endif
  tmp_var_list.push_back(var);
  return 0;
}

int ctc_set_sys_var(ctc_set_opt_request *broadcast_req) {
  Sctx_ptr<Security_context> ctx;
  THD *new_thd = nullptr;
  int ret = 0;
  ret = ctc_init_thd_priv(&new_thd, &ctx);
  if (ret != 0 || new_thd == nullptr) {
    ctc_log_error("[ctc_set_sys_var]:init thd fail; ret: %u", ret);
    return ERR_ALLOC_MEMORY;
  }
  List<set_var_base> tmp_var_list;
  Item *res = nullptr;
  set_opt_info_t *set_opt_info = broadcast_req->set_opt_info;
  for (uint32_t i = 0; i < broadcast_req->opt_num; i++) {
    ret = ctc_get_sys_var(new_thd, broadcast_req, set_opt_info, tmp_var_list, res);
    if (ret != 0) {
      ctc_log_error("[ctc_get_sys_var]:get global var fail; ret: %u, var_name: %s, var_value: %s",
          ret, set_opt_info->var_name, set_opt_info->var_value);
      return ret;
    }
    ret = sql_set_variables(new_thd, &tmp_var_list, false);
    if (ret != 0) {
      uint err_code = new_thd->get_stmt_da()->mysql_errno();
      strncpy(broadcast_req->err_msg, new_thd->get_stmt_da()->message_text(), ERROR_MESSAGE_LEN - 1);
      ctc_log_error("[ctc_set_sys_var]:set global opt fail; err_code: %u, var_name: %s, var_value: %s",
          err_code, set_opt_info->var_name, set_opt_info->var_value);
      return err_code;
    }
    tmp_var_list.clear();
    if (res) {
      res->cleanup();
    }
    res = nullptr;
    set_opt_info += 1;
  }

  new_thd->free_items();
  lex_end(new_thd->lex);
  new_thd->release_resources();
  delete new_thd;

  my_thread_end();
  
  return ret;
}

int ctc_ddl_execute_lock_tables_by_req(ctc_handler_t *tch, ctc_lock_table_info *lock_info, int *err_code) {
// unlock tables before locking tables
  ctc_mdl_unlock_tables_thd(tch);

  bool is_same_node = (tch->inst_id == ctc_instance_id);
  uint64_t mdl_thd_key = ctc_get_conn_key(tch->inst_id, tch->thd_id, true);
 
  if (is_same_node) {
    return false;
  }
 
  THD *thd = nullptr;
  ctc_init_thd(&thd, mdl_thd_key);
 
  MDL_request ctc_mdl_request;
  ctc_init_mdl_request(lock_info, &ctc_mdl_request, MDL_key::TABLE);
 
  if (thd->mdl_context.acquire_lock(&ctc_mdl_request, 10)) {
    *err_code = ER_LOCK_WAIT_TIMEOUT;
    ctc_log_error("[CTC_MDL_LOCK]:Get mdl lock fail. namespace:%d, db_name:%s, table_name:%s",
                  lock_info->mdl_namespace, lock_info->db_name, lock_info->table_name);
    return true;
  }

  lock_guard<mutex> lock(m_ctc_mdl_ticket_mutex);
  auto iter = g_ctc_mdl_ticket_maps.find(thd);
  map<string, MDL_ticket *> *ctc_mdl_ticket_map = nullptr;
  if (iter == g_ctc_mdl_ticket_maps.end()) {
    ctc_mdl_ticket_map = new map<string, MDL_ticket *>;
    g_ctc_mdl_ticket_maps[thd] = ctc_mdl_ticket_map;
  } else {
    ctc_mdl_ticket_map = g_ctc_mdl_ticket_maps[thd];
  }
  string mdl_ticket_key;
  mdl_ticket_key.assign(((const char*)(ctc_mdl_request.key.ptr())), ctc_mdl_request.key.length());
  assert(mdl_ticket_key.length() > 0);
  ctc_mdl_ticket_map->insert(map<string, MDL_ticket *>::value_type(mdl_ticket_key, ctc_mdl_request.ticket));

  thd->restore_globals();
  my_thread_end();

  return false;
}