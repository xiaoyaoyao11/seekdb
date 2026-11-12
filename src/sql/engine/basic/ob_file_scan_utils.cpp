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

/**
 * @file ob_file_scan_utils.cpp
 *
 * Implementation of the neutral scanning helpers used by the file table
 * functions.  See ob_file_scan_utils.h for the contract.
 */

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
// The Windows branch exists so that the scanning rules can be exercised by
// the unit tests on a development machine.  The production build is Linux
// only and uses the POSIX interfaces below.
# include <filesystem>
# include <windows.h>
#else
# include <dirent.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace file_scan
{

namespace
{

const size_t READ_BUFFER_SIZE = 64 * 102;

/// Open a file for reading.  The POSIX build passes the path through
/// unchanged, the Windows development branch converts it to UTF-16 so that
/// non ASCII paths keep working.
std::FILE *open_file(const std::string &path, const char *mode)
{
#if defined(_WIN32)
  const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                            static_cast<int>(path.size()), nullptr, 0);
  if (wide_len <= 0) {
    return nullptr;
  }
  std::wstring wide_path(static_cast<size_t>(wide_len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                      &wide_path[0], wide_len);
  std::wstring wide_mode;
  for (const char *m = mode; '\0' != *m; ++m) {
    wide_mode.push_back(static_cast<wchar_t>(*m));
  }
  return _wfopen(wide_path.c_str(), wide_mode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

/// Fold a string to lower case without touching the locale.
std::string to_lower(const std::string &value)
{
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[i]))));
  }
  return result;
}

