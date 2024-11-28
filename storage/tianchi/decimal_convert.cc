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
#include "my_global.h"
#include "decimal_convert.h"
#include <stdio.h>
#include "stdlib.h"
#include "tse_log.h"
// half of g_1ten_powers, used for rounding a decimal
const uint32 ct_5ten_powers[10] = {
    0u,          // 0
    5u,          // 5 x 10^0
    50u,         // 5 x 10^1
    500u,        // 5 x 10^2
    5000u,       // 5 x 10^3
    50000u,      // 5 x 10^4
    500000u,     // 5 x 10^5
    5000000u,    // 5 x 10^6
    50000000u,   // 5 x 10^7
    500000000u,  // 5 x 10^8
};
const uint32 ct_1ten_powers[10] = {
    1u,          // 10^0
    10u,         // 10^1
    100u,        // 10^2
    1000u,       // 10^3
    10000u,      // 10^4
    100000u,     // 10^5
    1000000u,    // 10^6
    10000000u,   // 10^7
    100000000u,  // 10^8
    1000000000u  // 10^9
};
static int32 cm_dec8_calc_prec(const dec8_t *dec);
static inline void ct_cm_str2text(char *str, text_t *text) {
  text->str = str;
  text->len = (str == NULL) ? 0 : (uint32)strlen(str);
}
status_t ct_cm_str_to_dec8(const char *str, dec8_t *dec) {
  text_t text;
  ct_cm_str2text(const_cast<char *>(str), &text);
  return ct_cm_text_to_dec8(&text, dec);
}
/**
 * Translates a text_t representation of a decimal into a decimal
 */
status_t ct_cm_text_to_dec8(const text_t *dec_text, dec8_t *dec) {
  num_errno_t err_no;
  num_part_t np;
  np.excl_flag = NF_NONE;
  err_no = ct_cm_split_num_text(dec_text, &np);
  if (err_no != NERR_SUCCESS) {
    tse_log_error("Decimal data type convert text to dec8 failed!");
    return CT_ERROR;
  }
  err_no = ct_cm_numpart_to_dec8(&np, dec);
  if (err_no != NERR_SUCCESS) {
    tse_log_error("Decimal data type convert numpart to dec8 failed!");
    return CT_ERROR;
  }
  return CT_SUCCESS_STATUS;
}
/**
 * Quickly find the precision of a cells
 * @note  (1) The cell u0 should be specially treated;
 *        (2) The tailing zeros will not be counted. If all cell except u0 are
 *        zeros, then the precision of u0 is re-counted by ignoring tailing
 * zeros e.g. | u0 = 1000 | u1 = 0 | u2 = 0 |..., the precision 1 will be
 *        returned.
 */
