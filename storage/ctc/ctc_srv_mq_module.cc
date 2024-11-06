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

#include "srv_mq_msg.h"
#include "ctc_srv_mq_module.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <atomic>
#include "assert.h"
#include "ctc_error.h"
#include "ctc_log.h"
#include "message_queue/dsw_shm.h"
#include "ctc_stats.h"
#include "ha_ctc.h"

#define MQ_THD_NUM 1
#define MAX_DDL_THD_NUM 1024
#define SHM_MAX_SEG_NUM 8
using namespace std;
#define CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, tag)                      \
  do {                                                                      \
    if ((req)->result != 0 && get_server_state() == SERVER_SHUTTING_DOWN) { \
      ctc_log_error("%s failed,server will shutdown:result:%d", (tag),      \
                    (req)->result);                                         \
      (req)->result = 0;                                                    \
    }                                                                       \
  } while (0)

#define CTC_GET_CLIENT_ID(inst_id) ((int) ((inst_id) & 0xFFFF))

// the last shm seg g_shm_segs[SHM_MAX_SEG_NUM] is for upstream, served by mysqld
uint32_t g_shm_file_num = 1;
shm_seg_s *g_shm_segs[SHM_MAX_SEG_NUM + 1] = {nullptr};
shm_seg_s *g_upstream_shm_seg = nullptr;
atomic_int g_ddl_thd_num(0);
int g_shm_client_id(-1);

static void* mq_msg_handler(void *arg) {
  pthread_detach(pthread_self());
  dsw_message_block_t *message_block = (dsw_message_block_t *)arg;
  ctc_log_note("recv msg ! cmd:%d", message_block->head.cmd_type);
  switch (message_block->head.cmd_type) {
    case CTC_FUNC_TYPE_EXECUTE_REWRITE_OPEN_CONN: {
      execute_ddl_mysql_sql_request *req = (execute_ddl_mysql_sql_request *)message_block->seg_buf[0];
      req->result = ctc_execute_rewrite_open_conn(req->thd_id, &req->broadcast_req);
      ctc_log_note("execute_rewrite_open_conn : sql_txt:%s,result:%d", req->broadcast_req.sql_str, req->result);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "ctc_ddl_execute_update");
      break;
    }
    case CTC_FUNC_TYPE_MYSQL_EXECUTE_UPDATE: {
      execute_ddl_mysql_sql_request *req =
          (execute_ddl_mysql_sql_request *)message_block->seg_buf[0];
      req->result = ctc_ddl_execute_update(req->thd_id, &req->broadcast_req, &req->allow_fail);
      ctc_log_system("[Disaster Recovery] execute_ddl_mysql_sql : db:%s, sql_txt:%s,result:%d", req->broadcast_req.db_name,
                   req->broadcast_req.sql_str, req->result);
      ctc_log_note("execute_ddl_mysql_sql : db:%s, sql_txt:%s,result:%d", req->broadcast_req.db_name,
                   req->broadcast_req.sql_str, req->result);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "ctc_ddl_execute_update");
      break;
    }
    case CTC_FUNC_TYPE_CLOSE_MYSQL_CONNECTION: {
      struct close_mysql_connection_request *req = (struct close_mysql_connection_request *)message_block->seg_buf[0];
      req->result = close_mysql_connection(req->thd_id, req->inst_id);
      ctc_log_note("close_connection : thd_id : %d, inst_id : %d, ret : %d.", req->thd_id, req->inst_id, req->result);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "close_mysql_connection");
      break;
    }
    case CTC_FUNC_TYPE_LOCK_TABLES: {
      struct ctc_lock_tables_request *req = (struct ctc_lock_tables_request *)message_block->seg_buf[0];
      req->result = ctc_ddl_execute_lock_tables(&(req->tch), req->db_name, &(req->lock_info), &(req->err_code));
      ctc_log_note("lock tables : thd_id : %d, inst_id : %d, ret : %d.", req->tch.thd_id, req->tch.inst_id,
                   req->result);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "ctc_ddl_execute_lock_tables");
      break;
    }
    case CTC_FUNC_TYPE_UNLOCK_TABLES: {
      struct ctc_unlock_tables_request *req = (struct ctc_unlock_tables_request *)message_block->seg_buf[0];
      req->result = ctc_ddl_execute_unlock_tables(&(req->tch), req->mysql_inst_id, &(req->lock_info));
      ctc_log_note("unlock tables : thd_id : %d, inst_id : %d, ret : %d.", req->tch.thd_id, req->tch.inst_id,
                   req->result);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "ctc_ddl_execute_unlock_tables");
      break;
    }
    case CTC_FUNC_TYPE_INVALIDATE_OBJECTS: {
      struct invalidate_mysql_dd_request *req = (struct invalidate_mysql_dd_request *)message_block->seg_buf[0];
      req->result = ctc_invalidate_mysql_dd_cache(&(req->tch), &req->broadcast_req, &(req->err_code));
      ctc_log_note("invalidate dd cache: thd_id : %d, inst_id : %d, ret : %d.", req->tch.thd_id, req->tch.inst_id,
                  req->result);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "ctc_invalidate_mysql_dd_cache");
      break;
    }
    case CTC_FUNC_SET_CLUSTER_ROLE_BY_CANTIAN: {
      struct set_cluster_role_by_cantian_request *req = (struct set_cluster_role_by_cantian_request *)message_block->seg_buf[0];
      req->result = ctc_set_cluster_role_by_cantian(req->is_slave);
      ctc_log_note("[Disaster Recovery] Set cluster role by cantian, is_slave:%d", req->is_slave);
      CTC_IGNORE_ERROR_WHEN_MYSQL_SHUTDOWN(req, "ctc_set_cluster_role_by_cantian");
      break;
    }
    default: {
      ctc_log_error("cmd type invalid, cmd_type:%d.", message_block->head.cmd_type);
      break;
    }
  }

  int result = sem_post(&message_block->head.sem);
  if (result != CT_SUCCESS) {
    ctc_log_error("sem post failed, result:%d.", result);
  }
  g_ddl_thd_num--;
  pthread_exit(0);
  return NULL;
}