/// Parse a decimal integer.  Returns false when the text is not an integer or
/// when it does not fit into a signed 64 bit value; `overflow` tells the two
/// cases apart.
bool parse_integer_strict(const std::string &value, bool *overflow)
{
  if (nullptr != overflow) {
    *overflow = false;
  }
  if (value.empty()) {
    return false;
  }
  size_t pos = 0;
  if ('+' == value[pos] || '-' == value[pos]) {
    ++pos;
  }
  if (pos >= value.size()) {
    return false;
  }
  for (size_t i = pos; i < value.size(); ++i) {
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  errno = 0;
  char *end = nullptr;
  (void)std::strtoll(value.c_str(), &end, 10);
  if (nullptr == end || '\0' != *end) {
    return false;
  }
  if (ERANGE == errno) {
    if (nullptr != overflow) {
      *overflow = true;
    }
    return false;
  }
  return true;
}

/// Split a timestamp into its date and time parts.
bool split_date_time(const std::string &value, std::string *date_part, std::string *time_part)
{
  if (value.size() < 10) {
    return false;
  }
  if ('-' != value[4] || '-' != value[7]) {
    return false;
  }
  date_part->assign(value, 0, 10);
  if (10 == value.size()) {
    time_part->clear();
    return true;
  }
  const char sep = value[10];
  if (' ' != sep && 'T' != sep && 't' != sep) {
    return false;
  }
  *time_part = value.substr(11);
  return true;
}

/// True when the text is a valid time of day, optionally with a fraction.
bool is_valid_time(const std::string &value)
{
  if (value.size() < 8) {
    return false;
  }
  if (':' != value[2] || ':' != value[5]) {
    return false;
  }
  for (size_t i = 0; i < 8; ++i) {
    if (2 == i || 5 == i) {
      continue;
    }
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  const int hour   = (value[0] - '0') * 10 + (value[1] - '0');
  const int minute = (value[3] - '0') * 10 + (value[4] - '0');
  const int second = (value[6] - '0') * 10 + (value[7] - '0');
  if (hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  if (8 == value.size()) {
    return true;
  }
  if ('.' != value[8]) {
    return false;
  }
  const std::string fraction = value.substr(9);
  if (fraction.empty()) {
    return false;
  }
  for (size_t i = 0; i < fraction.size(); ++i) {
    if (fraction[i] < '0' || fraction[i] > '9') {
      return false;
    }
  }
  return true;
}

/// Number of days in the given month, honouring the leap year rule.
bool days_in_month(int year, int month, int *days)
{
  static const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return false;
  }
  int count = month_days[month - 1];
  if (2 == month) {
    const bool leap = (0 == year % 4 && 0 != year % 100) || (0 == year % 400);
    if (leap) {
      count = 29;
    }
  }
  if (nullptr != days) {
    *days = count;
  }
  return true;
}

/// Match a single path component against a glob pattern using `*` and `?`.
bool match_component(const char *pattern, const char *name)
{
  const char *star = nullptr;
  const char *mark = nullptr;
  while ('\0' != *name) {
    if ('*' == *pattern) {
      star = pattern++;
      mark = name;
    } else if ('?' == *pattern || *pattern == *name) {
      ++pattern;
      ++name;
    } else if (nullptr != star) {
      pattern = star + 1;
      name   = ++mark;
    } else {
      return false;
    }
  }
  while ('*' == *pattern) {
    ++pattern;
  }
  return '\0' == *pattern;
}

/// Split a path into its components, ignoring empty parts.
std::vector<std::string> split_path(const std::string &path)
{
  std::vector<std::string> parts;
  std::string current;
  for (size_t i = 0; i < path.size(); ++i) {
    if ('/' == path[i] || '\\' == path[i]) {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(path[i]);
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

/// Join a directory and an entry name.
std::string join_path(const std::string &dir, const std::string &name)
{
  if (dir.empty()) {
    return name;
  }
  const char last = dir[dir.size() - 1];
  if ('/' == last || '\\' == last) {
    return dir + name;
  }
  return dir + "/" + name;
}

/// List the entries of a directory.  Returns false when the directory cannot
/// be opened.
bool list_directory(const std::string &dir, std::vector<std::string> *entries)
{
  entries->clear();
#if defined(_WIN32)
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::u8path(dir);
  for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().u8string();
    if ("." == name || ".." == name) {
      continue;
    }
    entries->push_back(name);
  }
  return !ec;
#else
  DIR *handle = opendir(dir.c_str());
  if (nullptr == handle) {
    return false;
  }
  struct dirent *entry = nullptr;
  while (nullptr != (entry = readdir(handle))) {
    const std::string name = entry->d_name;
    if ("." == name || ".." == name) {
      continue;
    }
    entries->push_back(name);
  }
  closedir(handle);
  return true;
#endif
}

/// True when the path exists and is a regular file.
bool is_regular_file(const std::string &path)
{
#if defined(_WIN32)
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::u8path(path), ec);
#else
  struct stat info;
  if (0 != stat(path.c_str(), &info)) {
    return false;
  }
  return S_ISREG(info.st_mode);
#endif
}

/// True when the path exists and is a directory.
bool is_directory(const std::string &path)
{
#if defined(_WIN32)
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::u8path(path), ec);
#else
  struct stat info;
  if (0 != stat(path.c_str(), &info)) {
    return false;
  }
  return S_ISDIR(info.st_mode);
#endif
}

/// Collect every directory below `dir`, recursively.
void collect_sub_directories(const std::string &dir, std::vector<std::string> *dirs)
{
  std::vector<std::string> entries;
  if (!list_directory(dir, &entries)) {
    return;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    const std::string child = join_path(dir, entries[i]);
    if (is_directory(child)) {
      dirs->push_back(child);
      collect_sub_directories(child, dirs);
    }
  }
}

/// Expand the directory part of a glob pattern into concrete directories.
void expand_directory_pattern(const std::string &dir_pattern, std::vector<std::string> *dirs)
{
  // The leading part of the pattern is the root the expansion starts from.
  std::string base = ".";
  std::string rest = dir_pattern;
  if (!rest.empty() && ('/' == rest[0] || '\\' == rest[0])) {
    base = "/";
    rest.erase(0, 1);
  } else if (rest.size() >= 2 && ':' == rest[1]
             && ((rest[0] >= 'A' && rest[0] <= 'Z') || (rest[0] >= 'a' && rest[0] <= 'z'))) {
    base = rest.substr(0, 2) + "/";
    rest = rest.substr(2);
  }

  std::vector<std::string> current;
  current.push_back(base);

  const std::vector<std::string> segments = split_path(rest);
  for (size_t s = 0; s < segments.size(); ++s) {
    const std::string &segment = segments[s];
    std::vector<std::string> next;
    if ("**" == segment) {
      for (size_t i = 0; i < current.size(); ++i) {
        next.push_back(current[i]);
        std::vector<std::string> children;
        collect_sub_directories(current[i], &children);
        for (size_t c = 0; c < children.size(); ++c) {
          next.push_back(children[c]);
        }
      }
    } else {
      for (size_t i = 0; i < current.size(); ++i) {
        std::vector<std::string> entries;
        if (!list_directory(current[i], &entries)) {
          continue;
        }
        for (size_t e = 0; e < entries.size(); ++e) {
          if (match_component(segment.c_str(), entries[e].c_str())) {
            next.push_back(join_path(current[i], entries[e]));
          }
        }
      }
    }
    current.swap(next);
  }
  dirs->swap(current);
}

} // namespace

const char *to_string(ColumnType type)
{
  switch (type) {
  case ColumnType::NULL_TYPE: return "NULL";
  case ColumnType::BOOLEAN:   return "BOOLEAN";
  case ColumnType::BIGINT:    return "BIGINT";
  case ColumnType::DOUBLE:    return "DOUBLE";
  case ColumnType::DATE:      return "DATE";
  case ColumnType::DATETIME:  return "DATETIME";
  case ColumnType::VARCHAR:   return "VARCHAR";
  }
  return "VARCHAR";
}

bool parse_format(const std::string &name, Format *format)
{
  if (nullptr == format) {
    return false;
  }
  const std::string lowered = to_lower(name);
  if ("csv" == lowered) {
    *format = Format::CSV;
  } else if ("json" == lowered || "jsonl" == lowered || "newline_delimited" == lowered
             || "ndjson" == lowered || "newline_delimited_json" == lowered) {
    *format = Format::JSONL;
  } else if ("parquet" == lowered) {
    *format = Format::PARQUET;
  } else if ("auto" == lowered) {
    *format = Format::AUTO;
  } else {
    return false;
  }
  return true;
}

ColumnType widen_type(ColumnType lhs, ColumnType rhs)
{
  if (ColumnType::NULL_TYPE == lhs) {
    return rhs;
  }
  if (ColumnType::NULL_TYPE == rhs) {
    return lhs;
  }
  if (lhs == rhs) {
    return lhs;
  }
  if (ColumnType::VARCHAR == lhs || ColumnType::VARCHAR == rhs) {
    return ColumnType::VARCHAR;
  }
  const bool lhs_temporal = (ColumnType::DATE == lhs || ColumnType::DATETIME == lhs);
  const bool rhs_temporal = (ColumnType::DATE == rhs || ColumnType::DATETIME == rhs);
  if (lhs_temporal != rhs_temporal) {
    // The numeric chain and the temporal chain are not comparable.
    return ColumnType::VARCHAR;
  }
  if (lhs_temporal) {
    return (ColumnType::DATE == lhs) ? rhs : lhs;
  }
  // BOOLEAN < BIGINT < DOUBLE
  int lhs_rank = 0;
  int rhs_rank = 0;
  if (ColumnType::BOOLEAN == lhs) {
    lhs_rank = 1;
  } else if (ColumnType::BIGINT == lhs) {
    lhs_rank = 2;
  } else {
    lhs_rank = 3;
  }
  if (ColumnType::BOOLEAN == rhs) {
    rhs_rank = 1;
  } else if (ColumnType::BIGINT == rhs) {
    rhs_rank = 2;
  } else {
    rhs_rank = 3;
  }
  return (lhs_rank >= rhs_rank) ? lhs : rhs;
}

void TypeInferrer::observe(const std::string &value, bool is_null)
{
  if (is_null) {
    has_null_ = true;
    return;
  }
  ColumnType observed = ColumnType::VARCHAR;
  if (looks_like_boolean(value)) {
    observed = ColumnType::BOOLEAN;
  } else if (looks_like_bigint(value)) {
    observed = ColumnType::BIGINT;
  } else if (looks_like_double(value)) {
    observed = ColumnType::DOUBLE;
  } else if (looks_like_datetime(value)) {
    observed = ColumnType::DATETIME;
  } else if (looks_like_date(value)) {
    observed = ColumnType::DATE;
  }
  type_ = widen_type(type_, observed);
}

bool looks_like_boolean(const std::string &value)
{
  const std::string lowered = to_lower(value);
  return "true" == lowered || "false" == lowered;
}

bool looks_like_bigint(const std::string &value)
{
  if (value.empty()) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] < '0' || value[i] > '9') {
      if (0 == i && ('+' == value[i] || '-' == value[i])) {
        continue;
      }
      return false;
    }
  }
  // A number that does not fit into a signed 64 bit integer is not a BIGINT,
  // it widens to DOUBLE instead.
  return parse_integer_strict(value, nullptr);
}