static int32 cm_dec8_calc_prec(const dec8_t *dec) {
  int32 i, j;
  uint32 u;
  int32 prec = 0;
  if (dec->ncells == 0) {
    return 0;
  }
  /* Step 1: Find the precision of remaining cells starting from backend */
  for (i = dec->ncells - 1; i > 0; --i) {
    if (dec->cells[i] > 0) {  // found the last non-zero cell (dec->cells[i]>0)
      // count digits in this cell by ignoring tailing zeros
      j = 0;
      u = dec->cells[i];
      while (u != 0 && u % 10 == 0) {
        ++j;
        u /= 10;
      }
      prec += (i * DEC8_CELL_DIGIT - j);
      break;
    }
  }
  /* Step 2: Count the precision of u0 */
  if (i == 0) {  // if u1, u2, ... are zeros, then the precision of u0 should
                 // remove tailing zeros
    u = dec->cells[0];
    while (u != 0 && u % 10 == 0) {  // remove tailing zeros
      u /= 10;
    }
    prec = (int32)cm_count_u32digits((c8typ_t)u);
  } else {
    prec += (int32)cm_count_u32digits(dec->cells[0]);
  }
  return prec;
}
/** recording the significand digits into num_part */
static inline void ct_cm_record_digit(num_part_t *np, int32 *precision,
                                      int32 *prec_offset, int32 pos, char c) {
  if (*precision >= 0) {
    ++(*precision);
    if (*precision > (DEC_MAX_NUM_SAVING_PREC + 1)) {
      // if the buff is full, ignoring the later digits
      return;
    } else if (*precision == (DEC_MAX_NUM_SAVING_PREC + 1)) {
      // mark the rounding mode is needed
      np->do_round = (c >= '5');
      return;
    }
  } else {
    *precision = 1;
  }
  if (*precision == 1) {
    // if found the first significant digit, records its position
    *prec_offset = pos;
  }
  CM_TEXT_APPEND(&np->digit_text, c);
}
static inline void cm_dec8_rebuild(dec8_t *rs, uint32 cell0) {
  /* decide the number of cells */
  if (rs->ncells < DEC8_CELL_SIZE) {
    rs->ncells++;
  }
  /* right shift cell data by 1 */
  uint32 i = rs->ncells;
  while (i-- > 1) {
    rs->cells[i] = rs->cells[i - 1];
  }
  /* put the carry into cells[0] */
  rs->cells[0] = (c8typ_t)cell0;
  rs->expn++;
}
/** CM_MAX_EXPN must greater than the maximal exponent that DB can capacity.
 * In current system, the maximal exponent is 308 for double. Therefore, the
 * value is set to 99999999 is reasonable. */
#define CM_MAX_EXPN 99999999
/**
 * Parse an exponent from the numeric text *dec_text*, i is the offset
 * of exponent. When unexpected character occur or the exponent overflow,
 * an error will be returned.
 */
