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
 * @file ob_file_copy_writer.h
 *
 * The CSV writer of the COPY statement.
 *
 * The writer is deliberately self contained: it does not depend on the
 * OceanBase runtime, it only uses the C++17 standard library and the POSIX
 * file interfaces.  That way the quoting rules, the representation of the
 * NULL values and the cleanup of a half written file can be exercised
 * directly by unit tests.
 *
 * The produced file is meant to be read back by the read_csv() table
 * function of the same feature.  The reader treats an unquoted empty field
 * as a NULL value, therefore the writer emits an unquoted empty field for a
 * NULL value by default.  A NULL value is never enclosed in the quotes, so
 * that the reader can distinguish it from the empty string.
 *
 * The writer streams the rows: only the row that is currently written is
 * kept in memory, so a result of an arbitrary size can be exported.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_COPY_WRITER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_COPY_WRITER_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
namespace file_scan
{

/**
 * The options of the COPY statement that influence the shape of the
 * produced file.
 */
struct CopyOptions
{
  CopyOptions()
      : delimiter_(','),
        quote_('"'),
        escape_('"'),
        has_header_(true),
        null_string_()
  {
  }

  /// The character that separates two fields of a row.
  char delimiter_;
  /// The character that encloses a field, zero disables the enclosing.
  char quote_;
  /// The character that escapes the quote inside a field, zero disables the escaping.
  char escape_;
  /// True when the first line of the file holds the names of the columns.
  bool has_header_;
  /**
   * The text that represents a NULL value.  The default is the empty text,
   * which is written as an unquoted empty field.  The reader of the file
   * interprets an unquoted empty field as a NULL value, so the default keeps
   * the round trip of a NULL value intact.
   */
  std::string null_string_;
};

/**
 * A streaming CSV writer.
 *
 * The writer creates the target file, refuses to overwrite an existing file
 * and removes the file again when the export fails half way, so that a
 * failed export never leaves a truncated file behind.
 */
class CopyWriter
{
public:
  CopyWriter() = default;
  ~CopyWriter();

  CopyWriter(const CopyWriter &) = delete;
  CopyWriter &operator=(const CopyWriter &) = delete;

  /**
   * Create the target file.
   *
   * Fails with Status::IO_ERROR when the file already exists, the caller is
   * expected to report the failure to the user.
   */
  Status open(const std::string &path, const CopyOptions &options, Error *error);

  /// Write the names of the columns as the first line of the file.
  Status write_header(const std::vector<std::string> &column_names, Error *error);

  /**
   * Write one row.  A field of the row is NULL when the matching entry of
   * `nulls` is not zero.  A NULL field is written as an unquoted empty
   * field, or as the configured NULL text when there is one.
   */
  Status write_row(const std::vector<std::string> &fields,
                   const std::vector<char> &nulls,
                   Error *error);

  /// Flush the buffer and close the file.  A failure removes the file.
  Status close(Error *error);

  /**
   * Close the file and remove it.  The method is called when the export
   * failed, so that a half written file does not survive the failure.
   */
  void abort();

  const std::string &path() const { return path_; }
  const CopyOptions &options() const { return options_; }
  /// The number of rows that were written so far.
  int64_t row_count() const { return row_count_; }

  /**
   * Quote a single field and append it to the given buffer.
   *
   * The field is enclosed in the quotes when it holds the delimiter, the
   * quote, a line break or a carriage return.  The quote inside the field is
   * escaped by doubling it, or by the escape character when one is
   * configured.
   */
  static void append_field(std::string *buffer,
                           const std::string &field,
                           const CopyOptions &options);

private:
  Status flush_buffer(Error *error);
  Status write_line(const std::string &line, Error *error);

  std::string path_;
  CopyOptions options_;
  std::string buffer_;
  std::FILE *file_ = nullptr;
  bool opened_ = false;
  bool header_written_ = false;
  int64_t row_count_ = 0;
};

} // namespace file_scan
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_COPY_WRITER_H_
