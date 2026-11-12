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
 * @file ob_file_scan_utils.h
 *
 * Core scanning helpers backing the "file as database" table functions
 * (read_csv / read_json / read_ndjson / read_parquet / glob).
 *
 * The helpers in this header are intentionally free of any OceanBase runtime
 * dependency so that the parsing rules (CSV quoting, JSONL shape validation,
 * deterministic type inference, glob expansion, file change detection) can be
 * exercised directly by unit tests and reasoned about in isolation.  The
 * OceanBase facing glue lives in ob_file_table_op.cpp and only converts the
 * neutral values produced here into OceanBase datums.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_scan
{

/**
 * The seven logical column types the file readers are allowed to infer.
 *
 *   NULL_TYPE : every sampled value was NULL
 *   BOOLEAN   : true / false
 *   BIGINT    : signed 64 bit integer
 *   DOUBLE    : floating point number
 *   DATE      : calendar date, rendered as YYYY-MM-DD
 *   DATETIME  : date plus time of day, rendered as YYYY-MM-DD HH:MM:SS
 *   VARCHAR   : everything else
 */
enum class ColumnType : int8_t
{
  NULL_TYPE = 0,
  BOOLEAN   = 1,
  BIGINT    = 2,
  DOUBLE    = 3,
  DATE      = 4,
  DATETIME  = 5,
  VARCHAR   = 6,
};

/// Human readable name of a column type, matching the names reported by
/// DESCRIBE (BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME, NULL).
const char *to_string(ColumnType type);

/// The file formats understood by the table functions.
enum class Format : int8_t
{
  AUTO    = 0, ///< deduced from the file extension
  CSV     = 1,
  JSONL   = 2, ///< newline delimited JSON
  PARQUET = 3,
};

/// Parse a user supplied format name; returns false when the name is unknown.
bool parse_format(const std::string &name, Format *format);

/// Options shared by the file readers.
struct ReadOptions
{
  Format format         = Format::AUTO;
  bool header           = true;   ///< first CSV record holds the column names
  int64_t sample_size   = -1;     ///< only -1 (full scan) is supported
  char delimiter        = ',';
  char quote            = '"';
  char escape           = '"';
  std::string null_string;        ///< text that is interpreted as NULL
};

/// The inferred shape of one column.
struct ColumnSchema
{
  std::string name;
  ColumnType type     = ColumnType::NULL_TYPE;
  bool nullable       = false;
};

/// Result of an operation.  `line` carries the 1 based record number when the
/// failure can be attributed to a specific record.
enum class Status : int32_t
{
  OK               = 0,
  INVALID_ARGUMENT = 1,
  NOT_FOUND        = 2,
  PERMISSION_DENIED = 3,
  IO_ERROR         = 4,
  PARSE_ERROR      = 5,
  UNSUPPORTED      = 6,
  FILE_CHANGED     = 7,
};

struct Error
{
  Status status         = Status::OK;
  std::string message;
  int64_t line          = -1;

  bool ok() const { return Status::OK == status; }
  void clear() { status = Status::OK; message.clear(); line = -1; }
  void set(Status s, const std::string &msg, int64_t l = -1)
  {
    status = s;
    message = msg;
    line = l;
  }
};

/**
 * Deterministic type inference.
 *
 * The inference is a widening lattice:
 *   BOOLEAN < BIGINT < DOUBLE < VARCHAR
 *   DATE    < DATETIME < VARCHAR
 * Mixing the numeric chain with the temporal chain widens to VARCHAR, so the
 * result never depends on the order in which the values are observed.
 */
class TypeInferrer
{
public:
  TypeInferrer() = default;

  /// Observe a textual value.  `is_null` marks an absent value, which never
  /// changes the inferred type but does mark the column as nullable.
  void observe(const std::string &value, bool is_null);

  ColumnType type() const { return type_; }
  bool has_null() const { return has_null_; }
  void reset()
  {
    type_ = ColumnType::NULL_TYPE;
    has_null_ = false;
  }

private:
  ColumnType type_    = ColumnType::NULL_TYPE;
  bool has_null_      = false;
};

/// Combine two already inferred types following the widening lattice.
ColumnType widen_type(ColumnType lhs, ColumnType rhs);

/// True when the text is a well formed calendar date (YYYY-MM-DD).
bool looks_like_date(const std::string &value);

/// True when the text is a well formed timestamp (YYYY-MM-DD[T ]HH:MM:SS).
bool looks_like_datetime(const std::string &value);

/// True when the text is a well formed signed 64 bit integer.
bool looks_like_bigint(const std::string &value);

/// True when the text is a well formed floating point number.
bool looks_like_double(const std::string &value);

/// True when the text spells a boolean literal (true / false).
bool looks_like_boolean(const std::string &value);

/// Make the column names of a header unique.  The n-th repetition of a name
/// (n starting at two) is suffixed with "__n", so `id,id` becomes `id,id__2`.
std::vector<std::string> make_unique_names(const std::vector<std::string> &names);

/**
 * Streaming CSV reader.
 *
 * The reader keeps a single record in memory at a time, so the file is never
 * materialised.  Schema inference performs a full pass over the file and the
 * scan itself performs a second pass, which keeps the inference independent
 * from the number of rows that are finally consumed.
 */
class CsvScanner
{
public:
  CsvScanner() = default;
  ~CsvScanner();

  CsvScanner(const CsvScanner &) = delete;
  CsvScanner &operator=(const CsvScanner &) = delete;

  /// Open the file and remember the header names.
  Status open(const std::string &path, const ReadOptions &options, Error *error);

  /// Scan the whole file and derive the column schema.
  Status infer_schema(std::vector<ColumnSchema> *columns, Error *error);

  /// Produce the next record.  Returns false when the file is exhausted.
  bool next_row(std::vector<std::string> *fields,
                std::vector<char> *nulls,
                Error *error);

  /// Restart the scan from the first record.
  Status rewind(Error *error);

  void close();

  const std::vector<std::string> &header_names() const { return header_names_; }
  const ReadOptions &options() const { return options_; }
  const std::string &path() const { return path_; }

private:
  bool read_record(std::vector<std::string> *fields,
                   std::vector<char> *nulls,
                   int64_t *record_line,
                   Error *error);
  bool read_field(std::string *field, bool *is_null, Error *error);
  bool fill_buffer();
  int peek_char();
  int get_char();

  std::string path_;
  ReadOptions options_;
  std::vector<std::string> header_names_;
  int64_t header_line_          = 0;
  bool header_consumed_         = false;
  bool eof_                     = false;
  bool opened_                  = false;
  int64_t current_line_         = 1;
  int64_t record_start_line_    = 1;
  std::string buffer_;
  size_t buffer_pos_            = 0;
  std::FILE *file_              = nullptr;
};

/**
 * Streaming newline delimited JSON reader.
 *
 * Only flat objects are accepted.  A nested object or array is reported as a
 * parse error together with the record number, which keeps the failure
 * diagnosable instead of silently producing wrong values.
 */
class JsonlScanner
{
public:
  JsonlScanner() = default;
  ~JsonlScanner();

  JsonlScanner(const JsonlScanner &) = delete;
  JsonlScanner &operator=(const JsonlScanner &) = delete;

  Status open(const std::string &path, const ReadOptions &options, Error *error);

  /// Scan the whole file and derive the union of all observed fields.
  Status infer_schema(std::vector<ColumnSchema> *columns, Error *error);

  /// Produce the next record.  Returns false when the file is exhausted.
  bool next_row(std::vector<std::string> *fields,
                std::vector<char> *nulls,
                Error *error);

  Status rewind(Error *error);

  void close();

  const std::vector<std::string> &header_names() const { return header_names_; }
  const ReadOptions &options() const { return options_; }
  const std::string &path() const { return path_; }

private:
  bool read_line(std::string *line, int64_t *line_no, Error *error);
  bool parse_record(const std::string &line,
                    int64_t line_no,
                    std::vector<std::string> *keys,
                    std::vector<std::string> *values,
                    std::vector<char> *is_null,
                    Error *error);

  std::string path_;
  ReadOptions options_;
  std::vector<std::string> header_names_;
  bool eof_        = false;
  bool opened_     = false;
  int64_t line_no_ = 0;
  std::FILE *file_ = nullptr;
};

/**
 * Expand a shell style pattern into the list of matching regular files.
 *
 * `*` and `?` never match the path separator, `**` matches any number of
 * directories.  The result is sorted so that the output is deterministic.
 */
Status glob_files(const std::string &pattern, std::vector<std::string> *files, Error *error);

/// A cheap snapshot of a file used to detect that the file changed while a
/// scan was running.
struct FileStamp
{
  int64_t size     = -1;
  int64_t mtime_ns = -1;
};

Status take_file_stamp(const std::string &path, FileStamp *stamp, Error *error);
bool file_stamp_equal(const FileStamp &lhs, const FileStamp &rhs);

/// Remove a leading UTF-8 byte order mark, if present.
std::string strip_utf8_bom(const std::string &value);

} // namespace file_scan
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
