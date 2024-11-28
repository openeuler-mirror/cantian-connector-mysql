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

#include <stdio.h>  // vfprintf, stderr
#include <stdarg.h>
#include <regex>
#include "sql/log.h"
#include "sql/mysqld.h"
#include "my_dbug.h"

#ifndef __TSE_LOG_H__
#define __TSE_LOG_H__

#define TSE_LOG_SIZE 1024
#define UNUSED_PARAM(_p) (void(_p))  // supress warning declared but not used
#define __FILENAME__ (strrchr("/" __FILE__, '/') + 1)

#if 0
typedef enum en_regex_type_e
{
  REGEX_LINE,
  REGEX_SECURITY,
  REGEX_TOKEN,
  REGEX_PASSWORD
}regex_type_e;

typedef struct regex_conf
{
  regex_type_e  regex_type;
  const char regex[256];
}regex_conf_t;

static regex_conf_t g_regex_conf[] =
{
  {REGEX_SECURITY,  "security:\\S*"}, // 加密密钥
  {REGEX_TOKEN,     "[Tt][Oo][Kk][Ee][Nn]((\\S*)|(\\s*:\\s*)|(\\s*=\\s*)|(\\s*-\\s*)|(\\s*\\(\\s*)|(\\s*\\[\\s*)|(\\s*\\{\\s*))\\S*"}, // token
  {REGEX_PASSWORD,  "[Pp][Aa][Ss][Ss]([Ww]|[Ww][Dd]|[Ww][Oo][Rr][Dd])((\\S*)|(\\s*:\\s*)|(\\s*=\\s*)|(\\s*-\\s*)|(\\s*\\(\\s*)|(\\s*\\[\\s*)|(\\s*\\{\\s*))\\S*"} // password
};

static void replace_all(char *filter_str, const char *regex) {
  std::regex pattern(regex);
  std::string s(filter_str);
  s = std::regex_replace(s, pattern, "*****");
  if (s.length() >= TSE_LOG_SIZE) {
      s = s.substr(0, TSE_LOG_SIZE - 1);
  }
  strcpy(filter_str, s.c_str());
  filter_str[s.length()] = '\0';
  return;
}
#endif

/* System Level log */
#define tse_log_system(fmt, args...) sql_print_information("[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)

/* Error Level log */
#define tse_log_error(fmt, args...) sql_print_error("[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)

/* Warning Level log */
#define tse_log_warning(fmt, args...) sql_print_warning("[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)

/* note Level log */
#define tse_log_note(fmt, args...) sql_print_information("[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)
#define tse_log_verbose tse_log_note
#define tse_log_info    tse_log_note
#define tse_log_trivia  tse_log_note

/* debug only Level log */
#define tse_log_debug(fmt, args...) DBUG_PRINT("tse", (fmt, ##args))
#define tse_log_debug_only tse_log_debug

#define TSE_ENGINE_ERROR(message)                           \
  do {                                                      \
    tse_log_error(message);                                 \
    my_error(ER_CTC_ENGINE_ERROR, MYF(0), message);  \
  } while (0)

#define TSE_RETURN_IF_ERROR(_expVal, retVal)                \
    do {                                                    \
        bool _status_ = (_expVal);                          \
        if (!_status_) {                                    \
            tse_log_system("TSE_RETURN_IF_ERROR return");   \
            return retVal;                                  \
        }                                                   \
    } while (0)

#define TSE_RETURN_IF_NOT_ZERO(retVal)                      \
    do {                                                    \
        int ret = retVal;                                   \
        if (ret != 0) {                                     \
            tse_log_system("TSE_RETURN_IF_NOT_ZERO return"); \
            return ret;                                     \
        }                                                   \
    } while (0)

#define TSE_RET_ERR_IF_NULL(var)                            \
    do {                                                    \
        if (var == NULL) {                                  \
            return CT_ERROR;                                \
        }                                                   \
    } while (0)

#endif // __TSE_LOG_H__