bool looks_like_double(const std::string &value)
{
  if (value.empty()) {
    return false;
  }
  bool has_digit = false;
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c >= '0' && c <= '9') {
      has_digit = true;
      continue;
    }
    if ('+' == c || '-' == c || '.' == c || 'e' == c || 'E' == c) {
      continue;
    }
    return false;
  }
  if (!has_digit) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  (void)std::strtod(value.c_str(), &end);
  if (nullptr == end || '\0' != *end) {
    return false;
  }
  return true;
}

bool looks_like_date(const std::string &value)
{
  if (10 != value.size()) {
    return false;
  }
  if ('-' != value[4] || '-' != value[7]) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    if (4 == i || 7 == i) {
      continue;
    }
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  const int year  = (value[0] - '0') * 1000 + (value[1] - '0') * 100 + (value[2] - '0') * 10 + (value[3] - '0');
  const int month = (value[5] - '0') * 10 + (value[6] - '0');
  const int day   = (value[8] - '0') * 10 + (value[9] - '0');
  int days = 0;
  if (!days_in_month(year, month, &days)) {
    return false;
  }
  return day >= 1 && day <= days;
}

bool looks_like_datetime(const std::string &value)
{
  std::string date_part;
  std::string time_part;
  if (!split_date_time(value, &date_part, &time_part)) {
    return false;
  }
  if (!looks_like_date(date_part)) {
    return false;
  }
  if (time_part.empty()) {
    return false;
  }
  return is_valid_time(time_part);
}

std::vector<std::string> make_unique_names(const std::vector<std::string> &names)
{
  std::vector<std::string> result = names;
  for (size_t i = 0; i < result.size(); ++i) {
    if (result[i].empty()) {
      result[i] = "column" + std::to_string(i);
    }
  }
  for (size_t i = 0; i < result.size(); ++i) {
    // Count how often the raw name occurs up to and including this position.
    size_t occurrence = 0;
    for (size_t j = 0; j <= i; ++j) {
      if (result[j] == result[i]) {
        ++occurrence;
      }
    }
    if (1 == occurrence) {
      continue;
    }
    size_t suffix = occurrence;
    std::string candidate = result[i] + "__" + std::to_string(suffix);
    // The generated name must not collide with an already used name.
    while (true) {
      bool taken = false;
      for (size_t j = 0; j < i; ++j) {
        if (result[j] == candidate) {
          taken = true;
          break;
        }
      }
      if (!taken) {
        break;
      }
      ++suffix;
      candidate = result[i] + "__" + std::to_string(suffix);
    }
    result[i] = candidate;
  }
  return result;
}

