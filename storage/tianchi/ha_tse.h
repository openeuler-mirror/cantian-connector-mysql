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

/* class for the the tse handler */

#ifndef __HA_TSE_H__
#define __HA_TSE_H__

#include <sys/types.h>
#include <vector>

#include "my_inttypes.h"
#include "sql/handler.h"
#include "sql/table.h"
#include "sql/item.h"
#include "sql/record_buffer.h"
#include "my_sqlcommand.h"
#include "tse_srv.h"
#include "datatype_cnvrtr.h"
#include "tse_error.h"
#include "tse_cbo.h"
#include "sql/abstract_query_plan.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/types/object_table_definition.h"
#include "sql/dd/string_type.h"

#pragma GCC visibility push(default)

using namespace std;

#define _INT_TO_STR(s) #s
#define INT_TO_STR(s) _INT_TO_STR(s)

#define CTC_CLIENT_VERSION_MAIN 3
#define CTC_CLIENT_VERSION_MINOR 0
#define CTC_CLIENT_VERSION_PATCH 0
#define CTC_CLIENT_VERSION_BUGFIX 101
#define CTC_VERSION_STR               \
  INT_TO_STR(CTC_CLIENT_VERSION_MAIN) \
  "." INT_TO_STR(CTC_CLIENT_VERSION_MINOR) "." INT_TO_STR(CTC_CLIENT_VERSION_PATCH) \
  "." INT_TO_STR(CTC_CLIENT_VERSION_BUGFIX)

// 版本号规则：十六进制后两位为小数点后数字，前面为小数点前数字，0x0310 --> 显示为3.16
#define CTC_CLIENT_VERSION_NUMBER 0x0300u


#define CT_NULL_VALUE_LEN   (uint16)0xFFFF
#define INVALID_MAX_UINT32 (uint32_t)0xFFFFFFFF
#define TSE_BUF_LEN (70 * 1024)
#define MAX_BATCH_FETCH_NUM 100
#define DEFAULT_RANGE_DENSITY 0.5
#define PREFER_RANGE_DENSITY 0.8
#define CT_MAX_RECORD_LENGTH 64000
#define BIG_RECORD_SIZE_DYNAMIC (BIG_RECORD_SIZE + 8)  // ha_tse构造函数中若没有申请到读写buf共享内存，就在使用时从40968内存池子中动态申请和释放

/* update if restraints changed in Cantian */
#define TSE_MAX_KEY_PART_LENGTH 4095  // CT_MAX_KEY_SIZE
#define TSE_MAX_KEY_PARTS 16         // CT_MAX_INDEX_COLUMNS
#define TSE_MAX_KEY_LENGTH 4095      // CT_MAX_KEY_SIZE
#define TSE_MAX_KEY_NUM 32           // CT_MAX_TABLE_INDEXES

/* The 'MAIN TYPE' of a column */
#define DATA_MISSING 0 /* missing column */
#define DATA_VARCHAR 1 
#define DATA_CHAR 2 /* fixed length character of the latin1_swedish_ci charset-collation */
#define DATA_FIXBINARY 3 /* binary string of fixed length */
#define DATA_BINARY 4 /* binary string */
#define DATA_BLOB 5 /* binary large object, or a TEXT type;         \
                      if prtype & DATA_BINARY_TYPE == 0, then this is \
                      actually a TEXT column (or a BLOB created       \
                      with < 4.0.14; since column prefix indexes      \
                      came only in 4.0.14, the missing flag in BLOBs  \
                      created before that does not cause any harm) */
#define DATA_INT 6 /* integer: can be any size 1 - 8 bytes */
#define DATA_SYS_CHILD 7 /* address of the child page in node pointer */
#define DATA_SYS 8 /* system column */
/* Data types >= DATA_FLOAT must be compared using the whole field, not as binary strings */
#define DATA_FLOAT 9
#define DATA_DOUBLE 10
#define DATA_DECIMAL 11 /* decimal number stored as an ASCII string */
#define DATA_VARMYSQL 12  /* any charset varying length char */
#define DATA_MYSQL 13  /* any charset fixed length char */
                         /* NOTE that 4.1.1 used DATA_MYSQL and
                         DATA_VARMYSQL for all character sets, and the
                         charset-collation for tables created with it
                         can also be latin1_swedish_ci */