static inline num_errno_t ct_cm_parse_num_expn(text_t *expn_text, int32 *expn) {
  char c;
  int32 tmp_exp;
  bool32 is_negexp = CT_FALSE;
  uint32 i = 0;
  // handle the sign of exponent
  c = expn_text->str[i];
  if (CM_IS_SIGN_CHAR(c)) {
    is_negexp = (c == '-');
    c = expn_text->str[++i];  // move to next character
  }
  if (i >= expn_text->len) { /* if no exponent digits, return error  */
    CT_THROW((i >= expn_text->len), NERR_NO_EXPN_DIGIT);
  }
  // skip leading zeros in the exponent
  while (CM_IS_ZERO(c)) {
    ++i;
    if (i >= expn_text->len) {
      *expn = 0;
      return NERR_SUCCESS;
    }
    c = expn_text->str[i];
  }
  // too many nonzero exponent digits
  tmp_exp = 0;
  for (;;) {
    CT_THROW((!CM_IS_DIGIT(c)), NERR_EXPN_WITH_NCHAR);
    if (tmp_exp < CM_MAX_EXPN) {  // to avoid int32 overflow
      tmp_exp = tmp_exp * CM_DEFAULT_DIGIT_RADIX + CM_C2D(c);
    }
    ++i;
    if (i >= expn_text->len) {
      break;
    }
    c = expn_text->str[i];
  }
  // check exponent overflow on positive integer
  CT_THROW((!is_negexp && tmp_exp > CM_MAX_EXPN), NERR_OVERFLOW);
  *expn = is_negexp ? -tmp_exp : tmp_exp;
  return NERR_SUCCESS;
}
/** calculate expn of the significand digits */
static inline int32 ct_cm_calc_significand_expn(int32 dot_offset,
                                                int32 prec_offset,
                                                int32 precision) {
  // Step 3.1. compute the sci_exp
  if (dot_offset >= 0) { /* if a dot exists */
    /* Now, prec_offset records the distance from the first significant digit to
     * the dot.
     * dot_offset > 0 means dot is counted, thus this means the sci_exp should
     * subtract one.  */
    dot_offset -= prec_offset;
    return ((dot_offset > 0) ? dot_offset - 1 : dot_offset);
  } else {
    return precision - 1;
  }
}
num_errno_t ct_cm_split_num_text(const text_t *num_text, num_part_t *np) {
  int32 i;
  char c;
  text_t text;           /** the temporary text */
  int32 dot_offset = -1; /** '.' offset, -1 if none */
  int32 prec_offset =
      -1; /** the offset of the first significant digit, -1 if none */
  int32 precision = -1;          /* see comments of the function */
  bool32 leading_flag = CT_TRUE; /** used to ignore leading zeros */
  /* When the number of significant digits exceeds the dight_buf
   * Then, a round happens when the MAX_NUMERIC_BUFF+1 significant
   * digit is equal and greater than '5' */
  // CM_POINTER3(text, np, buf);
  if (np == NULL) {
    assert(0);
  }
  np->digit_text.len = 0;
  np->has_dot = CT_FALSE;
  np->has_expn = CT_FALSE;
  np->do_round = CT_FALSE;
  np->is_neg = CT_FALSE;
  np->sci_expn = 0;
  text = *num_text;
  ct_cm_trim_text(&text);
  CT_THROW((text.len == 0 || text.len >= SIZE_M(1)),
           NERR_INVALID_LEN);  // text.len > 2^15
  i = 0;
  /* Step 1. fetch the sign of the decimal */
  if (text.str[i] == '-') {  // leading minus means negative
    // if negative sign is not allowed
    CT_THROW((np->excl_flag & NF_NEGATIVE_SIGN), NERR_UNALLOWED_NEG);
    np->is_neg = CT_TRUE;
    i++;
  } else if (text.str[i] == '+') {  // leading + allowed
    i++;
  }
  /* check again */
  CT_THROW((i >= (int32)text.len), NERR_NO_DIGIT);
  /* Step 2. parse the scale, exponent, precision, Significant value of the
   * decimal */
  for (; i < (int32)text.len; ++i) {
    c = text.str[i];
    if (leading_flag) {  // ignoring leading zeros
      if (CM_IS_ZERO(c)) {
        precision = 0;
        continue;
      } else if (c != '.') {
        leading_flag = CT_FALSE;
      }
    }
    if (CM_IS_DIGIT(c)) {  // recording the significand
      ct_cm_record_digit(np, &precision, &prec_offset, i, c);
      continue;
    }
    if (CM_IS_DOT(c)) {
      CT_THROW((np->excl_flag & NF_DOT), NERR_UNALLOWED_DOT);
      CT_THROW((dot_offset >= 0), NERR_MULTIPLE_DOTS);
      dot_offset = i;  //
      np->has_dot = CT_TRUE;
      continue;
    }
    // begin to handle and fetch exponent
    CT_THROW((!CM_IS_EXPN_CHAR(c)), NERR_UNEXPECTED_CHAR);
    // Exclude: 'E0012', '.E0012', '-E0012', '+.E0012', .etc
    CT_THROW((precision < 0), NERR_UNEXPECTED_CHAR);
    CT_THROW((np->excl_flag & NF_EXPN), NERR_UNALLOWED_EXPN);
    // redirect text pointing to expn part
    text.str += (i + 1);
    text.len -= (i + 1);
    num_errno_t nerr = ct_cm_parse_num_expn(&text, &np->sci_expn);
    CT_THROW((nerr != NERR_SUCCESS), nerr);
    np->has_expn = CT_TRUE;
    break;
  }  // end for
  CT_THROW((precision < 0), NERR_NO_DIGIT);
  if (precision == 0) {
    CM_ZERO_NUMPART(np);
    return NERR_SUCCESS;
  }
  // Step 3: Calculate the scale of the total number text
  np->sci_expn +=
      ct_cm_calc_significand_expn(dot_offset, prec_offset, precision);
  if (np->digit_text.len > num_text->len ||
      np->digit_text.len >= CT_MAX_NUM_PART_BUFF) {
    return NERR_ERROR;
  }
  return NERR_SUCCESS;
}
/*
 * Convert a cell text into a cell of big integer by specifying the
 * length digits in u0 (i.e., len_u0), and return the number of non-zero cells
 * Performance sensitivity.CM_ASSERT should be guaranteed by caller, i.g.
 * cells[0] > 0
 */