int mq_recv_msg(struct shm_seg_s *shm_seg, dsw_message_block_t *message_block)
{
  (void)shm_seg;
  int result;
  pthread_t thd;
  if (g_ddl_thd_num > MAX_DDL_THD_NUM) {
    ctc_log_error("ddl thd has reach max limit: %d", MAX_DDL_THD_NUM);
    assert(0);
  }
  result = pthread_create(&thd, NULL, mq_msg_handler, (void*)message_block);
  if (result != 0) {
    ctc_log_error("pthread_create failed, result:%d.", result);
  }
  g_ddl_thd_num++;
  return result;
}

void shm_log_err(char *log_text, int length)
{
  UNUSED_PARAM(length);
  ctc_log_error("%s", log_text);
}

void shm_log_info(char *log_text, int length)
{
  UNUSED_PARAM(length);
  ctc_log_system("%s", log_text);
}

int mq_srv_start(int proc_id)
{
  g_upstream_shm_seg = g_shm_segs[g_shm_file_num];
  shm_set_thread_cool_time(0);
  return shm_proc_start(g_upstream_shm_seg, proc_id, MQ_THD_NUM, NULL, 0, mq_recv_msg);
}

bool init_shm_segment(int shm_num, int *inst_id)
{
  if (g_shm_segs[shm_num] != nullptr) {
    ctc_log_system("The segment g_shm_segs[%d] has already been initialized.", shm_num);
    return true;
  }
  shm_key_t shm_key{};
  shm_key.type = SHM_KEY_MMAP;
  std::string shm_name = MQ_SHM_MMAP_NAME_PREFIX;
  std::string map_name = MQ_SHM_MMAP_NAME_PREFIX + std::string(".") + std::to_string(shm_num);
  strcpy(shm_key.mmap_name, map_name.c_str());
  strcpy(shm_key.shm_name, shm_name.c_str());
  shm_key.seg_id = shm_num;

  if (shm_num == 0) {
    if (shm_client_connect(&shm_key, inst_id) < 0) {
      ctc_log_error("shm client connect failed, shm_name(%s)", shm_key.shm_name);
      return false;
    }
    ha_ctc_set_inst_id((uint32_t) *inst_id);
    g_shm_client_id = CTC_GET_CLIENT_ID(*inst_id);
  }

  g_shm_segs[shm_num] = shm_init(&shm_key, false);

  if (g_shm_segs[shm_num] == nullptr) {
    ctc_log_error("shm init failed, shm_seg is null shm_num:%d.", shm_num);
    return false;
  }

  shm_assign_proc_id(g_shm_segs[shm_num], g_shm_client_id);
  return true;
}

int init_ctc_mq_moudle()
{
  shm_set_info_log_writer(shm_log_info);
  shm_set_error_log_writer(shm_log_err);

  int inst_id = -1;
  if (shm_tpool_init(MQ_THD_NUM) != 0) {
    ctc_log_error("shm tpool init failed");
    return -1;
  }

  if (!init_shm_segment(0, &inst_id)) {
    ctc_log_error("shm init failed, shm_seg is null");
    return -1;
  }
  if (ctc_get_shm_file_num(&g_shm_file_num) != 0) {
    ctc_log_error("shm init g_shm_file_num failed");
    return -1;
  }
  for (uint32_t i = 1; i < g_shm_file_num + 1; ++i) {
    if (!init_shm_segment(i, &inst_id)) {
      ctc_log_error("shm init failed, init_shm_segment[%u] init failed.", i);
      return -1;
    }
  }
  return 0;
}

