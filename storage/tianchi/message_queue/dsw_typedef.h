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

#ifndef __dsw_typedef_pub_h__
#define __dsw_typedef_pub_h__

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cpluscplus */

typedef int dsw_int;
typedef int dsw_bool;
typedef long dsw_long;

typedef int8_t dsw_s8;
typedef uint8_t dsw_u8;
typedef int16_t dsw_s16;
typedef uint16_t dsw_u16;
typedef int32_t dsw_s32;
typedef uint32_t dsw_u32;
typedef int64_t dsw_s64;
typedef uint64_t dsw_u64;


#define PACKFLAG __attribute__((__packed__))


#define DSW_TYPEOF(x) typeof((x))


extern void __dsw_assert(dsw_u64 exp);
extern void __dsw_assert_inner(dsw_u64 exp);
/*lint --emacro(506, DSW_ASSERT) --emacro(571, DSW_ASSERT)*/
#define DSW_ASSERT(x)               \
    {                               \
        __dsw_assert((dsw_u64)(x)); \
    }
/*lint --emacro(506, DSW_ASSERT_INNER) --emacro(571, DSW_ASSERT_INNER)*/
#define DSW_ASSERT_INNER(x)               \
    {                                     \
        __dsw_assert_inner((dsw_u64)(x)); \
    }


#define DSW_THREAD_MUTEX_INIT(mutex, attr)                      \
    do {                                                        \
        int inner_retval = pthread_mutex_init((mutex), (attr)); \
        if ((0 != inner_retval) && (EBUSY != inner_retval)) {   \
            DSW_ASSERT_INNER(0);                                \
        }                                                       \
    } while (0)

#define DSW_THREAD_MUTEX_LOCK(mutex)                            \
    do {                                                        \
        int inner_retval = pthread_mutex_lock((mutex));         \
        if ((0 != inner_retval) && (EDEADLK != inner_retval)) { \
            DSW_ASSERT_INNER(0);                                \
        }                                                       \
    } while (0)

#define DSW_THREAD_MUTEX_UNLOCK(mutex)                        \
    do {                                                      \
        int inner_retval = pthread_mutex_unlock((mutex));     \
        if ((0 != inner_retval) && (EPERM != inner_retval)) { \
            DSW_ASSERT_INNER(0);                              \
        }                                                     \
    } while (0)

#ifdef __cplusplus
}
#endif /* __cpluscplus */

#endif // __dsw_typedef_pub_h__