/* DATA_POINT&DATA_VAR_POINT are for standard geometry datatype 'point' and
DATA_GEOMETRY include all other standard geometry datatypes as described in
OGC standard(line_string, polygon, multi_point, multi_polygon,
multi_line_string, geometry_collection, geometry).
Currently, geometry data is stored in the standard Well-Known Binary(WKB)
format (http://www.opengeospatial.org/standards/sfa).
We use BLOB as underlying datatype for DATA_GEOMETRY and DATA_VAR_POINT
while CHAR for DATA_POINT */
#define DATA_GEOMETRY 14 /* geometry datatype of variable length */
/* The following two are disabled temporarily, we won't create them in
get_innobase_type_from_mysql_type().
TODO: We will enable DATA_POINT/them when we come to the fixed-length POINT
again. */
#define DATA_POINT 15 /* geometry datatype of fixed length POINT */
#define DATA_VAR_POINT 16 /* geometry datatype of variable length \
                             POINT, used when we want to store POINT \
                             as BLOB internally */
#define DATA_MTYPE_MAX  63 /* dtype_store_for_order_and_null_size() \
                             requires the values are <= 63 */

#define MAX_CHAR_COLL_NUM 32767 /* We now support 15 bits (up to 32767) collation number */

#define CHAR_COLL_MASK MAX_CHAR_COLL_NUM /* Mask to get the Charset Collation number (0x7fff) */

#define DATA_NOT_NULL 256 /* this is ORed to the precise type when the column is declared as NOT NULL */
#define DATA_UNSIGNED 512 /* this id ORed to the precise type when we have an unsigned integer type */
#define DATA_BINARY_TYPE 1024 /* if the data type is a binary character string, this is ORed to the precise type: \
                        this only holds for tables created with >= MySQL-4.0.14 */

#define TMP_DIR "tmp"
#define TSE_TMP_TABLE 1
#define TSE_INTERNAL_TMP_TABLE 2
#define TSE_TABLE_CONTAINS_VIRCOL 4

#define CTC_ANALYZE_TIME_SEC 100


/* cond pushdown */
#define INVALID_MAX_COLUMN (uint16_t)0xFFFF

#define RETURN_IF_OOM(result)               \
    {                                       \
        if (result == ERR_ALLOC_MEMORY)     \
            return HA_ERR_SE_OUT_OF_MEMORY; \
    }

#define IS_METADATA_NORMALIZATION() (tse_get_metadata_switch() == (int32_t)metadata_switchs::MATCH_META)
#define IS_PRIMARY_ROLE() (tse_get_cluster_role() == (int32_t)dis_cluster_role::PRIMARY)
#define IS_STANDBY_ROLE() (tse_get_cluster_role() == (int32_t)dis_cluster_role::STANDBY)

static const dd::String_type index_file_name_val_key("index_file_name");
static const uint ROW_ID_LENGTH = sizeof(uint64_t);
static const uint TSE_START_TIMEOUT = 120; // seconds
extern const char *tse_hton_name;
extern bool no_create_dir;
enum class lock_mode {
  NO_LOCK = 0,
  SHARED_LOCK,
  EXCLUSIVE_LOCK
};

enum class metadata_switchs {
  DEFAULT = -1,
  NOT_MATCH,
  MATCH_META,
  MATCH_NO_META,
  CLUSTER_NOT_READY
};

enum class dis_cluster_role {
  DEFAULT = -1,
  PRIMARY,
  STANDBY,
  CLUSTER_NOT_READY
};

typedef int (*tse_prefetch_fn)(tianchi_handler_t *tch, uint8_t *records, uint16_t *record_lens,
                               uint32_t *recNum, uint64_t *rowids, int32 max_row_size);