std::string strip_utf8_bom(const std::string &value)
{
  if (value.size() >= 3
      && 0xEF == static_cast<unsigned char>(value[0])
      && 0xBB == static_cast<unsigned char>(value[1])
      && 0xBF == static_cast<unsigned char>(value[2])) {
    return value.substr(3);
  }
  return value;
}

// ---------------------------------------------------------------------------
// CsvScanner
// ---------------------------------------------------------------------------

CsvScanner::~CsvScanner()
{
  close();
}

void CsvScanner::close()
{
  if (nullptr != file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  opened_          = false;
  header_consumed_ = false;
  eof_             = false;
  buffer_.clear();
  buffer_pos_ = 0;
}

bool CsvScanner::fill_buffer()
{
  if (eof_ || nullptr == file_) {
    return false;
  }
  buffer_.resize(READ_BUFFER_SIZE);
  const size_t read_bytes = std::fread(&buffer_[0], 1, buffer_.size(), file_);
  buffer_.resize(read_bytes);
  buffer_pos_ = 0;
  if (0 == read_bytes) {
    eof_ = true;
    return false;
  }
  return true;
}

int CsvScanner::peek_char()
{
  if (buffer_pos_ >= buffer_.size() && !fill_buffer()) {
    return -1;
  }
  return static_cast<unsigned char>(buffer_[buffer_pos_]);
}

int CsvScanner::get_char()
{
  const int c = peek_char();
  if (c >= 0) {
    ++buffer_pos_;
    if ('\n' == c) {
      ++current_line_;
    }
  }
  return c;
}

Status CsvScanner::open(const std::string &path, const ReadOptions &options, Error *error)
{
  close();
  path_              = path;
  options_           = options;
  header_consumed_   = false;
  eof_               = false;
  current_line_      = 1;
  record_start_line_ = 1;
  header_names_.clear();

  file_ = open_file(path, "rb");
  if (nullptr == file_) {
    if (nullptr != error) {
      if (ENOENT == errno) {
        error->set(Status::NOT_FOUND, std::string("file not found: ") + path);
      } else if (EACCES == errno) {
        error->set(Status::PERMISSION_DENIED, std::string("permission denied: ") + path);
      } else {
        error->set(Status::IO_ERROR, std::string("cannot open file: ") + path);
      }
    }
    return Status::IO_ERROR;
  }
  opened_ = true;

  // Drop a UTF-8 byte order mark so that the first header name stays clean.
  if (0xEF == peek_char()) {
    get_char();
    if (0xBB == peek_char()) {
      get_char();
      if (0xBF == peek_char()) {
        get_char();
      }
    }
  }
  return Status::OK;
}

Status CsvScanner::rewind(Error *error)
{
  if (!opened_) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "csv scanner is not open");
    }
    return Status::INVALID_ARGUMENT;
  }
  if (nullptr != file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  file_ = open_file(path_, "rb");
  if (nullptr == file_) {
    if (nullptr != error) {
      error->set(Status::IO_ERROR, std::string("cannot reopen file: ") + path_);
    }
    return Status::IO_ERROR;
  }
  eof_               = false;
  header_consumed_   = false;
  current_line_      = 1;
  record_start_line_ = 1;
  buffer_.clear();
  buffer_pos_ = 0;
  if (0xEF == peek_char()) {
    get_char();
    if (0xBB == peek_char()) {
      get_char();
      if (0xBF == peek_char()) {
        get_char();
      }
    }
  }
  return Status::OK;
}

bool CsvScanner::read_field(std::string *field, bool *is_null, Error *error)
{
  field->clear();
  *is_null = false;

  if ('\0' != options_.quote && options_.quote == peek_char()) {
    get_char();
    while (true) {
      const int c = get_char();
      if (c < 0) {
        if (nullptr != error) {
          error->set(Status::PARSE_ERROR, "unterminated quoted field", record_start_line_);
        }
        return false;
      }
      if ('\0' != options_.escape && options_.escape != options_.quote
          && options_.escape == c) {
        const int next = peek_char();
        if (options_.quote == next) {
          get_char();
          field->push_back(options_.quote);
          continue;
        }
        field->push_back(static_cast<char>(c));
        continue;
      }
      if (options_.quote == c) {
        // A doubled quote inside a quoted field denotes a literal quote.
        const int next = peek_char();
        if (options_.quote == next) {
          get_char();
          field->push_back(options_.quote);
          continue;
        }
        break;
      }
      field->push_back(static_cast<char>(c));
    }
    return true;
  }

  while (true) {
    const int c = peek_char();
    if (c < 0 || ',' == c || '\n' == c || '\r' == c) {
      break;
    }
    get_char();
    field->push_back(static_cast<char>(c));
  }
  if (field->empty()) {
    // An unquoted empty field is a NULL value.
    *is_null = true;
  } else if (!options_.null_string.empty() && *field == options_.null_string) {
    *is_null = true;
  }
  return true;
}

