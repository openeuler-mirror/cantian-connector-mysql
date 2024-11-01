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

#ifndef __CTC_SRV_MQ_MODULE__
#define __CTC_SRV_MQ_MODULE__

#include "ctc_srv.h"
#include "message_queue/dsw_shm.h"
#include "message_queue/dsw_message.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cpluscplus */

void *get_one_shm_inst(ctc_handler_t *tch);
int ctc_mq_register_func(void);
int ctc_mq_batch_send_message(void *shm_inst, CTC_FUNC_TYPE func_type, uint8_t* data_buf,
                              uint32_t send_data_len, uint32_t buf_size);

int ctc_mq_deal_func(void* shm_inst, enum CTC_FUNC_TYPE func_type, void* request,void* msg_buf,
                     uint32_t server_id = SERVER_PROC_ID, uint32_t wait_sec = 0);

#ifdef __cplusplus
}
#endif /* __cpluscplus */

#endif
