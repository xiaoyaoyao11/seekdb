/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include <cerrno>
#include <cstdlib>

#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER_INHERIT(ObFileTableSpec,
                            ObOpSpec,
                            column_exprs_,
                            path_,
                            func_type_,
                            delimiter_,
                            quote_char_,
                            has_header_,
                            sample_size_,
                            col_types_);

namespace
{

/**
 * The readers that can back a file table.  The values mirror the
 * ObFileTableFuncType enum of the resolver.
 */
enum ObFileReaderKind
{
  FILE_READER_CSV     = 0,
  FILE_READER_JSON    = 1,
  FILE_READER_NDJSON  = 2,
  FILE_READER_PARQUET = 3,
  FILE_READER_GLOB    = 4,
};

const int64_t USECS_PER_SEC = 1000000LL;
const int64_t USECS_PER_DAY = 86400LL * USECS_PER_SEC;

/**
 * Days between 1970-01-01 and the given proleptic gregorian date.
 * The algorithm works for any year and never touches the C library, so the
 * result does not depend on the time zone of the server.
 */
int32_t days_from_civil(int64_t year, int64_t month, int64_t day)
{
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int32_t>(era * 146097 + doe - 719468);
}

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool read_fixed_int(const std::string &value,
                    size_t &pos,
                    int digits,
                    int64_t &result)
{
  if (pos + digits > value.size()) {
    return false;
  }
  int64_t num = 0;
  for (int i = 0; i < digits; ++i) {
    const char c = value[pos + i];
    if (!is_digit(c)) {
      return false;
    }
    num = num * 10 + (c - '0');
  }
  pos += digits;
  result = num;
  return true;
}

/**
 * Parse a calendar date, optionally followed by a time of day.
 *
 * Accepted shapes are "YYYY-MM-DD", "YYYY-MM-DD HH:MM:SS" and
 * "YYYY-MM-DDTHH:MM:SS"; the fraction of a second is optional.
 */
bool parse_date_time(const std::string &value,
                     int64_t &days,
                     int64_t &usecs)
{
  size_t pos = 0;
  int64_t year = 0, month = 0, day = 0;
  if (!read_fixed_int(value, pos, 4, year)) { return false; }
  if (pos >= value.size() || value[pos] != '-') { return false; }
  ++pos;
  if (!read_fixed_int(value, pos, 2, month)) { return false; }
  if (pos >= value.size() || value[pos] != '-') { return false; }
  ++pos;
  if (!read_fixed_int(value, pos, 2, day)) { return false; }
  if (month < 1 || month > 12 || day < 1 || day > 31) { return false; }
  days = days_from_civil(year, month, day);

  usecs = 0;
  if (pos >= value.size()) {
    return true;
  }
  const char sep = value[pos];
  if (sep != ' ' && sep != 'T' && sep != 't') { return false; }
  ++pos;
  int64_t hour = 0, minute = 0, second = 0;
  if (!read_fixed_int(value, pos, 2, hour)) { return false; }
  if (pos >= value.size() || value[pos] != ':') { return false; }
  ++pos;
  if (!read_fixed_int(value, pos, 2, minute)) { return false; }
  if (pos >= value.size() || value[pos] != ':') { return false; }
  ++pos;
  if (!read_fixed_int(value, pos, 2, second)) { return false; }
  if (hour > 23 || minute > 59 || second > 59) { return false; }
  int64_t fraction = 0;
  if (pos < value.size() && (value[pos] == '.' || value[pos] == ':')) {
    // a fraction of a second, either ".123456" or ":123456"
    if (value[pos] == ':') { ++pos; }
    else { ++pos; }
    int digits = 0;
    while (pos < value.size() && is_digit(value[pos]) && digits < 6) {
      fraction = fraction * 10 + (value[pos] - '0');
      ++pos;
      ++digits;
    }
    while (digits < 6) {
      fraction *= 10;
      ++digits;
    }
  }
  usecs = hour * 3600 * USECS_PER_SEC + minute * 60 * USECS_PER_SEC + second * USECS_PER_SEC
          + fraction;
  return true;
}

} // anonymous namespace

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  is_glob_ = (FILE_READER_GLOB == MY_SPEC.func_type_);
  glob_idx_ = 0;
  glob_files_.clear();
  fields_.clear();
  nulls_.clear();
  if (OB_FAIL(open_source())) {
  } else {
    is_opened_ = true;
  }
  return ret;
}