bool CsvScanner::read_record(std::vector<std::string> *fields,
                             std::vector<char> *nulls,
                             int64_t *record_line,
                             Error *error)
{
  fields->clear();
  nulls->clear();

  // Skip completely empty lines, they do not carry a record.
  while (true) {
    const int c = peek_char();
    if (c < 0) {
      return false;
    }
    if ('\n' == c || '\r' == c) {
      get_char();
      if ('\r' == c) {
        const int next = peek_char();
        if ('\n' == next) {
          get_char();
        }
      }
      continue;
    }
    break;
  }

  record_start_line_ = current_line_;
  if (nullptr != record_line) {
    *record_line = record_start_line_;
  }

  while (true) {
    std::string field;
    bool is_null = false;
    if (!read_field(&field, &is_null, error)) {
      return false;
    }
    fields->push_back(field);
    nulls->push_back(is_null ? 1 : 0);

    const int c = peek_char();
    if (',' == c) {
      get_char();
      continue;
    }
    if ('\n' == c) {
      get_char();
      break;
    }
    if ('\r' == c) {
      get_char();
      const int next = peek_char();
      if ('\n' == next) {
        get_char();
      }
      break;
    }
    if (c < 0) {
      break;
    }
    // An unexpected character is consumed so that the scan always makes
    // progress instead of looping forever.
    get_char();
  }
  return true;
}

Status CsvScanner::infer_schema(std::vector<ColumnSchema> *columns, Error *error)
{
  if (nullptr == columns) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "null column list");
    }
    return Status::INVALID_ARGUMENT;
  }
  if (!opened_) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "csv scanner is not open");
    }
    return Status::INVALID_ARGUMENT;
  }
  if (Status::OK != rewind(error)) {
    return Status::IO_ERROR;
  }

  std::vector<std::string> fields;
  std::vector<char> nulls;
  int64_t record_line = 0;

  bool have_record = read_record(&fields, &nulls, &record_line, error);
  if (!have_record) {
    if (nullptr != error && error->ok()) {
      error->set(Status::PARSE_ERROR, "the file does not contain any record", 1);
    }
    return Status::PARSE_ERROR;
  }

  if (options_.header) {
    header_names_    = make_unique_names(fields);
    header_line_     = record_line;
    header_consumed_ = true;
    have_record      = read_record(&fields, &nulls, &record_line, error);
  } else {
    header_names_.clear();
    for (size_t i = 0; i < fields.size(); ++i) {
      header_names_.push_back("column" + std::to_string(i));
    }
    header_consumed_ = true;
  }

  const size_t column_count = header_names_.size();
  std::vector<TypeInferrer> inferrers(column_count);

  while (have_record) {
    if (fields.size() != column_count) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR,
                   "expect " + std::to_string(column_count) + " columns but got "
                     + std::to_string(fields.size()),
                   record_line);
      }
      return Status::PARSE_ERROR;
    }
    for (size_t i = 0; i < column_count; ++i) {
      inferrers[i].observe(fields[i], 0 != nulls[i]);
    }
    ++row_count_;
    have_record = read_record(&fields, &nulls, &record_line, error);
  }
  if (nullptr != error && !error->ok()) {
    return error->status;
  }

  columns->clear();
  for (size_t i = 0; i < column_count; ++i) {
    ColumnSchema schema;
    schema.name     = header_names_[i];
    schema.type     = inferrers[i].type();
    schema.nullable = inferrers[i].has_null();
    columns->push_back(schema);
  }
  // Leave the scanner at the beginning of the file so that the caller can
  // start producing rows right away.
  return rewind(error);
}