#define FILL_USER_INFO_WITH_THD(ctrl, _thd)                                                                        \
    do {                                                                                                           \
        const char *user_name = _thd->m_main_security_ctx.priv_user().str;                                         \
        const char *user_ip = _thd->m_main_security_ctx.priv_host().str;                                           \
        assert(strlen(user_name) + 1 <= sizeof(ctrl.user_name));                                                   \
        memcpy(ctrl.user_name, user_name, strlen(user_name) + 1);                                                  \
        assert(strlen(user_ip) + 1 <= sizeof(ctrl.user_ip));                                                       \
        memcpy(ctrl.user_ip, user_ip, strlen(user_ip) + 1);                                                        \
    } while (0)

#define FILL_BROADCAST_BASE_REQ(broadcast_req, _sql_str, _user_name, _user_ip, _mysql_inst_id, _sql_command)       \
    do {                                                                                                           \
        (broadcast_req).mysql_inst_id = (_mysql_inst_id);                                                          \
        (broadcast_req).sql_command = (_sql_command);                                                              \
        strncpy((broadcast_req).sql_str, (_sql_str), MAX_DDL_SQL_LEN - 1);                                         \
        strncpy((broadcast_req).user_name, (_user_name), SMALL_RECORD_SIZE - 1);                                   \
        strncpy((broadcast_req).user_ip, (_user_ip), SMALL_RECORD_SIZE - 1);                                       \
    } while (0)

#define CHECK_HAS_MEMBER_IMPL(member)                                                                              \
template<typename T>                                                                                               \
struct check_has_##member {                                                                                        \
  template <typename _T>static auto check(_T)->typename std::decay<decltype(_T::member)>::type;                    \
  static void check(...);                                                                                          \
  using type = decltype(check(std::declval<T>()));                                                                 \
  constexpr static bool value = !std::is_void<type>::value;                                                        \
};

#define CHECK_HAS_MEMBER_FUNC_IMPL(NAME)                                                  \
template<typename T, typename... Args>                                                  \
struct check_has_member_func_##NAME {                                                              \
    template<typename U>                                                                \
    constexpr static auto check(const void*)->                                          \
        decltype(std::declval<U>().NAME(std::declval<Args>()...), std::true_type());    \
                                                                                        \
    template<typename U>                                                                \
    constexpr static std::false_type check(...);                                        \
                                                                                        \
    static constexpr bool value = decltype(check<T>(nullptr))::value;                   \
};

CHECK_HAS_MEMBER_IMPL(get_inst_id)
CHECK_HAS_MEMBER_IMPL(get_metadata_switch)
CHECK_HAS_MEMBER_IMPL(set_metadata_switch)
CHECK_HAS_MEMBER_IMPL(get_metadata_status)
CHECK_HAS_MEMBER_IMPL(op_before_load_meta)
CHECK_HAS_MEMBER_IMPL(op_after_load_meta)
CHECK_HAS_MEMBER_IMPL(drop_database_with_err)
CHECK_HAS_MEMBER_IMPL(binlog_log_query_with_err)
 
CHECK_HAS_MEMBER_FUNC_IMPL(is_empty)
CHECK_HAS_MEMBER_FUNC_IMPL(invalidates)

#define CHECK_HAS_MEMBER_FUNC(CLASS, MEMBER, ...) \
    check_has_member_func_##MEMBER<CLASS>::value

#define CHECK_HAS_MEMBER(type, member) check_has_##member<type>::value

/** @brief
  Tse_share is a class that will be shared among all open handlers.
*/
class Tse_share : public Handler_share {
 public:
  tianchi_cbo_stats_t *cbo_stats = nullptr;
  int used_count = 0;
  bool need_fetch_cbo = false;
  time_t get_cbo_time = 0;
};

/** @brief
  Class definition for the storage engine
*/

class ha_tse : public handler {
public:
  ha_tse(handlerton *hton, TABLE_SHARE *table);
  ~ha_tse();

  int initialize();

  template<typename T>
  T *get_share() {
    DBUG_TRACE;
    T *tmp_share = nullptr;

    lock_shared_ha_data();
    if (!(tmp_share = static_cast<T *>(get_ha_share_ptr()))) {
      tmp_share = new T;
      if (!tmp_share) goto err;
      set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
    }
    tmp_share->used_count++;
  err:
    unlock_shared_ha_data();
    return tmp_share;
  }