static inline int32 cm_digitext_to_cell8s(digitext_t *dtext, cell8_t cells,
                                          int32 len_u0) {
  uint32 i, k;
  text_t cell_text;
  // make u0
  cell_text.str = dtext->str;
  cell_text.len = (uint32)len_u0;
  cells[0] = (c8typ_t)cm_celltext2uint32(&cell_text);
  // make u1, u2, ..., uk
  k = 1;
  for (i = (uint32)len_u0; k < DEC8_CELL_SIZE && i < dtext->len; k++) {
    cell_text.str = dtext->str + i;
    cell_text.len = (uint32)DEC8_CELL_DIGIT;
    cells[k] = (c8typ_t)cm_celltext2uint32(&cell_text);
    i += DEC8_CELL_DIGIT;
  }
  // the tailing cells of significant cells may be zeros, for returning
  // accurate ncells, they should be ignored.
  while (cells[k - 1] == 0) {
    --k;
  }
  return (int32)k;
}
/**
 * Convert a digit text with a scientific exponent into a decimal
 * The digit text may be changed when adjust the scale of decimal to be
 * an integral multiple of DEC_CELL_DIGIT, by appending zeros.
 * @return the precision of u0
 * @note
 * Performance sensitivity.CM_ASSERT should be guaranteed by caller,
 * i.g. dtext->len > 0 && dtext->len <= (uint32)DEC_MAX_ALLOWED_PREC
 */
static inline int32 cm_digitext_to_dec8(dec8_t *dec, digitext_t *dtext,
                                        int32 sci_exp) {
  int32 delta;
  int32 len_u0;  // the length of u0
  len_u0 = (int32)dtext->len % DEC8_CELL_DIGIT;
  ++sci_exp;  // increase the sci_exp to obtain the position of dot
  delta = sci_exp - len_u0;
  delta += (int32)DEC8_CELL_DIGIT << 16;  // make delta to be positive
  delta %= DEC8_CELL_DIGIT;               // get the number of appending zeros
  len_u0 = (len_u0 + delta) % DEC8_CELL_DIGIT;
  if (len_u0 == 0) {
    len_u0 = DEC8_CELL_DIGIT;
  }
  while (delta-- > 0) {
    CM_TEXT_APPEND(dtext, '0');
  }
  if (dtext->len < CT_MAX_DEC_OUTPUT_ALL_PREC) {
    CM_NULL_TERM(dtext);
  }
  dec->ncells = (uint8)cm_digitext_to_cell8s(dtext, dec->cells, len_u0);
  dec->expn = SEXP_2_D8EXP(sci_exp - len_u0);
  return len_u0;
}
static inline void cm_do_numpart_round8(
    const num_part_t *np MY_ATTRIBUTE((unused)), dec8_t *dec, uint32 prec0) {
  c8typ_t carry = ct_1ten_powers[prec0 % DEC8_CELL_DIGIT];
  uint32 i = dec->ncells;

  while (i-- > 0) {
    dec->cells[i] += carry;
    carry = (dec->cells[i] >= DEC8_CELL_MASK);
    if (carry == 0) {
      return;
    }
    dec->cells[i] -= DEC8_CELL_MASK;
  }
  if (carry > 0) {
    cm_dec8_rebuild(dec, 1);
  }
}
num_errno_t ct_cm_numpart_to_dec8(num_part_t *np, dec8_t *dec) {
  if (NUMPART_IS_ZERO(np)) {
    cm_zero_dec8(dec);
    return NERR_SUCCESS;
  }
  // Step 3.2. check overflow by comparing scientific scale and MAX_NUMERIC_EXPN
  if (np->sci_expn > MAX_NUMERIC_EXPN) {  // overflow return Error
    return NERR_OVERFLOW;
  } else if (np->sci_expn < MIN_NUMERIC_EXPN) {  // underflow return 0
    cm_zero_dec8(dec);
    return NERR_SUCCESS;
  }
  // Step 4: make the final decimal value
  dec->sign = (uint8)np->is_neg;
  int32 prec0 = cm_digitext_to_dec8(dec, &np->digit_text, np->sci_expn);
  if (np->do_round) {  // when round happens, the dec->cells should increase 1
    cm_do_numpart_round8(np, dec, (uint32)prec0);
    cm_dec8_trim_zeros(dec);  // rounding may change the precision
  }
  return NERR_SUCCESS;
}
/**
 * Convert a decimal into C-string, and return the ac
 */
