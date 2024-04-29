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

#ifndef __CTC_META_DATA_H__
#define __CTC_META_DATA_H__

#include <mysql.h>
#include "tse_srv.h"

#pragma GCC visibility push(default)

#define CTC_MDL_TIMEOUT (31536000)

int close_tse_mdl_thd(uint32_t thd_id, uint32_t mysql_inst_id);
int tse_mdl_lock_thd(tianchi_handler_t *tch, tse_lock_table_info *lock_info, int *err_code);
void tse_mdl_unlock_thd(tianchi_handler_t *tch, tse_lock_table_info *lock_info);
int ctc_set_sys_var(tse_ddl_broadcast_request *broadcast_req);
int tse_ddl_execute_lock_tables_by_req(tianchi_handler_t *tch, tse_lock_table_info *lock_info, int *err_code);
void tse_mdl_unlock_tables_thd(tianchi_handler_t *tch);

#pragma GCC visibility pop

#endif // __CTC_META_DATA_H__