bool CsvScanner::next_row(std::vector<std::string> *fields,
                          std::vector<char> *nulls,
                          Error *error)
{
  if (nullptr == fields || nullptr == nulls) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "null output buffer");
    }
    return false;
  }
  if (!opened_) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "csv scanner is not open");
    }
    return false;
  }
  if (!header_consumed_) {
    std::vector<std::string> header_fields;
    std::vector<char> header_nulls;
    int64_t record_line = 0;
    if (!read_record(&header_fields, &header_nulls, &record_line, error)) {
      return false;
    }
    if (options_.header) {
      header_names_ = make_unique_names(header_fields);
    } else {
      header_names_.clear();
      for (size_t i = 0; i < header_fields.size(); ++i) {
        header_names_.push_back("column" + std::to_string(i));
      }
    }
    header_consumed_ = true;
    if (options_.header) {
      return read_record(fields, nulls, nullptr, error);
    }
    fields->swap(header_fields);
    nulls->swap(header_nulls);
    return true;
  }

  int64_t record_line = 0;
  if (!read_record(fields, nulls, &record_line, error)) {
    return false;
  }
  if (options_.header && fields->size() != header_names_.size()) {
    if (nullptr != error) {
      error->set(Status::PARSE_ERROR,
                 "expect " + std::to_string(header_names_.size()) + " columns but got "
                   + std::to_string(fields->size()),
                 record_line);
    }
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// JsonlScanner
// ---------------------------------------------------------------------------

JsonlScanner::~JsonlScanner()
{
  close();
}

void JsonlScanner::close()
{
  if (nullptr != file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  opened_  = false;
  eof_     = false;
  line_no_ = 0;
}

Status JsonlScanner::open(const std::string &path, const ReadOptions &options, Error *error)
{
  close();
  path_    = path;
  options_ = options;
  eof_     = false;
  line_no_ = 0;
  header_names_.clear();

  file_ = open_file(path, "rb");
  if (nullptr == file_) {
    if (nullptr != error) {
      if (ENOENT == errno) {
        error->set(Status::NOT_FOUND, std::string("file not found: ") + path);
      } else if (EACCES == errno) {
        error->set(Status::PERMISSION_DENIED, std::string("permission denied: ") + path);
      } else {
        error->set(Status::IO_ERROR, std::string("cannot open file: ") + path);
      }
    }
    return Status::IO_ERROR;
  }
  opened_ = true;
  return Status::OK;
}

Status JsonlScanner::rewind(Error *error)
{
  if (!opened_) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "jsonl scanner is not open");
    }
    return Status::INVALID_ARGUMENT;
  }
  if (nullptr != file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  file_ = open_file(path_, "rb");
  if (nullptr == file_) {
    if (nullptr != error) {
      error->set(Status::IO_ERROR, std::string("cannot reopen file: ") + path_);
    }
    return Status::IO_ERROR;
  }
  eof_     = false;
  line_no_ = 0;
  return Status::OK;
}

bool JsonlScanner::read_line(std::string *line, int64_t *line_no, Error *error)
{
  while (true) {
    line->clear();
    bool got_any = false;
    while (true) {
      if (eof_) {
        break;
      }
      const int c = std::fgetc(file_);
      if (EOF == c) {
        eof_ = true;
        break;
      }
      got_any = true;
      if ('\n' == c) {
        break;
      }
      if ('\r' == c) {
        const int next = std::fgetc(file_);
        if ('\n' != next && EOF != next) {
          std::ungetc(next, file_);
        }
        break;
      }
      line->push_back(static_cast<char>(c));
    }
    ++line_no_;
    if (nullptr != line_no) {
      *line_no = line_no_;
    }
    if (!line->empty()) {
      return true;
    }
    if (eof_ && !got_any) {
      return false;
    }
    if (eof_) {
      // A trailing empty line does not carry a record.
      return false;
    }
    // Blank lines are ignored.
  }
}

