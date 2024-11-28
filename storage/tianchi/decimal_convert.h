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
#include <limits.h>
#include "my_dbug.h"
#include <cstdint>
#include "string.h"

#ifndef DECIMAL_CONVERT_H
#define DECIMAL_CONVERT_H
#define DEC8_EXPN_UNIT 8
/* The number of cells (an uint32) used to store the decimal type. */
#define DEC8_CELL_SIZE (uint8)9
#define DEC4_CELL_SIZE (uint8)18
#define CT_MAX_DEC_OUTPUT_ALL_PREC (int32)72
#define CT_FALSE (uint8)0
#define CT_TRUE (uint8)1
#define MAX_NUMERIC_EXPN (int32)127
#define MIN_NUMERIC_EXPN (int32) - 127
#define MAX_NUMERIC_BUFF 65
#define CT_NUMBER_BUFFER_SIZE (uint32)128
#define SEXP_2_D8EXP(sci_exp) (int16)((sci_exp) / DEC8_EXPN_UNIT)
/* The number of digits that an element of an int256, i.e., an uint32
can encode. This indicates each uint32 can record at most DEC_ELEM_DIGIT
digits. Its value 9 is the upper, since 10^(9+1) < 2^32 < 10^11  */
#define DEC8_CELL_DIGIT 8
/* The the mask used to handle each cell. It is equal to 10^DEC_CELL_DIGIT */
#define DEC8_CELL_MASK 100000000U
/* The the mask used to handle each cell. It is equal to 10^DEC4_CELL_DIGIT */
#define DEC4_CELL_MASK 10000U

/* the maximal precision that stored into DB */
#define DEC_MAX_NUM_SAVING_PREC (int32)72

#define SIZE_K(n) (uint32)((n)*1024)
#define SIZE_M(n) (1024 * SIZE_K(n))
/** The format to print a cell */
#define DEC8_CELL_FMT "%08u"
#define CM_DEFAULT_DIGIT_RADIX 10
#define CM_IS_ZERO(c) ((c) == '0')
#define CM_IS_DIGIT(c) ((c) >= '0' && ((c) <= '9'))
#define CM_IS_DOT(c) ((c) == '.')
#define CM_IS_EXPN_CHAR(c) ((c) == 'e' || ((c) == 'E'))
#define CM_IS_SIGN_CHAR(c) ((c) == '-' || ((c) == '+'))
#define ZERO_DEC_HEAD(dec) (dec)->head = 0
#undef MIN
#define MIN(A, B) ((B) < (A) ? (B) : (A))
/** Convert a digital char into numerical digit */
#define CM_C2D(c) ((c) - '0')
#define D8EXP_2_SEXP(dexp) ((dexp)*DEC8_EXPN_UNIT)
/* DEC_MAX_ALLOWED_PREC = DEC_CELL_SIZE * DEC_CELL_DIGIT indicates the maximal
precision that a decimal can capture at most */
#define DEC8_MAX_ALLOWED_PREC (DEC8_CELL_SIZE * DEC8_CELL_DIGIT)
/** Append a char at the end of text */
#define CM_TEXT_APPEND(text, c) (text)->str[(text)->len++] = (c)
#define NUMPART_IS_ZERO(np) \
  ((np)->digit_text.len == 1 && CM_IS_ZERO((np)->digit_text.str[0]))
/* Get the scientific exponent of a decimal6 */
/* Get the scientific exponent of a decimal when given its exponent and
 * precision */
#define DEC8_GET_SEXP_BY_PREC0(sexp, prec0) ((int32)(sexp) + (int32)(prec0)-1)
/* Get the scientific exponent of a decimal when given its exponent and cell0 */
#define DEC8_GET_SEXP_BY_CELL0(sexp, c8_0) \
  DEC8_GET_SEXP_BY_PREC0(sexp, cm_count_u32digits(c8_0))
#define DEC8_GET_SEXP(dec) \
  DEC8_GET_SEXP_BY_CELL0(D8EXP_2_SEXP((dec)->expn), ((dec)->cells[0]))
/* Get the position of n-th digit of an dec8, when given precision
 * of cell0 (i.e., the position of the dot).
 * @note Both n and the pos begin with 0 */
#define DEC8_POS_N_BY_PREC0(n, prec0) \
  ((n) + (int32)DEC8_CELL_DIGIT - (int32)(prec0))
/* Get the tail address of a text */
#define CM_GET_TAIL(text) ((text)->str + (text)->len)
/** Clear all characters of the text */
#define CM_TEXT_CLEAR(text) (text)->len = 0
/* if the condition is true, throw return the value.
 * Note: this Macro used to reduce Circle Complexity */
#define CT_THROW(cond, value) \
  do {                        \
    if (cond) {               \
      return (value);         \
    }                         \
  } while (0)
#define CM_ZERO_NUMPART(np)         \
  do {                              \
    (np)->digit_text.str[0] = '0';  \
    (np)->digit_text.str[1] = '\0'; \
    (np)->digit_text.len = 1;       \
  } while (0)