status_t ct_cm_dec8_to_str(const dec8_t *dec, int max_len, char *str) {
  text_t text;
  text.str = str;
  text.len = 0;
  CT_RETURN_IF_ERROR(ct_cm_dec8_to_text(dec, max_len, &text));
  str[text.len] = '\0';
  return CT_SUCCESS_STATUS;
}
/**
 * Convert the significant digits of cells into text with a maximal len
 * @note  The tailing zeros are removed when outputting
 */
static void cm_cell8s_to_text(const cell8_t cells, uint32 ncell, text_t *text,
                              int32 max_len) {
  uint32 i;
  int iret_snprintf;
  iret_snprintf = snprintf(text->str, DEC8_CELL_DIGIT + 1, "%u", cells[0]);
  if (iret_snprintf == -1) {
    assert(0);
  }
  text->len = (uint32)iret_snprintf;
  for (i = 1; (text->len < (uint32)max_len) && (i < ncell); ++i) {
    iret_snprintf = snprintf(CM_GET_TAIL(text), DEC8_CELL_DIGIT + 1,
                             DEC8_CELL_FMT, (uint32)cells[i]);
    if (iret_snprintf == -1) {
      assert(0);
    }
    text->len += (uint32)iret_snprintf;
  }
  // truncate redundant digits
  if (text->len > (uint32)max_len) {
    text->len = (uint32)max_len;
  }
  // truncate tailing zeros
  for (i = (uint32)text->len - 1; i > 0; --i) {
    if (!CM_IS_ZERO(text->str[i])) {
      break;
    }
    --text->len;
  }
  CM_NULL_TERM(text);
}
/* Copy the data a decimal */
static inline void cm_dec8_copy(dec8_t *dst, const dec8_t *src) {
  if (SECUREC_UNLIKELY(dst == src)) {
    return;
  }
  dst->head = src->head;
  /* Another way to Copy the data of decimals is to use loops, for example:
   *    uint32 i = src->ncells;
   *    while (i-- > 0)
   *        dst->cells[i] = src->cells[i];
   * However, this function is performance sensitive, and not too safe when
   * src->ncells is abnormal. By actural testing, using switch..case here
   * the performance can improve at least 1.5%. The testing results are
   *    WHILE LOOP  : 5.64% cm_dec8_copy
   *    SWITCH CASE : 4.14% cm_dec8_copy
   * Another advantage is that the default branch of SWITCH CASE can be used
   * to handle abnormal case, which reduces an IF statement.
   */
  switch (src->ncells) {
    case 7:
      dst->cells[6] = src->cells[6];
      /* fall-through */
    case 6:
      dst->cells[5] = src->cells[5];
      /* fall-through */
    case 5:
      dst->cells[4] = src->cells[4];
      /* fall-through */
    case 4:
      dst->cells[3] = src->cells[3];
      /* fall-through */
    case 3:
      dst->cells[2] = src->cells[2];
      /* fall-through */
    case 2:
      dst->cells[1] = src->cells[1];
      /* fall-through */
    case 1:
      dst->cells[0] = src->cells[0];
      /* fall-through */
    case 0:
      break;
    default:
      cm_zero_dec8(dst);
      break;
  }
}
/**
 * Product a cell array with the digit at pos (starting from left) is k
 */