bool JsonlScanner::parse_record(const std::string &line,
                                 int64_t line_no,
                                 std::vector<std::string> *keys,
                                 std::vector<std::string> *values,
                                 std::vector<char> *is_null,
                                 Error *error)
{
  keys->clear();
  values->clear();
  is_null->clear();

  std::string message;
  size_t pos = 0;

  auto skip_ws = [&pos, &line]() {
    while (pos < line.size() && (' ' == line[pos] || '\t' == line[pos])) {
      ++pos;
    }
  };

  auto parse_string = [&pos, &line, &message](std::string *out) -> bool {
    if (pos >= line.size() || '"' != line[pos]) {
      message = "expected a string";
      return false;
    }
    ++pos;
    out->clear();
    while (pos < line.size()) {
      const char c = line[pos];
      if ('"' == c) {
        ++pos;
        return true;
      }
      if ('\\' == c) {
        ++pos;
        if (pos >= line.size()) {
          break;
        }
        const char esc = line[pos++];
        switch (esc) {
        case '"':  out->push_back('"');  break;
        case '\\': out->push_back('\\'); break;
        case '/':  out->push_back('/');  break;
        case 'b':  out->push_back('\b'); break;
        case 'f':  out->push_back('\f'); break;
        case 'n':  out->push_back('\n'); break;
        case 'r':  out->push_back('\r'); break;
        case 't':  out->push_back('\t'); break;
        case 'u': {
          if (pos + 4 > line.size()) {
            message = "truncated unicode escape";
            return false;
          }
          unsigned int code = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = line[pos + i];
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code += static_cast<unsigned int>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code += static_cast<unsigned int>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code += static_cast<unsigned int>(h - 'A' + 10);
            } else {
              message = "invalid unicode escape";
              return false;
            }
          }
          pos += 4;
          if (code < 0x80) {
            out->push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (code >> 6)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out->push_back(static_cast<char>(0xE0 | (code >> 12)));
            out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default:
          message = "invalid escape sequence";
          return false;
        }
        continue;
      }
      out->push_back(c);
      ++pos;
    }
    message = "unterminated string";
    return false;
  };

  skip_ws();
  if (pos >= line.size() || '{' != line[pos]) {
    if (nullptr != error) {
      error->set(Status::PARSE_ERROR, "a JSONL record must be an object", line_no);
    }
    return false;
  }
  ++pos;
  skip_ws();
  if (pos < line.size() && '}' == line[pos]) {
    ++pos;
    skip_ws();
    if (pos != line.size()) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR, "unexpected trailing content", line_no);
      }
      return false;
    }
    return true;
  }

  while (true) {
    skip_ws();
    std::string key;
    if (!parse_string(&key)) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR, message, line_no);
      }
      return false;
    }
    skip_ws();
    if (pos >= line.size() || ':' != line[pos]) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR, "expected ':' after the field name", line_no);
      }
      return false;
    }
    ++pos;
    skip_ws();
    if (pos >= line.size()) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR, "unexpected end of the record", line_no);
      }
      return false;
    }
    const char c = line[pos];
    std::string value;
    bool null_value = false;
    if ('"' == c) {
      if (!parse_string(&value)) {
        if (nullptr != error) {
          error->set(Status::PARSE_ERROR, message, line_no);
        }
        return false;
      }
    } else if ('{' == c) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR, "nested objects are not supported", line_no);
      }
      return false;
    } else if ('[' == c) {
      if (nullptr != error) {
        error->set(Status::PARSE_ERROR, "nested arrays are not supported", line_no);
      }
      return false;
    } else if ('t' == c || 'f' == c) {
      const size_t start = pos;
      while (pos < line.size() && ((line[pos] >= 'a' && line[pos] <= 'z')
                                   || (line[pos] >= 'A' && line[pos] <= 'Z'))) {
        ++pos;
      }
      value.assign(line, start, pos - start);
      if ("true" != value && "false" != value) {
        if (nullptr != error) {
          error->set(Status::PARSE_ERROR, "invalid literal '" + value + "'", line_no);
        }
        return false;
      }
    } else if ('n' == c) {
      const size_t start = pos;
      while (pos < line.size() && ((line[pos] >= 'a' && line[pos] <= 'z')
                                   || (line[pos] >= 'A' && line[pos] <= 'Z'))) {
        ++pos;
      }
      value.assign(line, start, pos - start);
      if ("null" != value) {
        if (nullptr != error) {
          error->set(Status::PARSE_ERROR, "invalid literal '" + value + "'", line_no);
        }
        return false;
      }
      null_value = true;
    } else {
      const size_t start = pos;
      if ('-' == line[pos] || '+' == line[pos]) {
        ++pos;
      }
      bool has_digit = false;
      while (pos < line.size()) {
        const char n = line[pos];
        if (n >= '0' && n <= '9') {
          has_digit = true;
          ++pos;
          continue;
        }
        if ('.' == n || 'e' == n || 'E' == n || '+' == n || '-' == n) {
          ++pos;
          continue;
        }
        break;
      }
      if (!has_digit) {
        if (nullptr != error) {
          error->set(Status::PARSE_ERROR, "unexpected character in the record", line_no);
        }
        return false;
      }
      value.assign(line, start, pos - start);
    }

    keys->push_back(key);
    values->push_back(value);
    is_null->push_back(null_value ? 1 : 0);

    skip_ws();
    if (pos < line.size() && ',' == line[pos]) {
      ++pos;
      continue;
    }
    if (pos < line.size() && '}' == line[pos]) {
      ++pos;
      break;
    }
    if (nullptr != error) {
      error->set(Status::PARSE_ERROR, "expected ',' or '}' in the record", line_no);
    }
    return false;
  }

  skip_ws();
  if (pos != line.size()) {
    if (nullptr != error) {
      error->set(Status::PARSE_ERROR, "unexpected trailing content", line_no);
    }
    return false;
  }
  return true;
}

Status JsonlScanner::infer_schema(std::vector<ColumnSchema> *columns, Error *error)
{
  if (nullptr == columns) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "null column list");
    }
    return Status::INVALID_ARGUMENT;
  }
  if (!opened_) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "jsonl scanner is not open");
    }
    return Status::INVALID_ARGUMENT;
  }
  if (Status::OK != rewind(error)) {
    return Status::IO_ERROR;
  }

  std::vector<std::string> keys;
  std::vector<std::string> values;
  std::vector<char> is_null;
  std::string line;
  int64_t line_no = 0;
  bool have_line  = read_line(&line, &line_no, error);

  std::vector<std::string> field_order;
  std::vector<TypeInferrer> inferrers;
  bool have_record = false;

  while (have_line) {
    if (!parse_record(line, line_no, &keys, &values, &is_null, error)) {
      return Status::PARSE_ERROR;
    }
    have_record = true;
    ++row_count_;
    for (size_t i = 0; i < keys.size(); ++i) {
      size_t index = field_order.size();
      for (size_t k = 0; k < field_order.size(); ++k) {
        if (field_order[k] == keys[i]) {
          index = k;
          break;
        }
      }
      if (index == field_order.size()) {
        field_order.push_back(keys[i]);
        inferrers.push_back(TypeInferrer());
      }
      inferrers[index].observe(values[i], 0 != is_null[i]);
    }
    have_line = read_line(&line, &line_no, error);
  }
  if (nullptr != error && !error->ok()) {
    return error->status;
  }
  if (!have_record) {
    if (nullptr != error) {
      error->set(Status::PARSE_ERROR, "the file does not contain any record", 1);
    }
    return Status::PARSE_ERROR;
  }

  header_names_ = field_order;
  columns->clear();
  for (size_t i = 0; i < field_order.size(); ++i) {
    ColumnSchema schema;
    schema.name     = field_order[i];
    schema.type     = inferrers[i].type();
    schema.nullable = inferrers[i].has_null();
    columns->push_back(schema);
  }
  // Leave the scanner at the beginning of the file so that the caller can
  // start producing rows right away.
  return rewind(error);
}