  template<typename T>
  void free_share() {
    DBUG_TRACE;
    T *tmp_share;

    lock_shared_ha_data();
    if ((tmp_share = static_cast<T *>(get_ha_share_ptr()))) {
      if (!--tmp_share->used_count) {
        free_cbo_stats();
        delete tmp_share;
        set_ha_share_ptr(static_cast<Handler_share *>(nullptr));
      }
    }
    unlock_shared_ha_data();
  }

  bool check_unsupported_operation(THD *thd, HA_CREATE_INFO *create_info);

  void check_error_code_to_mysql(THD *thd, ct_errno_t *ret);
  
  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const override {
    DBUG_TRACE;
    return "CTC";
  }

  /**
    Replace key algorithm with one supported by SE, return the default key
    algorithm for SE if explicit key algorithm was not provided.

    @sa handler::adjust_index_algorithm().
  */
  virtual enum ha_key_alg get_default_index_algorithm() const override {
    DBUG_TRACE;
    return HA_KEY_ALG_BTREE;
  }
  
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    /* This method is never used for FULLTEXT or SPATIAL keys.
    We rely on handler::ha_table_flags() to check if such keys
    are supported. */
    assert(key_alg != HA_KEY_ALG_FULLTEXT && key_alg != HA_KEY_ALG_RTREE);
    return key_alg == HA_KEY_ALG_BTREE;
  }
  /**
   Rows also use a fixed-size format.
   instead of create_info->table_options ::
   share->db_options_in_use//db_create_options (see hanler.h, see row_format in
   information_schema)
  */
  enum row_type get_real_row_type(const HA_CREATE_INFO *) const override {
    DBUG_TRACE;
    return ROW_TYPE_FIXED;
  }

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override {
    DBUG_TRACE;
    return (HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE | HA_NULL_IN_KEY | HA_DESCENDING_INDEX |
            HA_CAN_EXPORT | HA_GENERATED_COLUMNS | HA_NO_READ_LOCAL_LOCK |
            HA_SUPPORTS_DEFAULT_EXPRESSION | HA_CAN_INDEX_BLOBS | HA_CAN_SQL_HANDLER |
            HA_CAN_INDEX_VIRTUAL_GENERATED_COLUMN | HA_ATTACHABLE_TRX_COMPATIBLE);
  }

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint key, uint, bool) const override {
    DBUG_TRACE;

    if (table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT) return 0;

    ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE | HA_READ_ORDER |
                  HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;

    // @todo: Check if spatial indexes really have all these properties
    if (table_share->key_info[key].flags & HA_SPATIAL)
      flags |= HA_KEY_SCAN_NOT_ROR;

    return flags;
  }

  /** @brief
    unireg.cc will call max_supported_record_length(), max_supported_keys(),
    max_supported_key_parts(), uint max_supported_key_length()
    to make sure that the storage engine can handle the data it is about to
    send. Return *real* limits of your storage engine here; MySQL will do
    min(your_limits, MySQL_limits) automatically.
   */
  uint max_supported_record_length() const override {
    DBUG_TRACE;
    return CT_MAX_RECORD_LENGTH;
  }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
  */
  /* uint max_supported_keys() const {
    DBUG_TRACE;
    return (MAX_KEY);
  } */

  /** @details
    Set the maximum number of indexes per table
  */
  uint max_supported_keys() const override;

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  // uint max_supported_key_length() const { return (3500); }

  /** @details
    Get the maximum possible key length
  */
  uint max_supported_key_length() const override;

  /** @details
  Set the maximum columns of a composite index
*/
  uint max_supported_key_parts() const override;

  /** @details
    Get the maximum supported indexed columns length
  */
  uint max_supported_key_part_length(
      HA_CREATE_INFO *create_info) const override;

  /** @brief
    Called in test_quick_select to determine if indexes should be used.
  */
  virtual double scan_time() override {
    DBUG_TRACE;
    return (ulonglong)(stats.records + stats.deleted) / 100 + 2;
  }

  /** @brief
    This method will never be called if you do not implement indexes.
  */
  virtual double read_time(
    uint index,   /*!< in: key number */
    uint ranges,  /*!< in: how many ranges */
    ha_rows rows) /*!< in: estimated number of rows in the ranges */ override {
    DBUG_TRACE;

    if (index != table->s->primary_key) {
      /* Not clustered */
      return (handler::read_time(index, ranges, rows));
    }

    if (rows <= 2) {
      return ((double)rows);
    }

    /* Assume that the read time is proportional to the scan time for all
    rows + at most one seek per range. */

    double time_for_scan = scan_time();

    if (stats.records < rows) {
      return (time_for_scan);
    }

    return (ranges + (double)rows / (double)stats.records * time_for_scan);
  }

  bool inplace_alter_table(
      TABLE *altered_table MY_ATTRIBUTE((unused)),
      Alter_inplace_info *ha_alter_info MY_ATTRIBUTE((unused)),
      const dd::Table *old_table_def MY_ATTRIBUTE((unused)),
      dd::Table *new_table_def MY_ATTRIBUTE((unused))) override;

  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;  // required

  int close(void) override;  // required