static inline bool32 cm_dec8_make_round(const dec8_t *dec, uint32 pos,
                                        dec8_t *dx) {
  int32 i;
  uint32 carry, j;
  cm_dec8_copy(dx, dec);
  if (pos >= DEC8_MAX_ALLOWED_PREC) {
    return CT_FALSE;
  }
  i = (int32)(pos / DEC8_CELL_DIGIT);
  j = pos % DEC8_CELL_DIGIT;

  carry = (uint32)ct_5ten_powers[DEC8_CELL_DIGIT - j];
  for (; i >= 0; i--) {
    dx->cells[i] += carry;
    carry = (dx->cells[i] >= DEC8_CELL_MASK);
    if (!carry) {
      return CT_FALSE;
    }
    dx->cells[i] -= DEC8_CELL_MASK;
  }
  if (carry > 0) {
    cm_dec8_rebuild(dx, 1);
  }
  return carry;
}
/**
 * Round a decimal to a text with the maximal length max_len
 * If the precision is greater than max_len, a rounding mode is used.
 * The rounding mode may cause a change on precision, e.g., the 8-precision
 * decimal 99999.999 rounds to 7-precision decimal is 100000.00, and then
 * its actual precision is 8. The function will return the change. If
 * no change occurs, zero is returned.
 * @note
 * Performance sensitivity.CM_ASSERT should be guaranteed by caller,
 * i.g. 1.max_len > 0    2.dec->cells[0] > 0
 */
static int32 cm_dec8_round_to_text(const dec8_t *dec, int32 max_len,
                                   text_t *text_out) {
  dec8_t txtdec;
  uint32 prec_u0;
  int32 prec;
  prec = cm_dec8_calc_prec(dec);
  if (prec <= max_len) {  // total prec under the max_len
    cm_cell8s_to_text(dec->cells, dec->ncells, text_out, prec);
    return 0;
  }
  /** if prec > max_len, the rounding mode is applied */
  prec_u0 = cm_count_u32digits(dec->cells[0]);
  // Rounding model begins by adding with {5[(prec - max_len) zeros]}
  // Obtain the pos of 5 for rounding, then prec is used to represent position
  prec = DEC8_POS_N_BY_PREC0(max_len, prec_u0);
  // add for rounding and check whether the carry happens, and capture the
  // changes of the precision
  if (cm_dec8_make_round(dec, (uint32)prec, &txtdec)) {
    // if carry happens, the change must exist
    cm_cell8s_to_text(txtdec.cells, dec->ncells + 1, text_out, max_len);
    return 1;
  } else {
    cm_cell8s_to_text(txtdec.cells, dec->ncells, text_out, max_len);
    return (cm_count_u32digits(txtdec.cells[0]) > prec_u0) ? 1 : 0;
  }
}
#define DEC_EXPN_BUFF_SZ 16
/**
 * Output a decimal type in scientific format, e.g., 2.34566E-20
 */