int deinit_ctc_mq_moudle()
{
  for (uint32_t i = 0; i < g_shm_file_num + 1; ++i) {
    if (g_shm_segs[i] == nullptr) {
      continue;
    }
    shm_seg_stop(g_shm_segs[i]);
  }
  shm_tpool_destroy();
  for (uint32_t i = 0; i < g_shm_file_num + 1; ++i) {
    if (g_shm_segs[i] == nullptr) {
      continue;
    }
    shm_seg_exit(g_shm_segs[i]);
  }
  shm_client_disconnect();
  return 0;
}

int (*ctc_init)() = init_ctc_mq_moudle;
int (*ctc_deinit)() = deinit_ctc_mq_moudle;

static int g_group_num = 0;
cpu_set_t g_masks[SHM_MAX_SEG_NUM];
static int g_cpu_info[SHM_SEG_MAX_NUM][SMALL_RECORD_SIZE];

void *get_one_shm_inst(ctc_handler_t *tch)
{
  uint32_t hashSeed = tch == nullptr ? 0 : tch->thd_id;
  if (g_group_num != 0 && tch != nullptr && tch->bind_core != 1) {
    cpu_set_t mask = g_masks[(hashSeed % g_shm_file_num) % g_group_num];
    pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
    tch->bind_core = 1;
  }
   return (void *)g_shm_segs[hashSeed % g_shm_file_num];
}

void *alloc_share_mem(void *seg, uint32_t mem_size)
{
  return shm_alloc((shm_seg_s *)seg, mem_size);
}

void free_share_mem(void *seg, void *shm_mem)
{
  UNUSED_PARAM(seg);
  shm_free(nullptr, shm_mem);
}

void batch_free_shm_buf(void *seg, dsw_message_block_t* msg)
{
  UNUSED_PARAM(seg);
  for (uint16_t i = 0; i < msg->head.seg_num; ++i) {
    if (msg->seg_buf[i]) {
      shm_free(nullptr, msg->seg_buf[i]);
    } else {
      ctc_log_error("seg buf is null, i:%u, seg_num:%u.", i, msg->head.seg_num);
    }
  }
}

static void set_cpu_info(register_instance_request *req)
{
  g_group_num = req->group_num;
  for (int i = 0; i < g_group_num; i++) {
    for (int j = 0; j < SMALL_RECORD_SIZE; j++) {
      g_cpu_info[i][j] = req->cpu_info[i][j];
      if (g_cpu_info[i][j] < 0) {
        break;
      }
    }
  }
}

static void set_cpu_mask()
{
  for (int i = 0; i < g_group_num; i++) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int j = 0; j < SHM_SEG_MAX_NUM; j++) {
      if (g_cpu_info[i][j] < 0) {
        break;
      }
      CPU_SET(g_cpu_info[i][j], &mask);
    }
    g_masks[i] = mask;
  }
}

static void ctc_log_reg_error_by_code(int error_code)
{
  switch(error_code) {
    case ERR_CONNECTION_FAILED:
#ifdef WITH_CANTIAN
      ctc_log_error("connection failed");
#else
      ctc_log_error("shm connection failed");
#endif
      break;
    case REG_MISMATCH_CTC_VERSION:
      ctc_log_error("CTC client version mismatch server!");
      break;
    default:
      ctc_log_error("recv unknown register instance error code %d", error_code);
      break;
  }
}