#ifdef METADATA_NORMALIZED
  int write_row(uchar *buf, bool write_through = false) override;
#endif

#ifndef METADATA_NORMALIZED
  int write_row(uchar *buf) override;
#endif

  int update_row(const uchar *old_data, uchar *new_data) override;

  int delete_row(const uchar *buf) override ;

  int index_init(uint index, bool sorted) override;

  int index_end() override;

  int index_read(uchar *buf, const uchar *key, uint key_len,
                 ha_rkey_function find_flag) override;
  int index_read_last(uchar *buf, const uchar *key_ptr, uint key_len) override;

  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;

  int index_fetch(uchar *buf);

  /**
    @brief
    Positions an index cursor to the index specified in the handle. Fetches the
    row if available. If the key value is null, begin at the first key of the
    index. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  // int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
  //                    enum ha_rkey_function find_flag);

  /** @brief
    Used to read forward through the index.
    It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_next(uchar *buf) override;

  /** @brief
    Used to read backwards through the index.
    It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_prev(uchar *buf) override;

  /** @brief
    index_first() asks for the first key in the index.
    It's not an obligatory method;
    Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.
    skip it and and MySQL will treat it as not implemented.
  */
  int index_first(uchar *buf) override;

  /** @brief
    index_last() asks for the last key in the index.
    It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
    Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.
  */
  int index_last(uchar *buf) override;

  /** @brief
    rnd_init() is called when the system wants the storage engine to do a table
    scan. See the example in the introduction at the top of this file to see
    when rnd_init() is called.

    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.

    @details
    Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
    sql_table.cc, and sql_update.cc.

    @see
    filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
    sql_update.cc
  */
  int rnd_init(bool scan) override;  // required

  int rnd_end() override;

  /**
    @brief
    This is called for each row of the table scan. When you run out of records
    you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
    The Field structure for the table is the key to getting data into buf
    in a manner that will allow the server to understand it.

    @details
    Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
    sql_table.cc, and sql_update.cc.

    @see
    filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
    sql_update.cc
  */
  int rnd_next(uchar *buf) override;  ///< required

  /**
    @brief
    This is like rnd_next, but you are given a position to use
    to determine the row. The position will be of the type that you stored in
    ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
    or position you saved when position() was called.

    @details
    Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and
    sql_update.cc.

    @see
    filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
  */
  int rnd_pos(uchar *buf, uchar *pos) override;  ///< required

  int prefetch_and_fill_record_buffer(uchar *buf, tse_prefetch_fn);
  void fill_record_to_rec_buffer();
  void reset_rec_buf(bool is_prefetch = false);
  /**
    @brief
    position() is called after each call to rnd_next() if the data needs
    to be ordered. You can do something like the following to store
    the position:
    @code
    my_store_ptr(ref, ref_length, current_position);
    @endcode

    @details
    The server uses ref to store data. ref_length in the above case is
    the size needed to store current_position. ref is just a byte array
    that the server will maintain. If you are using offsets to mark rows, then
    current_position should be the offset. If it is a primary key like in
    BDB, then it needs to be a primary key.

    Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

    @see
    filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
  */
  void position(const uchar *record) override;  ///< required

  virtual void info_low();

  int info(uint) override;  ///< required

  /**
    @brief
    extra() is called whenever the server wishes to send a hint to
    the storage engine. The myisam engine implements the most hints.
    ha_innodb.cc has the most exhaustive list of these hints.

      @see
    ha_innodb.cc
  */
  int extra(enum ha_extra_function operation) override;

  /**
    @brief
    Used to delete all rows in a table, including cases of truncate and cases
    where the optimizer realizes that all rows will be removed as a result of an
    SQL statement.

    @details
    Called from item_sum.cc by Item_func_group_concat::clear(),
    Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
    Called from sql_delete.cc by mysql_delete().
    Called from sql_select.cc by JOIN::reinit().
    Called from sql_union.cc by st_select_lex_unit::exec().

    @see
    Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
    Item_func_group_concat::clear() in item_sum.cc;
    mysql_delete() in sql_delete.cc;
    JOIN::reinit() in sql_select.cc and
    st_select_lex_unit::exec() in sql_union.cc.
  */
  int delete_all_rows(void) override;

  /**
    Reset state of file to after 'open'.
    This function is called after every statement for all tables used
    by that statement.
  */
  int reset() override;

  /**
    @brief
    Given a starting key and an ending key, estimate the number of rows that
    will exist between the two keys.

    @details
    end_key may be empty, in which case determine if start_key matches any rows.

    Called from opt_range.cc by check_quick_keys().

    @see
    check_quick_keys() in opt_range.cc
  */
  ha_rows records_in_range(uint inx, key_range *min_key, key_range *max_key) override;

  int records(ha_rows *num_rows) override;
  int records_from_index(ha_rows *num_rows, uint inx) override;

  void set_tse_range_key(tse_key *tse_key, key_range *mysql_range_key, bool is_min_key);

  /**
    @brief
    Used to delete a table. By the time delete_table() has been called all
    opened references to this table will have been closed (and your globally
    shared references released). The variable name will just be the name of
    the table. You will need to remove any files you have created at this point.

    @details
    If you do not implement this, the default delete_table() is called from
    handler.cc and it will delete all files with the file extensions from
    handlerton::file_extensions.

    Called from handler.cc by delete_table and ha_create_table(). Only used
    during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
    the storage engine.

    @see
    delete_table and ha_create_table() in handler.cc
  */
  int delete_table(const char *from, const dd::Table *table_def) override;

  // void drop_table(const char *name);

  /**
    @brief
    create() is called to create a database. The variable name will have the
    name of the table.
    create() is called to create a database. The variable name will have the
    name of the table.

    @details
    When create() is called you do not need to worry about
    opening the table. Also, the .frm file will have already been
    created so adjusting create_info is not necessary. You can overwrite
    the .frm file at this point if you wish to change the table
    definition, but there are no methods currently provided for doing
    so.

    Called from handle.cc by ha_create_table().

    @see
    ha_create_table() in handle.cc
  */
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;  ///< required

  /**
  @brief
  Get number of lock objects returned in store_lock.

  @details
  Returns the number of store locks needed in call to store lock.
  We return number of partitions we will lock multiplied with number of
  locks needed by each partition. Assists the above functions in allocating
  sufficient space for lock structures.
*/
  uint lock_count(void) const  override{ return 0; }

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;  ///< required

  /**
  @brief
  This will start/end a transaction for statement, reference to innodb engine,
  for lock tables command will lock tables, but for now ignore this.

  @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

  @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
  int external_lock(THD *thd, int lock_type) override;

  /**
  @brief
  This will start/end a transaction for statement, reference to innodb engine,
  for insert/update/delete command on temp table will lock tables, but for now
  ignore this.

  @details
  Called from sql_base.cc by check_lock_and_start_stmt().

  @see
  sql_base.cc by check_lock_and_start_stmt().
*/
  int start_stmt(THD *thd, thr_lock_type lock_type) override;

  bool can_prefetch_records() const ;
  uint32_t cur_pos_in_buf = INVALID_MAX_UINT32;    // current position in prefetched record buf
  /* Number of rows actually prefetched */
  uint32_t actual_fetched_nums = INVALID_MAX_UINT32;

  uint64_t cur_off_in_prefetch_buf = 0;

  uint32_t cur_fill_buf_index = 0;

  /** Find out if a Record_buffer is wanted by this handler, and what is
  the maximum buffer size the handler wants.

  @param[out] max_rows gets set to the maximum number of records to
              allocate space for in the buffer
  @retval true   if the handler wants a buffer
  @retval false  if the handler does not want a buffer */
  bool is_record_buffer_wanted(ha_rows *const max_rows) const override;
  uint max_col_index;

  /** Find max column index needed by optimizer in read-only scan
  in order to shrink bufsize when calling cantian_to_mysql func for filling
  data into record_buffer **/
  void set_max_col_index_4_reading();

  /* function pointer used for converting record into mysql format*/
  cnvrt_to_mysql_fn cnvrt_to_mysql_record;
  void update_create_info(
      HA_CREATE_INFO *create_info MY_ATTRIBUTE((unused))) override;

  /* function used for notifying SE to collect cbo stats */
  int analyze(THD *thd, HA_CHECK_OPT *check_opt) override;

  /* optimize is mapped to "ALTER TABLE tablename ENGINE=InnoDB" in innodb. it will return
     HA_ADMIN_TRY_ALTER when optimize, and then recreate + analyze will be done in sql_admin.cc
     in case HA_ADMIN_TRY_ALTER.
  */
  int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;

  /* functions used for notifying SE to start/terminate batch insertion */
  void start_bulk_insert(ha_rows rows) override;
  int end_bulk_insert() override;
  
  /** Check if Tse supports a particular alter table in-place
  @param altered_table TABLE object for new version of table.
  @param ha_alter_info Structure describing changes to be done
  by ALTER TABLE and holding data used during in-place alter.
  @retval HA_ALTER_INPLACE_NOT_SUPPORTED Not supported
  @retval HA_ALTER_INPLACE_NO_LOCK Supported
  @retval HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE
          Supported, but requires lock during main phase and
          exclusive lock during prepare phase.
  @retval HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE
          Supported, prepare phase requires exclusive lock.  */
  enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;

  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;

  int check(THD *thd, HA_CHECK_OPT *check_opt) override;

  bool get_error_message(int error, String *buf) override;

  int set_prefetch_buffer();

  void update_blob_addrs(uchar *record);

  void free_blob_addrs();

  int fill_and_cnvrt_key_info(index_key_info_t &key_info, ha_rkey_function find_flag, const uchar *key, uint key_len);

  virtual int initialize_cbo_stats();

  virtual void free_cbo_stats();

  virtual int get_cbo_stats_4share();

  const Item *cond_push(const Item *cond, bool other_tbls_ok) override;

  int engine_push(AQP::Table_access *table) override;

  bool get_se_private_data(dd::Table *dd_table, bool reset) override;

  bool is_replay_ddl(MYSQL_THD thd);

 protected:
  /* fooling percona-server's gap lock detection on rr which blocks some SQL */
  virtual bool has_gap_locks() const noexcept { return true; }

 protected:

  /* tianchi handler that can be used to determine who sends the request */
  tianchi_handler_t m_tch;

  /* cantian record buffer */
  uchar *m_tse_buf;

  /* select lock mode */
  lock_mode m_select_lock = lock_mode::NO_LOCK;

  bool m_is_covering_index = false;

  bool m_is_replace = false;

  bool m_is_insert_dup = false;

  bool m_ignore_dup = false;

  expected_cursor_action_t m_action;

  bool m_index_sorted;

  bool m_error_if_not_empty = false;

  /**
   * Store tmp cantian format record when calling batch read intfs.
   */
  uchar *m_prefetch_buf = nullptr;

  /* cached record lens of prefetched row */
  uint16_t m_record_lens[MAX_BATCH_FETCH_NUM];

  /* cached rowids in case position is called during prefetch*/
  uint64_t m_rowids[MAX_BATCH_FETCH_NUM];

  /* estimated cantian record length*/
  int32 m_cantian_rec_len;

  /* max number of records that can be batch inserted at once */
  uint64_t m_max_batch_num;

  /* private record buffer */
  uchar *m_rec_buf_data = nullptr;

  Record_buffer *m_rec_buf;

  Record_buffer *m_rec_buf_4_writing = nullptr;

  vector<uchar *> m_blob_addrs;

  /** Pointer to Tse_share on the TABLE_SHARE. */
  Tse_share *m_share;

  tse_select_mode_t get_select_mode();

  /** Pointer to Cond pushdown */
  tse_conds *m_cond = nullptr;
  Item *m_pushed_conds = nullptr;
  Item *m_remainder_conds = nullptr;