static inline status_t cm_dec8_to_sci_text(text_t *text, const dec8_t *dec,
                                           int32 max_len) {
  int32 i;
  char obuff[CT_NUMBER_BUFFER_SIZE]; /** output buff */
  text_t cell_text = {.str = obuff, .len = 0};
  char sci_buff[DEC_EXPN_BUFF_SZ];
  int32 sci_exp; /** The scientific scale of the dec */
  int32 placer;
  int iret_snprintf;
  sci_exp = DEC8_GET_SEXP(dec);
  // digits of sci_exp + sign(dec) + dot + E + sign(expn)
  placer = (int32)dec->sign + 3;
  placer += (int32)cm_count_u32digits((c8typ_t)abs(sci_exp));
  if (max_len <= placer) {
    return CT_ERROR;
  }
  /* The round of a decimal may increase the precision by 1 */
  if (cm_dec8_round_to_text(dec, max_len - placer, &cell_text) > 0) {
    ++sci_exp;
  }
  // compute the exponent placer
  iret_snprintf = snprintf(sci_buff, DEC_EXPN_BUFF_SZ - 1, "E%+d", sci_exp);
  if (iret_snprintf != EOK) {
    return CT_ERROR;
  }
  placer = iret_snprintf;
  // Step 1. output sign
  text->len = 0;
  if (dec->sign == DEC_SIGN_MINUS) {
    CM_TEXT_APPEND(text, '-');
  }
  CM_TEXT_APPEND(text, cell_text.str[0]);
  CM_TEXT_APPEND(text, '.');
  for (i = 1; (int32)text->len < max_len - placer; ++i) {
    if (i < (int32)cell_text.len) {
      CM_TEXT_APPEND(text, cell_text.str[i]);
    } else {
      CM_TEXT_APPEND(text, '0');
    }
  }
  iret_snprintf =
      snprintf(&text->str[text->len], DEC_EXPN_BUFF_SZ - 1, "%s", sci_buff);
  if (iret_snprintf != EOK) {
    return CT_ERROR;
  }
  text->len += (uint32)iret_snprintf;
  return CT_SUCCESS_STATUS;
}
static inline void cm_concat_text(text_t *text, const text_t *part) {
  for (uint32 i = 0; i < part->len; ++i) {
    CM_TEXT_APPEND(text, part->str[i]);
  }
}
/**
 * Append num characters c to the text; if num<=0, do nothing;
 * @note the user must ensure sufficient space to store them
 */
static inline void cm_text_appendc(text_t *text, int32 num, char c) {
  while (num-- > 0) {
    CM_TEXT_APPEND(text, c);
  }
}
static inline status_t cm_concat_ntext(text_t *dst, const text_t *src,
                                       int32 num) {
  if (num <= 0) {
    return CT_SUCCESS_STATUS;
  }
  if ((uint32)num > src->len) {
    num = (int32)src->len;
  }
  if (num != 0) {
    memcpy(CM_GET_TAIL(dst), src->str, num);
  }
  dst->len += (uint32)num;
  return CT_SUCCESS_STATUS;
}
/**
 * @note
 * Performance sensitivity.CM_ASSERT should be guaranteed by caller, i.g.
 * dot_pos <= max_len - dec->sign
 */