EXTER_ATTACK int ctc_mq_deal_func(void *shm_inst, CTC_FUNC_TYPE func_type,
                                  void *request, void* msg_buf, uint32_t server_id, uint32_t wait_sec)
{
  uint64_t start_time = 0;
  if (ctc_stats::get_instance().get_statistics_enabled()) {
    start_time = my_getsystime() / 10;
  }

  shm_seg_s *seg = (shm_seg_s *)shm_inst;
  dsw_message_block_t *msg;
  bool is_alloc = false;
  if (msg_buf != nullptr) {
    msg =  (dsw_message_block_t *)msg_buf;
  } else {
    msg = (dsw_message_block_t *)shm_alloc(seg, sizeof(dsw_message_block_t));
    is_alloc = true;
  }
  
  if (msg == nullptr) {
    ctc_log_error("alloc shm failed, len:%lu.", sizeof(dsw_message_block_t));
    return ERR_ALLOC_MEMORY;
  }
  int ret = CT_SUCCESS;
  if (is_alloc) {
    ret = sem_init(&msg->head.sem, 1, 0);
  }
  
  if (ret != CT_SUCCESS) {
    if (is_alloc) {
      shm_free(nullptr, msg);
    }
    ctc_log_error("sem init failed, ret:%d, func_type:%d.", ret, func_type);
    return ERR_SEM_FAULT;
  }

  msg->head.src_nid = g_shm_client_id;
  msg->head.dst_nid = server_id;
  msg->head.seg_num = 1;
  msg->head.seg_desc[0].type = 0;
  msg->head.seg_desc[0].length = REQUEST_SIZE;
  msg->head.cmd_type = (uint32_t)func_type;
  msg->seg_buf[0] = request;
  
  ct_errno_t result = CT_SUCCESS;
  do {
    ret = shm_send_msg(seg, server_id, msg);
    if (ret != CT_SUCCESS) {
      result = ERR_SHM_SEND_MSG_FAILED;
      ctc_log_error("send msg failed, ret:%d, func_type:%d.", ret, func_type);
      break;
    }
    
    // register funcs won't relinquish the processor
    if (func_type < CTC_FUNC_TYPE_REGISTER_INSTANCE) {
      sched_yield();
    }
    
    if (wait_sec > 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += wait_sec;
      ret = sem_timedwait(&msg->head.sem, &ts);
    } else {
      ret = sem_wait(&msg->head.sem);
    }

    if (ret != CT_SUCCESS) {
      result = ERR_SEM_FAULT;
      ctc_log_error("send msg sem wait failed, ret:%d, func_type:%d.", ret, func_type);
      break;
    }
  } while (0);

  if (is_alloc) {
    ret = sem_destroy(&msg->head.sem);
    if (ret != CT_SUCCESS) {
      ctc_log_error("sem destory failed, ret:%d, func_type:%d.", ret, func_type);
    }
    shm_free(nullptr, msg);
  }
  

  if (ctc_stats::get_instance().get_statistics_enabled()) {
    ctc_stats::get_instance().gather_stats(func_type, my_getsystime() / 10 - start_time);
  }

  return result;
}

int ctc_mq_register_func(void)
{
  mq_srv_start(g_shm_client_id);
  
  shm_seg_s *shm_inst = (shm_seg_s *)get_one_shm_inst(nullptr);
  register_instance_request *req = (register_instance_request *)alloc_share_mem(shm_inst, sizeof(register_instance_request));
  if (req == NULL) {
    ctc_log_error("[CTC_INIT]: alloc shm mem error, shm_inst(%p), size(%lu)", shm_inst, sizeof(register_instance_request));
    return ERR_ALLOC_MEMORY;
  }
  req->ctc_version = (uint32_t)CTC_CLIENT_VERSION_NUMBER;

  int result = ERR_CONNECTION_FAILED;
  if (ctc_mq_deal_func(shm_inst, CTC_FUNC_TYPE_REGISTER_INSTANCE, req, nullptr, SERVER_REGISTER_PROC_ID) == CT_SUCCESS) {
    result = req->result;
  }
  
  if (result == CT_SUCCESS) {
    set_cpu_info(req);
    set_cpu_mask();
  } else {
    ctc_log_reg_error_by_code(result);
  }

  shm_free(nullptr, req);
  return result;
}

int ctc_mq_get_batch_data(dsw_message_block_t *message_block, uint8_t *buf_data, uint32_t buf_len)
{
  uint32_t use_buf_len = 0;
  for (uint16_t i = 0; i < message_block->head.seg_num; ++i) {
    if (message_block->head.seg_desc[i].length > CTC_MQ_MESSAGE_SLICE_LEN) {
      ctc_log_error("seg_data length error, seg_len:%u.", message_block->head.seg_desc[i].length);
      return ERR_GENERIC_INTERNAL_ERROR;
    }

    if (use_buf_len + message_block->head.seg_desc[i].length > buf_len) {
      ctc_log_error("buf len error, buf_len:%u, use_buf_len:%u.", buf_len, use_buf_len);
      return ERR_GENERIC_INTERNAL_ERROR;
    }

    if (message_block->seg_buf[i] == nullptr) {
      ctc_log_error("buf_data error, seg_buf[%u] is null.", i);
      return ERR_GENERIC_INTERNAL_ERROR;
    }

    memcpy(buf_data + use_buf_len, message_block->seg_buf[i], message_block->head.seg_desc[i].length);
    use_buf_len += message_block->head.seg_desc[i].length;
  }
  return CT_SUCCESS;
}

