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

#ifndef __CTC_PROXY_UTIL_H__
#define __CTC_PROXY_UTIL_H__

#include <mysql.h>

#pragma GCC visibility push(default)

using namespace std;

#define AGENT_CONN_KEY_MASK (0x8000000000000000ULL)

// this api can called by both CTC storage level(ha_ctc.so) and ctc_proxy.so, mysql_close C API used mysqlclient
void ctc_close_mysql_conn(MYSQL **curr_conn);

/*
  conn_key format:
  
  |-----------------|---------------|-------------|
  |16bits cantian_id|16bits mysql_id|             |
  |-----------------|---------------|32bits thd_id|
  |       32bits mysql_inst_id      |             |
  |---------------------------------|-------------|
  |   64 bits conn_key (first bit is proxy mask)  |
  |-----------------------------------------------|
  
  because cantiand_id ranges from [0, 1]
  use the first bit of conn_key to represent if this conn is proxy or agent (0: proxy, 1: agent)
*/
inline uint64_t ctc_get_conn_key(uint32_t inst_id, uint32_t thd_id, bool use_proxy) {
  uint64_t conn_key = (((uint64_t)(inst_id)) << 32) + thd_id;
  if (!use_proxy) {
    conn_key |= AGENT_CONN_KEY_MASK;
  }
  return conn_key;
}

inline uint32_t ctc_get_inst_id_from_conn_key(uint64_t conn_key) {
  return (conn_key & (AGENT_CONN_KEY_MASK - 1)) >> 32;
}

inline uint16_t ctc_get_cantian_id_from_conn_key(uint64_t conn_key) {
  return (conn_key & (AGENT_CONN_KEY_MASK - 1)) >> 48;
}

int ctc_init_agent_client(MYSQL *&curr_conn);
int ctc_mysql_conn(MYSQL *&con, const char *host, const char *user, const char *passwd);

uint32_t get_g_name_locks(void);
uint32_t add_g_name_locks(uint32_t num);
uint32_t sub_g_name_locks(uint32_t num);
int ctc_mysql_query(MYSQL *mysql, const char *query);

#pragma GCC visibility pop

#endif // __CTC_PROXY_UTIL_H__