private:
  int process_cantian_record(uchar *buf, record_info_t *record_info, ct_errno_t ct_ret, int rc_ret);
  int bulk_insert();
  virtual int bulk_insert_low(dml_flag_t flag, uint *dup_offset);
  int convert_mysql_record_and_write_to_cantian(uchar *buf, int *cantian_record_buf_size,
                                                uint16_t *serial_column_offset, dml_flag_t flag);
  void prep_cond_push(const Item *cond);
  int handle_auto_increment(bool &has_explicit_autoinc);
  bool pre_check_for_cascade(bool is_update);
};

struct dict_col {
  uint mtype;
  uint prtype;
  uint len;
};

#pragma pack(4)
typedef struct {
  uint64 sess_addr;
  uint thd_id;
  uint16 bind_core;
  uint is_tse_trx_begin;
  uint8_t sql_stat_start;  /* TRUE when we start processing of an SQL statement */
  std::unordered_map<tianchi_handler_t *, uint64_t> *cursors_map;
  std::vector<uint64_t> *invalid_cursors;
  void* msg_buf;
} thd_sess_ctx_s;
#pragma pack()

int ha_tse_get_inst_id();
void ha_tse_set_inst_id(uint32_t inst_id);

bool is_ddl_sql_cmd(enum_sql_command sql_cmd);
bool is_dcl_sql_cmd(enum_sql_command sql_cmd);
thd_sess_ctx_s *get_or_init_sess_ctx(handlerton *hton, THD *thd);
handlerton *get_tse_hton();
void update_sess_ctx_by_tch(tianchi_handler_t &tch, handlerton *hton, THD *thd);
void update_member_tch(tianchi_handler_t &tch, handlerton *hton, THD *thd, bool alloc_msg_buf = true);
int get_tch_in_handler_data(handlerton *hton, THD *thd, tianchi_handler_t &tch, bool alloc_msg_buf = true);
void tse_ddl_hook_cantian_error(const char *tag, THD *thd, ddl_ctrl_t *ddl_ctrl,
                                ct_errno_t *ret);
int tse_ddl_handle_fault(const char *tag, const THD *thd,
                         const ddl_ctrl_t *ddl_ctrl, ct_errno_t ret, const char *param = nullptr, int fix_ret = 0);
bool ddl_enabled_normal(MYSQL_THD thd);
bool engine_skip_ddl(MYSQL_THD thd);
bool engine_ddl_passthru(MYSQL_THD thd);
bool is_alter_table_copy(MYSQL_THD thd, const char *name = nullptr);
bool is_alter_table_scan(bool m_error_if_not_empty);

int tse_fill_conds(const Item *pushed_cond, Field **field, tse_conds *m_cond, bool no_backslash);
void free_m_cond(tianchi_handler_t m_tch, tse_conds **conds);
void tse_set_metadata_switch();
int32_t tse_get_metadata_switch();
bool is_meta_version_initialize();
bool user_var_set(MYSQL_THD thd, string target_str);
bool is_initialize();
bool is_starting();

bool tse_is_temporary(const dd::Table *table_def);
int32_t tse_get_cluster_role();
void tse_set_mysql_read_only();
void tse_reset_mysql_read_only();

#pragma GCC visibility pop
#endif
