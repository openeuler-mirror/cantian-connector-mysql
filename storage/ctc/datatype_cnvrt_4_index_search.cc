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
#include "sql/field.h"
#include "typelib.h"
#include "ctc_log.h"
#include "ctc_error.h"
#include "datatype_cnvrt_4_index_search.h"

constexpr uint8 OFFSET_VARCHAR_TYPE = 2;

static void ctc_convert_mysql_key_to_cantian(KEY_PART_INFO &key_part, const uint8_t *key, uint16_t key_len, uint32_t *data_field_len,
                                             uint8_t **use_key, uint32_t *use_key_len, bool *is_key_null) {
  if (key_len == 0) {
    return;
  }

  Field *field = key_part.field;
  uint16_t offset = 0;
  if (field->is_nullable()) {
    offset = 1;
  }
  bool check_blob = false;
  // convert ctc index datatype
  uint32_t data_len = field->key_length();
  if (field->type() == MYSQL_TYPE_VARCHAR) {
    *data_field_len = offset + data_len + OFFSET_VARCHAR_TYPE;
  } else if (field->is_flag_set(BLOB_FLAG) &&
             field->part_of_prefixkey.to_ulonglong() &&
             !field->part_of_key_not_extended.to_ulonglong()) {
    *data_field_len = key_part.store_length;
    check_blob = true;
  } else {
    *data_field_len = offset + data_len;
  }

  if (field->is_nullable() && key[0] == 1) {
    *is_key_null = true;
    *use_key_len = 0;
    return;
  }

  if (field->type() == MYSQL_TYPE_VARCHAR || check_blob) {
    // 2:the first two digits of the key pointer record the length.
    *use_key = const_cast<uint8_t *>(key + offset + OFFSET_VARCHAR_TYPE);
    *use_key_len = *(uint16_t *)const_cast<uint8_t *>(key + offset);
  } else if (field->real_type() == MYSQL_TYPE_STRING) {
      // strip the pad_char
      *use_key = const_cast<uint8_t *>(key + offset);
      while (data_len > 0 && key[data_len + offset - 1] == field->charset()->pad_char) {
        data_len--;
      }
      *use_key_len = data_len;
  } else {
    *use_key = const_cast<uint8_t *>(key + offset);
    *use_key_len = data_len;
  }

  return;
}

static void ctc_index_make_up_key_length(int *key, uint8_t **origin_key, uint32_t *origin_key_len, uint32_t length) {
  int tmp_key = 0;
  uint8_t *tmp_key_ptr = (uint8_t *)&tmp_key;
  memcpy(tmp_key_ptr, *origin_key, *origin_key_len);

  // 通过符号位补齐高位字段
  if ((*origin_key_len == 1 && (tmp_key & 0x80)) || // 1表示MYSQL_TYPE_TINY类型，0x80用于获取origin_key的符号位
      (*origin_key_len == 2 && (tmp_key & 0x8000)) || // 2表示MYSQL_TYPE_SHORT类型，0x8000用于获取origin_key的符号位
      (*origin_key_len == 3 && (tmp_key & 0x800000))) { // 3表示MMYSQL_TYPE_INT24类型，0x800000用于获取origin_key的符号位
    *key = 0xFFFFFFFF; // 将key的bit位全赋值为1
  } else {
    *key = 0;
  }

  memcpy(key, *origin_key, *origin_key_len);
  *origin_key = reinterpret_cast<uint8_t *>(key);
  *origin_key_len = length;

  return;
}

int ctc_fill_index_key_info(TABLE *table, const uchar *key, uint key_len, const key_range *end_range, 
                            index_key_info_t *index_key_info, bool index_skip_scan) {
  const uchar *my_key = nullptr;
  const uchar *end_key = key + key_len;
  if (end_range != nullptr) {
    my_key = end_range->key;
  }

  index_key_info->key_num = 0;
  do {
    if (index_key_info->key_num >= table->key_info[index_key_info->active_index].actual_key_parts) {
      ctc_log_error("ctc_fill_index_key_info: colunm id(%d) is bigger than table key parts(%d).", 
                    index_key_info->key_num, table->key_info[index_key_info->active_index].actual_key_parts);
      return ERR_GENERIC_INTERNAL_ERROR;
    }

    KEY_PART_INFO key_part = table->key_info[index_key_info->active_index].key_part[index_key_info->key_num];
    uint32_t data_field_len = 0;
    ctc_convert_mysql_key_to_cantian(key_part, key, key_len, &data_field_len,
                                     &index_key_info->key_info[index_key_info->key_num].left_key,
                                     &index_key_info->key_info[index_key_info->key_num].left_key_len,
                                     &index_key_info->key_info[index_key_info->key_num].is_key_null);

    if (!index_skip_scan && end_range != nullptr && my_key < end_range->key + end_range->length) {
      ctc_convert_mysql_key_to_cantian(key_part, my_key, end_range->length, &data_field_len,
                                       &index_key_info->key_info[index_key_info->key_num].right_key,
                                       &index_key_info->key_info[index_key_info->key_num].right_key_len,
                                       &index_key_info->key_info[index_key_info->key_num].is_key_null);
    }

    key += data_field_len;
    if (end_range != nullptr && my_key < end_range->key + end_range->length) {
      my_key += data_field_len;
    }

    ++index_key_info->key_num;
  } while ((key < end_key) && (key_len != 0));

  for (uint i = index_key_info->key_num; i < table->key_info[index_key_info->active_index].actual_key_parts; ++i) {
    index_key_info->key_info[i].is_key_null = true;
  }

  return CT_SUCCESS;
}

