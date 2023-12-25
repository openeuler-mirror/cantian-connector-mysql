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
#ifndef __dsw_message_pub_h__
#define __dsw_message_pub_h__

#include <semaphore.h>
#include "dsw_typedef.h"
#include "dsw_list.h"
#ifdef __cplusplus
extern "C" {
#endif /* __cpluscplus */

#pragma pack(1)


#define SERVER_PROC_ID 0
#define SERVER_REGISTER_PROC_ID 1
#define CLIENT_PROC_ID 1


typedef struct {
    dsw_u32 type; /* Data type */
    dsw_u32 length;
} dsw_message_segment_desc_t;

#define DSW_MESSAGE_SEGMENT_NUM_MAX (512) // TODO: too long, rename


typedef struct token_latency_timestamp_s {
    dsw_u16 start_time;
    dsw_u16 latency;
} token_latency_timestamp_t;

/* reserved for updating
 * 1. detect msg use 6 bytes for apm lids
 * 2. unhealthy used the first 5 bytes, and negotiation used 6th byte
 * 3. 前面的#pragma pack(1)保证了内存一字节对齐
 */
typedef union message_head_reserved_s {
    dsw_u8 reserved[12];

    /* client模块:
     * update map when IOView is changed.
     * delete_all_osd_in_pool 1:删除池子内所有osd，0:删除指定osd
     */
    struct update_map_in_del_pool_s {
        dsw_u32 osd_id;
        dsw_u8 delete_all_osd_in_pool;
    } update_map_in_del_pool;

    dsw_u16 pool_id; // client 模块
    dsw_u8 io_type;  // vbp/kvs-client判断是否双活IO

    struct token_latency_s {
        dsw_u8 padding[6];                                 // padding for unhealthy_and_negoation
        token_latency_timestamp_t token_latency_timestamp; // rsm使用
    } token_latency;
} message_head_reserved_t;

/**
 * @ingroup Message
 * message时间戳阶段点：所有阶段点。
 */
typedef struct {
    sem_t sem;
    dsw_u32 magic;
    dsw_u32 version;
    dsw_u64 request_id; /* Request ID */
    dsw_u32 src_nid;    /* Source node ID */
    dsw_u32 dst_nid;    /* Destination node ID */
    dsw_u8 net_point;   /* Type of net point */
    dsw_u8 src_mid;
    dsw_u8 dst_mid;
    dsw_u32 cmd_type;  /* Command type */
    dsw_u16 slice_id;  /* Message slice ID */
    dsw_u16 priority;  /* Message priority */
    dsw_u16 handle_id; /* IO trace handle ID */
    dsw_u16 seg_num;
    dsw_message_segment_desc_t seg_desc[DSW_MESSAGE_SEGMENT_NUM_MAX];
    dsw_u32 crc;    /* crc of the msg */
    dsw_u64 try_id; /* try id */

    /* reserved for updating
     * 1. detect msg use 6 bytes for apm lids
     * 2. unhealthy used the first 5 bytes, and negotiation used 6th byte
     */
    message_head_reserved_t reserved_u;
    dsw_u32 head_crc; /* crc of msg head */
} dsw_message_head_t;

/*
 * Definition of message block
 *
 * A whole message block mainly cotains a message head and at most four message data. Meanwhile, the node which creates
 * the message block should record time stamp for the node (Notice: the time stamp is a 64 bits value generated by local
 * tick generator, and is only valid locally) The Time stamp will been initialized to be DSW_MESSAGE_TS_NORMAL as
 * default. If DSW_MESSAGE_TS_DROPED, the message will been discarded directly by net module.
 */
typedef struct {
    dsw_message_head_t head;
    void *seg_buf[DSW_MESSAGE_SEGMENT_NUM_MAX];
    dsw_u64 ts; /* Time stamp */
    dsw_u32 seg_buf_crc;
    list_head_t msg_node;
} dsw_message_block_t;

#ifdef __arm__
#pragma pack(0)
#else
#pragma pack()
#endif

#ifdef __cplusplus
}
#endif /* __cpluscplus */
#endif // __dsw_message_pub_h__