int ObFileTableOp::open_source()
{
  int ret = OB_SUCCESS;
  const std::string path(MY_SPEC.path_.ptr(), MY_SPEC.path_.length());
  file_scan::Error error;
  if (is_glob_) {
    if (file_scan::Status::OK != file_scan::glob_files(path, &glob_files_, &error)) {
      LOG_WARN("failed to expand the glob pattern", K(path.c_str()), K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (FILE_READER_CSV == MY_SPEC.func_type_) {
    file_scan::ReadOptions options;
    options.format = file_scan::Format::CSV;
    options.header = (0 != MY_SPEC.has_header_);
    options.sample_size = MY_SPEC.sample_size_;
    options.delimiter = static_cast<char>(MY_SPEC.delimiter_);
    options.quote = static_cast<char>(MY_SPEC.quote_char_);
    if (file_scan::Status::OK != csv_scanner_.open(path, options, &error)) {
      LOG_WARN("failed to open the csv file", K(path.c_str()), K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (FILE_READER_JSON == MY_SPEC.func_type_ || FILE_READER_NDJSON == MY_SPEC.func_type_) {
    file_scan::ReadOptions options;
    options.format = file_scan::Format::JSONL;
    options.header = true;
    options.sample_size = MY_SPEC.sample_size_;
    if (file_scan::Status::OK != jsonl_scanner_.open(path, options, &error)) {
      LOG_WARN("failed to open the json file", K(path.c_str()), K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (FILE_READER_PARQUET == MY_SPEC.func_type_) {
    file_scan::ReadOptions options;
    options.format = file_scan::Format::PARQUET;
    options.header = true;
    options.sample_size = MY_SPEC.sample_size_;
    if (file_scan::Status::OK != parquet_scanner_.open(path, options, &error)) {
      LOG_WARN("failed to open the parquet file", K(path.c_str()), K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  } else {
    LOG_WARN("unsupported file reader", K(MY_SPEC.func_type_), K(path.c_str()));
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ObFileTableOp::close_source()
{
  int ret = OB_SUCCESS;
  csv_scanner_.close();
  jsonl_scanner_.close();
  parquet_scanner_.close();
  glob_files_.clear();
  glob_idx_ = 0;
  fields_.clear();
  nulls_.clear();
  is_opened_ = false;
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  file_scan::Error error;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (is_glob_) {
    glob_idx_ = 0;
  } else if (FILE_READER_CSV == MY_SPEC.func_type_) {
    if (file_scan::Status::OK != csv_scanner_.rewind(&error)) {
      LOG_WARN("failed to rewind the csv file", K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (FILE_READER_JSON == MY_SPEC.func_type_ || FILE_READER_NDJSON == MY_SPEC.func_type_) {
    if (file_scan::Status::OK != jsonl_scanner_.rewind(&error)) {
      LOG_WARN("failed to rewind the json file", K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (FILE_READER_PARQUET == MY_SPEC.func_type_) {
    if (file_scan::Status::OK != parquet_scanner_.rewind(&error)) {
      LOG_WARN("failed to rewind the parquet file", K(error.message.c_str()));
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObFileTableOp::switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_switch_iterator())) {
  } else {
    glob_idx_ = 0;
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(try_check_status())) {
  } else {
    clear_evaluated_flag();
    if (OB_FAIL(fill_row())) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to read the next row of the file", K(ret), K(MY_SPEC.path_));
      }
    } else {
      LOG_DEBUG("output row", "row", ROWEXPR2STR(eval_ctx_, MY_SPEC.output_));
    }
  }
  return ret;
}

int ObFileTableOp::next_record(bool &has_row)
{
  int ret = OB_SUCCESS;
  file_scan::Error error;
  has_row = false;
  if (FILE_READER_CSV == MY_SPEC.func_type_) {
    if (!csv_scanner_.next_row(&fields_, &nulls_, &error)) {
      if (!error.ok()) {
        LOG_WARN("failed to read the csv record",
                 K(static_cast<int32_t>(error.status)), K(error.line), K(error.message.c_str()));
        ret = OB_ERR_UNEXPECTED;
      }
    } else {
      has_row = true;
    }
  } else if (FILE_READER_JSON == MY_SPEC.func_type_ || FILE_READER_NDJSON == MY_SPEC.func_type_) {
    if (!jsonl_scanner_.next_row(&fields_, &nulls_, &error)) {
      if (!error.ok()) {
        LOG_WARN("failed to read the json record",
                 K(static_cast<int32_t>(error.status)), K(error.line), K(error.message.c_str()));
        ret = OB_ERR_UNEXPECTED;
      }
    } else {
      has_row = true;
    }
  } else if (FILE_READER_PARQUET == MY_SPEC.func_type_) {
    if (!parquet_scanner_.next_row(&fields_, &nulls_, &error)) {
      if (!error.ok()) {
        LOG_WARN("failed to read the parquet record",
                 K(static_cast<int32_t>(error.status)), K(error.line), K(error.message.c_str()));
        ret = OB_ERR_UNEXPECTED;
      }
    } else {
      has_row = true;
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported file reader", K(MY_SPEC.func_type_));
  }
  return ret;
}

int ObFileTableOp::fill_row()
{
  int ret = OB_SUCCESS;
  const int64_t column_cnt = MY_SPEC.column_exprs_.count();
  if (is_glob_) {
    if (glob_idx_ >= static_cast<int64_t>(glob_files_.size())) {
      ret = OB_ITER_END;
    } else if (column_cnt < 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObExpr *column_expr = MY_SPEC.column_exprs_.at(0);
      if (OB_FAIL(set_glob_datum(column_expr))) {
      }
    }
  } else {
    bool has_row = false;
    if (OB_FAIL(next_record(has_row))) {
    } else if (!has_row) {
      ret = OB_ITER_END;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < column_cnt; ++i) {
        ObExpr *column_expr = MY_SPEC.column_exprs_.at(i);
        if (OB_FAIL(set_column_datum(i, column_expr))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    // the output expressions that are not produced by this operator have to be
    // evaluated as well
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.output_.count(); ++i) {
      ObExpr *output = MY_SPEC.output_.at(i);
      bool is_column_expr = false;
      for (int64_t j = 0; OB_SUCC(ret) && j < MY_SPEC.column_exprs_.count(); ++j) {
        if (MY_SPEC.column_exprs_.at(j) == output) {
          is_column_expr = true;
          break;
        }
      }
      if (!is_column_expr) {
        ObDatum *datum = NULL;
        if (OB_FAIL(output->eval(eval_ctx_, datum))) {
        }
      }
    }
  }
  return ret;
}

int ObFileTableOp::set_glob_datum(ObExpr *dst_expr)
{
  int ret = OB_SUCCESS;
  const std::string &file = glob_files_.at(glob_idx_);
  ObDatum &dst = dst_expr->locate_datum_for_write(eval_ctx_);
  dst.set_string(file.c_str(), static_cast<uint32_t>(file.size()));
  dst_expr->set_evaluated_projected(eval_ctx_);
  ++glob_idx_;
  return ret;
}

int ObFileTableOp::set_column_datum(int64_t col_idx, ObExpr *dst_expr)
{
  int ret = OB_SUCCESS;
  ObDatum &dst = dst_expr->locate_datum_for_write(eval_ctx_);
  const bool is_null = (col_idx >= static_cast<int64_t>(nulls_.size())) || nulls_.at(col_idx);
  if (is_null) {
    dst.set_null();
  } else {
    const std::string &value = fields_.at(col_idx);
    const int32_t type = MY_SPEC.col_types_.at(col_idx);
    switch (type) {
      case ObNullType: {
        dst.set_null();
        break;
      }
      case ObTinyIntType: {
        // the inferred schema stores booleans as true / false
        bool value_bool = false;
        if (0 == value.compare("true")) {
          value_bool = true;
        } else if (0 == value.compare("false")) {
          value_bool = false;
        } else {
          value_bool = (0 == value.compare("1"));
        }
        dst.set_bool(value_bool);
        break;
      }
      case ObIntType: {
        errno = 0;
        char *end = NULL;
        const long long parsed = strtoll(value.c_str(), &end, 10);
        if (NULL == end || errno != 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to convert the value to an integer", K(col_idx), K(value.c_str()));
        } else {
          dst.set_int(static_cast<int64_t>(parsed));
        }
        break;
      }
      case ObDoubleType: {
        errno = 0;
        char *end = NULL;
        const double parsed = strtod(value.c_str(), &end);
        if (NULL == end || errno != 0) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to convert the value to a double", K(col_idx), K(value.c_str()));
        } else {
          dst.set_double(parsed);
        }
        break;
      }
      case ObDateType: {
        int64_t days = 0;
        int64_t usecs = 0;
        if (!parse_date_time(value, days, usecs)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to convert the value to a date", K(col_idx), K(value.c_str()));
        } else {
          dst.set_date(static_cast<int32_t>(days));
        }
        break;
      }
      case ObDateTimeType: {
        int64_t days = 0;
        int64_t usecs = 0;
        if (!parse_date_time(value, days, usecs)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to convert the value to a datetime", K(col_idx), K(value.c_str()));
        } else {
          dst.set_datetime(days * USECS_PER_DAY + usecs);
        }
        break;
      }
      default: {
        // varchar and every other type are exposed as the raw text
        dst.set_string(value.c_str(), static_cast<uint32_t>(value.size()));
        break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    dst_expr->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(close_source())) {
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
