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
#include <mysql/components/services/log_builtins.h>
#include "my_dbug.h"
#include "mysqld_error.h"

#ifndef __TSE_LOG_H__
#define __TSE_LOG_H__

#define TSE_LOG_SIZE 1024
#define UNUSED_PARAM(_p) (void(_p))  // supress warning declared but not used
#define __FILENAME__ (strrchr("/" __FILE__, '/') + 1)

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

static void do_security_filter(char *filter_str) {
  for (uint index = 0; index < sizeof(g_regex_conf) / sizeof(g_regex_conf[0]); index++) {
    replace_all(filter_str, g_regex_conf[index].regex);
  }
}

/*
  Print message to MySQL Server's error log(s)  

  @param loglevel    Selects the loglevel used when
                     printing the message to log.
  @param[in]  fmt    printf-like format string
  @param[in]  ap     Arguments

*/

inline void tse_log_print(enum loglevel loglevel, const char *fmt, ...) MY_ATTRIBUTE((format(printf, 2, 3)));

void tse_log_print(enum loglevel loglevel, const char *fmt, ...) {
  if (log_error_verbosity < loglevel) return;
  assert(fmt);
  va_list args;
  char msg_buf[TSE_LOG_SIZE];

  va_start(args, fmt);
  int res = vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
  va_end(args);

  assert(res);
  if (res >= TSE_LOG_SIZE) {
    msg_buf[TSE_LOG_SIZE - 2] = '.';
    msg_buf[TSE_LOG_SIZE - 3] = '.';
    msg_buf[TSE_LOG_SIZE - 4] = '.';
  }

  if (!opt_general_log_raw) { //配置文件中的log-raw
    do_security_filter(msg_buf);
  }

  LogErr(loglevel, ER_IB_MSG_1381, msg_buf);
}

/* System Level log */
#define tse_log_system(fmt, args...) tse_log_print(SYSTEM_LEVEL, "[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)

/* Error Level log */
#define tse_log_error(fmt, args...) tse_log_print(ERROR_LEVEL, "[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)

/* Warning Level log */
#define tse_log_warning(fmt, args...) tse_log_print(WARNING_LEVEL, "[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)

/* note Level log */
#define tse_log_note(fmt, args...) tse_log_print(INFORMATION_LEVEL, "[%s:%s(%d)]" fmt, __FILENAME__, __FUNCTION__, __LINE__, ##args)
#define tse_log_verbose tse_log_note
#define tse_log_info    tse_log_note
#define tse_log_trivia  tse_log_note

/* debug only Level log */
#define tse_log_debug(fmt, args...) DBUG_PRINT("tse", (fmt, ##args))
#define tse_log_debug_only tse_log_debug

#define TSE_ENGINE_ERROR(message)                           \
  do {                                                      \
    tse_log_error(message);                                 \
    my_error(ER_IB_MSG_1381, MYF(0), message);  \
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

#endif // __TSE_LOG_H__