#define CM_NULL_TERM(text) \
  { (text)->str[(text)->len] = '\0'; }
#define CT_RETURN_IF_ERROR(ret)          \
  do {                                   \
    status_t _status_ = (ret);           \
    if (_status_ != CT_SUCCESS_STATUS) { \
      return _status_;                   \
    }                                    \
  } while (0)
/* To decide whether a decimal is zero */
#define DECIMAL_IS_ZERO(dec) ((dec)->ncells == 0)
/* +/- */
#define DEC_SIGN_PLUS (uint8)0
#define DEC_SIGN_MINUS (uint8)1
typedef uint32 c8typ_t;
typedef uint64 cc8typ_t;
typedef c8typ_t cell8_t[DEC8_CELL_SIZE];
typedef unsigned int bool32;
typedef struct st_dec8 {
  union {
    struct {
      uint8 sign;   /* 0: for positive integer; 1: for negative integer */
      uint8 ncells; /* number of cells, 0: for unspecified precision */
      int16 expn;   /* the exponent of the number */
    };
    c8typ_t head;
  };
  cell8_t cells;
} dec8_t;
#pragma pack(4)
typedef struct st_text {
  char *str;
  uint32 len;
} text_t;
#pragma pack()
typedef uint16 c4typ_t;
typedef uint32 cc4typ_t;
typedef c4typ_t cell4_t[DEC4_CELL_SIZE];
#pragma pack(2)
typedef struct st_dec4 {
  union {
    struct {
      uint8 sign : 1;   /* 0: for positive integer; 1: for negative integer */
      uint8 ncells : 7; /* number of cells, 0: for unspecified precision */
      int8 expn;        /* the exponent of the number */
    };
    c4typ_t head;
  };
  cell4_t cells;
} dec4_t;
#pragma pack()
typedef enum en_status {
  CT_ERROR = -1,
  CT_SUCCESS_STATUS = 0,
  CT_TIMEDOUT = 1,
} status_t;
typedef enum en_num_errno {
  NERR_SUCCESS = 0,  // CT_SUCCESS
  NERR_ERROR,        /* error without concrete reason */
  NERR_INVALID_LEN,
  NERR_NO_DIGIT,
  NERR_UNEXPECTED_CHAR,
  NERR_NO_EXPN_DIGIT,
  NERR_EXPN_WITH_NCHAR,
  NERR_EXPN_TOO_LONG,
  NERR_EXPN_OVERFLOW,
  NERR_OVERFLOW,
  NERR_UNALLOWED_NEG,
  NERR_UNALLOWED_DOT,
  NERR_UNALLOWED_EXPN,
  NERR_MULTIPLE_DOTS,
  NERR_EXPECTED_INTEGER,
  NERR_EXPECTED_POS_INT,
  NERR__NOT_USED__ /* for safely accessing the error information */
} num_errno_t;
typedef enum en_num_flag {
  NF_NONE = 0x0,
  NF_NEGATIVE_SIGN = 0x0001,                    /* `-` */
  NF_POSTIVE_SIGN = 0x0002,                     /* `+` */
  NF_SIGN = NF_NEGATIVE_SIGN | NF_POSTIVE_SIGN, /* `+` */
  NF_DOT = 0x0004,                              /* `.` */
  NF_EXPN = 0x0008,                             /* `E` or `e` */
  NF_SZ_INDICATOR = 0x0010,                     /* B, K, M, G, T, P, E */
  NF_ALL = 0xFFFF
} num_flag_t;
#define CT_MAX_NUM_PART_BUFF (CT_MAX_DEC_OUTPUT_ALL_PREC)
typedef struct st_digitext {
  char str[CT_MAX_NUM_PART_BUFF];
  uint32 len;
} digitext_t;
typedef struct st_num_part {
  bool32 is_neg;
  bool32 has_dot;
  bool32 has_expn;
  bool32 do_round;
  int32 sci_expn;
  /* indicating which num flag should be excluded, it should be specified
   * before parsing. */
  uint32 excl_flag;
  digitext_t digit_text;
  /* for parse size type (unsigned integer with [K|M|G|T|P...]) */
  char sz_indicator;
} num_part_t;
#if defined(SECUREC_NEED_BUILTIN_EXPECT_DECLARE)
long __builtin_expect(long exp, long c);
#endif
#define SECUREC_LIKELY(x) __builtin_expect(!!(x), 1)
#define SECUREC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SECUREC_LIKELY(x) (x)
#define SECUREC_UNLIKELY(x) (x)
#endif
/* success */
#ifndef EOK
#define EOK 0
#endif
status_t ct_cm_str_to_dec8(const char *str, dec8_t *dec);
status_t ct_cm_text_to_dec8(const text_t *text, dec8_t *dec);
num_errno_t ct_cm_split_num_text(const text_t *num_text, num_part_t *np);
num_errno_t ct_cm_numpart_to_dec8(num_part_t *np, dec8_t *dec);
status_t ct_cm_dec8_to_str(const dec8_t *dec, int max_len, char *str);
status_t ct_cm_dec8_to_text(const dec8_t *dec, int32 max_len, text_t *text);
static inline void ct_cm_rtrim_text(text_t *text) {
  int32 index;
  if (text->str == NULL) {
    text->len = 0;
    return;
  } else if (text->len == 0) {
    return;
  }
  index = (int32)text->len - 1;
  while (index >= 0) {
    if ((uchar)text->str[index] > (uchar)' ') {
      text->len = (uint32)(index + 1);
      return;
    }
    --index;
  }
}
static inline void ct_cm_ltrim_text(text_t *text) {
  if (text->str == NULL) {
    text->len = 0;
    return;
  } else if (text->len == 0) {
    return;
  }
  while (text->len > 0) {
    if ((uchar)*text->str > ' ') {
      break;
    }
    text->str++;
    text->len--;
  }
}
static inline void ct_cm_trim_text(text_t *text) {
  ct_cm_ltrim_text(text);
  ct_cm_rtrim_text(text);
}
static inline uint32 cm_count_u32digits(uint32 u32) {
  // Binary search
  if (u32 >= 100000u) {
    if (u32 >= 10000000u) {
      return (u32 < 100000000u) ? 8 : ((u32 >= 1000000000u) ? 10 : 9);
    }
    return (u32 >= 1000000u) ? 7 : 6;
  }
  if (u32 >= 1000u) {
    return (uint32)((u32 >= 10000u) ? 5 : 4);
  }
  return (uint32)((u32 >= 100u) ? 3 : ((u32 >= 10u) ? 2 : 1));
}
static inline void cm_zero_dec8(dec8_t *dec) { ZERO_DEC_HEAD(dec); }
/**
 * Convert a single cell text into uint32. A single cell text is a text of
 * digits, with the number of text is no more than 9
 */