int ctc_convert_key_from_mysql_to_cantian(Field *field, uint8_t **mysql_ptr, dec4_t *cantian_ptr, uint32_t *len)
{
  int ret = CT_SUCCESS;
  const field_cnvrt_aux_t *mysql_info = get_auxiliary_for_field_convert(field, field->type());
  // 针对tiny和short类型，对应到cantian是int类型，所以key length需要按照cantian大小的存储
  if (mysql_info->mysql_field_type == MYSQL_TYPE_TINY || mysql_info->mysql_field_type == MYSQL_TYPE_SHORT ||
      mysql_info->mysql_field_type == MYSQL_TYPE_INT24) {
    ctc_index_make_up_key_length(reinterpret_cast<int *>(cantian_ptr), mysql_ptr, len, sizeof(int));

    return ret;
  }

  if (field->type() == MYSQL_TYPE_BIT) {
    *(int64_t *)cantian_ptr = bit_cnvt_mysql_cantian(*mysql_ptr, field);
    *len = CTC_BYTE_8;
    *mysql_ptr = reinterpret_cast<uint8_t *>(cantian_ptr);
    return ret;
  }

  if (field->type() == MYSQL_TYPE_FLOAT) {
    double temp;
    temp = **(float **)mysql_ptr;
    memcpy((void *)cantian_ptr, &temp, sizeof(double));
    *mysql_ptr = reinterpret_cast<uint8_t *>(cantian_ptr);
    *len = sizeof(double);

    return ret;
  }

  if (mysql_info->sql_data_type == DATETIME_DATA) {
    ret = convert_datetime_to_cantian(mysql_info, reinterpret_cast<uchar *>(cantian_ptr), *mysql_ptr, field);
    if (ret != CT_SUCCESS) {
      ctc_log_error("ctc convert index datatime to cantian failed.");
      return ret;
    }

    *mysql_ptr = reinterpret_cast<uint8_t *>(cantian_ptr);
    *len = sizeof(date_t);
    return ret;
  }

  if (field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL) {
    ret = decimal_mysql_to_cantian(*mysql_ptr, reinterpret_cast<uchar *>(cantian_ptr), field, len);
    if (ret != CT_SUCCESS) {
      ctc_log_error("ctc convert index decimal to cantian failed.");
      return ret;
    }

    *mysql_ptr = reinterpret_cast<uint8_t *>(cantian_ptr);
  }

  return ret;
}

int ctc_convert_index_datatype(TABLE *table, index_key_info_t *index_key_info, bool has_right_key, dec4_t *data) {
  int ret;

  for (int i = 0; i < index_key_info->key_num; ++i) {
    Field *field = table->key_info[index_key_info->active_index].key_part[i].field;
    if (index_key_info->key_info[i].left_key_len != 0) {
      ret = ctc_convert_key_from_mysql_to_cantian(field, &index_key_info->key_info[i].left_key, data++, &index_key_info->key_info[i].left_key_len);
      if (ret != CT_SUCCESS) {
        ctc_log_error("ctc_convert_key_from_mysql_to_cantian: convert mysql index search left key failed, ret(%d).", ret);
        return ret;
      }
    }

    if (has_right_key && index_key_info->key_info[i].right_key_len != 0) {
      ret = ctc_convert_key_from_mysql_to_cantian(field, &index_key_info->key_info[i].right_key, data++, &index_key_info->key_info[i].right_key_len);
      if (ret != CT_SUCCESS) {
        ctc_log_error("ctc_convert_key_from_mysql_to_cantian: convert mysql index search right key failed, ret(%d).", ret);
        return ret;
      }
    }
  }

  return CT_SUCCESS;
}