bool JsonlScanner::next_row(std::vector<std::string> *fields,
                            std::vector<char> *nulls,
                            Error *error)
{
  if (nullptr == fields || nullptr == nulls) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "null output buffer");
    }
    return false;
  }
  if (!opened_) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "jsonl scanner is not open");
    }
    return false;
  }
  std::string line;
  int64_t line_no = 0;
  if (!read_line(&line, &line_no, error)) {
    return false;
  }
  std::vector<std::string> keys;
  std::vector<std::string> values;
  std::vector<char> is_null;
  if (!parse_record(line, line_no, &keys, &values, &is_null, error)) {
    return false;
  }

  fields->clear();
  nulls->clear();
  for (size_t i = 0; i < header_names_.size(); ++i) {
    bool found = false;
    for (size_t k = 0; k < keys.size(); ++k) {
      if (keys[k] == header_names_[i]) {
        fields->push_back(values[k]);
        nulls->push_back(is_null[k]);
        found = true;
        break;
      }
    }
    if (!found) {
      // A field that is missing from this record is a NULL value.
      fields->push_back(std::string());
      nulls->push_back(1);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// glob and file stamps
// ---------------------------------------------------------------------------

Status glob_files(const std::string &pattern, std::vector<std::string> *files, Error *error)
{
  if (nullptr == files) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "null file list");
    }
    return Status::INVALID_ARGUMENT;
  }
  files->clear();
  if (pattern.empty()) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "the glob pattern is empty");
    }
    return Status::INVALID_ARGUMENT;
  }

  const size_t slash = pattern.find_last_of('/');
  std::string dir_pattern;
  std::string name_pattern;
  if (std::string::npos == slash) {
    dir_pattern  = ".";
    name_pattern = pattern;
  } else {
    dir_pattern  = pattern.substr(0, slash);
    name_pattern = pattern.substr(slash + 1);
    if (dir_pattern.empty()) {
      dir_pattern = "/";
    }
  }

  std::vector<std::string> dirs;
  expand_directory_pattern(dir_pattern, &dirs);
  for (size_t i = 0; i < dirs.size(); ++i) {
    std::vector<std::string> entries;
    if (!list_directory(dirs[i], &entries)) {
      continue;
    }
    for (size_t e = 0; e < entries.size(); ++e) {
      if (!match_component(name_pattern.c_str(), entries[e].c_str())) {
        continue;
      }
      const std::string full = join_path(dirs[i], entries[e]);
      if (is_regular_file(full)) {
        files->push_back(full);
      }
    }
  }
  std::sort(files->begin(), files->end());
  files->erase(std::unique(files->begin(), files->end()), files->end());
  return Status::OK;
}

Status take_file_stamp(const std::string &path_arg, FileStamp *stamp, Error *error)
{
  if (nullptr == stamp) {
    if (nullptr != error) {
      error->set(Status::INVALID_ARGUMENT, "null stamp");
    }
    return Status::INVALID_ARGUMENT;
  }
#if defined(_WIN32)
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::u8path(path_arg);
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    if (nullptr != error) {
      error->set(Status::NOT_FOUND, std::string("file not found: ") + path_arg);
    }
    return Status::NOT_FOUND;
  }
  const auto write_time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    if (nullptr != error) {
      error->set(Status::NOT_FOUND, std::string("file not found: ") + path_arg);
    }
    return Status::NOT_FOUND;
  }
  stamp->size     = static_cast<int64_t>(size);
  stamp->mtime_ns = static_cast<int64_t>(write_time.time_since_epoch().count());
#else
  struct stat info;
  if (0 != stat(path_arg.c_str(), &info)) {
    if (nullptr != error) {
      error->set(Status::NOT_FOUND, std::string("file not found: ") + path_arg);
    }
    return Status::NOT_FOUND;
  }
  stamp->size = static_cast<int64_t>(info.st_size);
  stamp->mtime_ns = static_cast<int64_t>(info.st_mtime) * 1000000000LL
                    + static_cast<int64_t>(info.st_mtim.tv_nsec);
#endif
  return Status::OK;
}

bool file_stamp_equal(const FileStamp &lhs, const FileStamp &rhs)
{
  return lhs.size == rhs.size && lhs.mtime_ns == rhs.mtime_ns;
}

} // namespace file_scan
} // namespace sql
} // namespace oceanbase