static int ctc_mq_fill_batch_data(shm_seg_s *seg, dsw_message_block_t* msg, uint8_t* data_buf,
                                  uint32_t data_len, uint32_t buf_size)
{
  assert(data_len <= buf_size);

  uint8_t* buf_tmp = data_buf;
  msg->head.seg_num = 0;
  uint16_t i;
  uint32_t tmp_len = buf_size;
  uint32_t copy_len;
  while (tmp_len > 0) {
    i = msg->head.seg_num;
    if (tmp_len > CTC_MQ_MESSAGE_SLICE_LEN) {
      msg->head.seg_desc[i].length = CTC_MQ_MESSAGE_SLICE_LEN;
    } else {
      msg->head.seg_desc[i].length = tmp_len;
    }
    tmp_len -= msg->head.seg_desc[i].length;

    msg->head.seg_desc[i].type = 0;
    msg->seg_buf[i] = shm_alloc(seg, msg->head.seg_desc[i].length);
    if (msg->seg_buf[i] == nullptr) {
      ctc_log_error("Alloc seg_buf failed, seg_num:%u, buf_len:%u.", i, msg->head.seg_desc[i].length);
      batch_free_shm_buf(seg, msg);
      return ERR_ALLOC_MEMORY;
    }

    if (data_len > 0) {
      copy_len = data_len > CTC_MQ_MESSAGE_SLICE_LEN ? CTC_MQ_MESSAGE_SLICE_LEN : data_len;
      data_len -= copy_len;

      memcpy(msg->seg_buf[i], buf_tmp, copy_len);
      buf_tmp += copy_len;
    }
    ++msg->head.seg_num;
  }
  return CT_SUCCESS;
}

EXTER_ATTACK int ctc_mq_batch_send_message(void *shm_inst, CTC_FUNC_TYPE func_type, uint8_t* data_buf,
                                           uint32_t send_data_len, uint32_t buf_size)
{
  if (buf_size > CTC_MQ_MESSAGE_SLICE_LEN * DSW_MESSAGE_SEGMENT_NUM_MAX) {
    ctc_log_error("batch send message failed, data_len:%u.", buf_size);
    return ERR_GENERIC_INTERNAL_ERROR;
  }

  shm_seg_s *seg = (shm_seg_s *)shm_inst;
  dsw_message_block_t* msg = (dsw_message_block_t*)shm_alloc(seg, sizeof(dsw_message_block_t));
  if (msg == nullptr) {
    ctc_log_error("alloc shm failed, len:%lu.", sizeof(dsw_message_block_t));
    return ERR_ALLOC_MEMORY;
  }

  ct_errno_t result = CT_SUCCESS;
  int ret = sem_init(&msg->head.sem, 1, 0);
  if (ret != 0) {
    shm_free(nullptr, msg);
    result = ERR_SEM_FAULT;
    ctc_log_error("sem init failed, ret(%d).", ret);
    return result;
  }

  do {
    ret = ctc_mq_fill_batch_data(seg, msg, data_buf, send_data_len, buf_size);
    if (ret != CT_SUCCESS) {
      result = ERR_BATCH_DATA_HANDLE_FAILED;
      break;
    }

    msg->head.src_nid = g_shm_client_id;
    msg->head.dst_nid = SERVER_PROC_ID;
    msg->head.cmd_type = (uint32_t)func_type;
    ret = shm_send_msg(seg, SERVER_PROC_ID, msg);
    if (ret != CT_SUCCESS) {
      result = ERR_SHM_SEND_MSG_FAILED;
      ctc_log_error("batch send msg failed, ret:%d, func_type:%d.", ret, func_type);
      break;
    }

    sched_yield();
    ret = sem_wait(&msg->head.sem);
    if (ret != CT_SUCCESS) {
      result = ERR_SEM_FAULT;
      ctc_log_error("batch send msg sem wait failed, ret:%d, func_type:%d.", ret, func_type);
      break;
    }

    ret = ctc_mq_get_batch_data(msg, data_buf, buf_size);
    if (ret != 0) {
      result = ERR_BATCH_DATA_HANDLE_FAILED;
      ctc_log_error("get batch data error, data_buf:%p, data_len:%u.", data_buf, buf_size);
      break;
    }
  } while (0);

  ret = sem_destroy(&msg->head.sem);
  if (ret != CT_SUCCESS) {
    ctc_log_error("sem destory failed, ret:%d, func_type:%d.", ret, func_type);
  }
  batch_free_shm_buf(seg, msg);
  shm_free(nullptr, msg);
  return result;
}