static inline uint32 cm_celltext2uint32(const text_t *cellt) {
  uint32 val = 0;
  for (uint32 i = 0; i < cellt->len; ++i) {
    val = val * 10 + (uint32)(uint8)CM_C2D(cellt->str[i]);
  }
  return val;
}
static inline void cm_dec8_trim_zeros(dec8_t *dec) {
  while (dec->ncells > 0 && dec->cells[dec->ncells - 1] == 0) {
    --dec->ncells;
  }
}
static inline void cm_zero_dec4(dec4_t *dec) { ZERO_DEC_HEAD(dec); }
static inline uint32 cm_dec4_stor_sz(const dec4_t *d4) {
  return ((uint32)(1 + (d4)->ncells)) * sizeof(c4typ_t);
}
// Decode a decimal from a void data with size
static inline void ct_cm_dec_4_to_8(dec8_t *d8, const dec4_t *d4,
                                    uint32 sz_byte) {
  // check validation again
  if ((uint32)cm_dec4_stor_sz(d4) > sz_byte) {
    assert(0);
  }
  if (DECIMAL_IS_ZERO(d4)) {
    cm_zero_dec8(d8);
    return;
  }
  uint32 i4 = 0;
  uint32 i8 = 0;
  d8->sign = d4->sign;
  if (d4->expn < 0) {
    d8->expn = (d4->expn - 1) / 2;
  } else {
    d8->expn = d4->expn / 2;
  }
  if (!(d4->expn & 1)) {
    d8->cells[0] = d4->cells[0];
    i4 = 1;
    i8 = 1;
  }
  for (; i4 < d4->ncells; i4 += 2, i8++) {
    d8->cells[i8] = (c8typ_t)d4->cells[i4] * DEC4_CELL_MASK;
    if (i4 + 1 < d4->ncells) {
      d8->cells[i8] += d4->cells[i4 + 1];
    }
  }
  d8->ncells = i8;
}
static inline void cm_dec_8_to_4(dec4_t *d4, const dec8_t *d8) {
  if (DECIMAL_IS_ZERO(d8)) {
    cm_zero_dec4(d4);
    return;
  }
  uint32 i8 = 0;
  uint32 i4 = 0;
  d4->sign = d8->sign;
  d4->expn = (int8)(d8->expn * 2 + 1);
  if (d8->cells[0] < DEC4_CELL_MASK) {
    d4->cells[0] = (c4typ_t)d8->cells[0];
    d4->expn--;
    i4++;
    i8++;
  }
  for (; i8 < d8->ncells; i8++, i4 += 2) {
    d4->cells[i4] = d8->cells[i8] / DEC4_CELL_MASK;
    d4->cells[i4 + 1] = d8->cells[i8] % DEC4_CELL_MASK;
  }

  if (i4 > DEC4_CELL_SIZE) {
    assert(0);
  }

  // remove tailing zero if exits
  if (d4->cells[i4 - 1] == 0) {
    i4--;
  }
  d4->ncells = (uint8)i4;
}