static inline status_t cm_dec8_to_plain_text(text_t *text, const dec8_t *dec,
                                             int32 max_len, int32 sci_exp,
                                             int32 prec) {
  int32 dot_pos;
  char obuff[CT_NUMBER_BUFFER_SIZE]; /** output buff */
  text_t cell_text;
  cell_text.str = obuff;
  cell_text.len = 0;
  // clear text & output sign
  text->len = 0;
  if (dec->sign == DEC_SIGN_MINUS) {
    CM_TEXT_APPEND(text, '-');
  }
  dot_pos = sci_exp + 1;
  if (prec <= dot_pos) {
    (void)cm_dec8_round_to_text(dec, max_len - dec->sign,
                                &cell_text);  // subtract sign
    cm_concat_text(text, &cell_text);
    cm_text_appendc(text, dot_pos - prec, '0');
    CM_NULL_TERM(text);
    return CT_SUCCESS_STATUS;
  }
  /* get the position of dot w.r.t. the first significant digit */
  if (dot_pos == max_len - dec->sign) {
    /* handle the border case with dot at the max_len position,
     * then the dot is not outputted. Suppose max_len = 10,
     *  (1). 1234567890.222 --> 1234567890 is outputted
     * If round mode products carry, e.g. the rounded value of
     * 9999999999.9 is 10000000000, whose length is 11 and greater than
     * max_len, then the scientific format is used to print the decimal
     */
    if (cm_dec8_round_to_text(dec, dot_pos, &cell_text) > 0) {
      CM_TEXT_CLEAR(text);
      return cm_dec8_to_sci_text(text, dec, max_len);
    }
    cm_concat_text(text, &cell_text);
    cm_text_appendc(text, max_len - (int32)text->len, '0');
  } else if (dot_pos == max_len - dec->sign - 1) {
    /* handle the border case with dot at the max_len - 1 position,
     * then only max_len-1 is print but the dot is emitted. Assume
     * max_len = 10, the following cases output:
     *  (1). 123456789.2345 ==> 123456789  (.2345 is abandoned)
     *  (2). 987654321.56   ==> 987654322  (.56 is rounded to 1)
     * If a carry happens, e.g., 999999999.6 ==> 1000000000, max_len
     * number of digits will be printed.
     * */
    int32 change = cm_dec8_round_to_text(dec, dot_pos, &cell_text);
    cm_concat_text(text, &cell_text);
    cm_text_appendc(text, max_len + change - ((int32)text->len + 1), '0');
  } else if (dot_pos >= 0) { /* dot is inside of cell_text and may be output */
    // round mode may product carry, and thus may affect the dot_pos
    dot_pos += cm_dec8_round_to_text(dec, max_len - dec->sign - 1,
                                     &cell_text);  // subtract sign & dot
    if ((int32)cell_text.len <= dot_pos) {
      cm_concat_text(text, &cell_text);
      cm_text_appendc(text, dot_pos - (int32)cell_text.len, '0');
    } else {
      cm_concat_ntext(text, &cell_text, dot_pos);
      CM_TEXT_APPEND(text, '.');
      // copy remaining digits
      cell_text.str += (uint32)dot_pos;
      cell_text.len -= (uint32)dot_pos;
      cm_concat_text(text, &cell_text);
    }
  } else {  // dot_pos < 0
    /* dot is in the most left & add |dot_pos| zeros between dot and cell_text
     * Thus, the maxi_len should consider sign, dot, and the adding zeros */
    dot_pos += cm_dec8_round_to_text(dec, max_len - dec->sign - 1 + dot_pos,
                                     &cell_text);
    CM_TEXT_APPEND(text, '.');
    cm_text_appendc(text, -dot_pos, '0');
    cm_concat_ntext(text, &cell_text, max_len - (int32)text->len);
  }
  CM_NULL_TERM(text);
  return CT_SUCCESS_STATUS;
}
/**
 * Convert a decimal into a text with a given maximal precision
 * @note
 * Performance sensitivity.CM_ASSERT should be guaranteed by caller,
 * i.g. 1.dec->sign == DEC_SIGN_PLUS    2.dec->expn == 0    3.dec->cells[0] > 0
 */
status_t ct_cm_dec8_to_text(const dec8_t *dec, int32 max_len, text_t *text) {
  int32 sci_exp; /** The scientific scale of the dec */
  int32 prec;
  if (dec == NULL || text == NULL) {
    assert(0);
  }
  max_len = MIN(max_len, (int32)(CT_NUMBER_BUFFER_SIZE - 1));
  if (dec->ncells == 0) {
    text->str[0] = '0';
    text->len = 1;
    return CT_SUCCESS_STATUS;
  }
  // Compute the final scientific scale of the dec, i.e., format of d.xxxx , d >
  // 0. Each decimal has an unique scientific representation.
  sci_exp = DEC8_GET_SEXP(dec);
  // get the total precision of the decimal
  prec = cm_dec8_calc_prec(dec);
  // Scientific representation when the scale exceeds the maximal precision
  // or have many leading zeros and have many significant digits
  // When sci_exp < 0, the length for '.' should be considered
  if ((sci_exp < -6 && -sci_exp + prec + (int32)dec->sign > max_len) ||
      (sci_exp > 0 && sci_exp + 1 + (int32)dec->sign > max_len)) {
    return cm_dec8_to_sci_text(text, dec, max_len);
  }
  // output plain text
  return cm_dec8_to_plain_text(text, dec, max_len, sci_exp, prec);
}
