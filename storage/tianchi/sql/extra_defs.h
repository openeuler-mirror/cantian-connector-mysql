typedef float rec_per_key_t;
constexpr const int DATETIME_MAX_DECIMALS = 6;
void set_records_per_key(KEY *key, uint key_part_no, ulong rec_per_key_est);
bool field_has_flag(const Field *fld, int flag_bit);

/**
 Predicate which returns true if at least one of the date members are non-zero.

 @param my_time time point to check.
 @retval false if all the date members are zero
 @retval true otherwise
 */
static inline bool non_zero_date(const MYSQL_TIME &my_time) {
  return my_time.year || my_time.month || my_time.day;
}

/**
 Predicate which returns true if at least one of the time members are non-zero.

 @param my_time time point to check.
 @retval false if all the time members are zero
 @retval true otherwise
*/
static inline bool non_zero_time(const MYSQL_TIME &my_time) {
  return my_time.hour || my_time.minute || my_time.second ||
         my_time.second_part;
}

