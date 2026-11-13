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
 * @file ob_file_copy_writer.cpp
 *
 * The implementation of the CSV writer of the COPY statement.
 *
 * Like the scanning helpers the module is free of any OceanBase runtime
 * dependency, it only uses the C++17 standard library and the POSIX file
 * interfaces.  That way the quoting rules, the representation of the NULL
 * values and the cleanup of a half written file can be exercised directly
 * by the unit tests.
 */

#include "sql/engine/basic/ob_file_copy_writer.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
# include <io.h>
# include <sys/stat.h>
#else
# include <sys/stat.h>
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
/// The size of the buffer that collects the rows before they are written.
const size_t COPY_WRITE_BUFFER_SIZE = 256 * 1024;

/// True when the text holds a character that forces the field to be quoted.
bool field_needs_quote(const std::string &field, const CopyOptions &options)
{
  for (size_t i = 0; i < field.size(); ++i) {
    const char c = field[i];
    if (c == options.delimiter_ || c == options.quote_
        || c == '\n' || c == '\r') {
      return true;
    }
  }
  return false;
}

/// True when the path names an existing file.
bool file_exists(const std::string &path)
{
  struct stat path_stat;
  return 0 == stat(path.c_str(), &path_stat);
}

} // anonymous namespace

CopyWriter::~CopyWriter()
{
  if (opened_ && NULL != file_) {
    // The destructor must never leave a half written file behind.
    std::fclose(file_);
    file_ = nullptr;
    std::remove(path_.c_str());
  }
}

Status CopyWriter::open(const std::string &path, const CopyOptions &options, Error *error)
{
  if (opened_) {
    error->set(Status::INVALID_ARGUMENT, "the copy writer is already open");
    return Status::INVALID_ARGUMENT;
  }
  path_ = path;
  options_ = options;
  buffer_.clear();
  row_count_ = 0;
  header_written_ = false;

  // The COPY statement must not overwrite an existing file.  The existence
  // of the target is checked first, the file is then created exclusively so
  // that a file that appears in between is not overwritten either.
  if (file_exists(path_)) {
    error->set(Status::IO_ERROR, "the target file of the copy statement already exists: " + path_);
    return Status::IO_ERROR;
  }
  file_ = std::fopen(path_.c_str(), "wbx");
  if (NULL == file_) {
    // The exclusive mode is not available on every platform, the file may
    // exist nevertheless.
    if (errno == EEXIST) {
      error->set(Status::IO_ERROR, "the target file of the copy statement already exists: " + path_);
      return Status::IO_ERROR;
    }
    file_ = std::fopen(path_.c_str(), "wb");
    if (NULL == file_) {
      error->set(Status::IO_ERROR, "failed to create the target file of the copy statement: " + path_);
      return Status::IO_ERROR;
    }
  }
  opened_ = true;
  return Status::OK;
}

Status CopyWriter::write_header(const std::vector<std::string> &column_names, Error *error)
{
  if (!opened_) {
    error->set(Status::INVALID_ARGUMENT, "the copy writer is not open");
    return Status::INVALID_ARGUMENT;
  }
  if (header_written_) {
    error->set(Status::INVALID_ARGUMENT, "the header of the copy statement was written already");
    return Status::INVALID_ARGUMENT;
  }
  std::string line;
  for (size_t i = 0; i < column_names.size(); ++i) {
    if (i > 0) {
      line.push_back(options_.delimiter_);
    }
    append_field(&line, column_names[i], options_);
  }
  line.push_back('\n');
  const Status status = write_line(line, error);
  if (Status::OK == status) {
    header_written_ = true;
  }
  return status;
}

Status CopyWriter::write_row(const std::vector<std::string> &fields,
                             const std::vector<char> &nulls,
                             Error *error)
{
  if (!opened_) {
    error->set(Status::INVALID_ARGUMENT, "the copy writer is not open");
    return Status::INVALID_ARGUMENT;
  }
  std::string line;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) {
      line.push_back(options_.delimiter_);
    }
    const bool is_null = (i >= nulls.size()) || (0 != nulls[i]);
    if (is_null) {
      if (options_.null_string_.empty()) {
        // An unquoted empty field, the reader of the file interprets it
        // as a NULL value.
      } else {
        // The NULL text is written as it is, the reader only interprets it
        // as a NULL value when it is configured with the same text.
        line.append(options_.null_string_);
      }
    } else {
      append_field(&line, fields[i], options_);
    }
  }
  line.push_back('\n');
  const Status status = write_line(line, error);
  if (Status::OK == status) {
    ++row_count_;
  }
  return status;
}

void CopyWriter::append_field(std::string *buffer,
                              const std::string &field,
                              const CopyOptions &options)
{
  const char quote = options.quote_;
  const bool need_quote = (0 != quote) && field_needs_quote(field, options);
  if (!need_quote) {
    buffer->append(field);
    return;
  }
  buffer->push_back(quote);
  const char escape = options.escape_;
  for (size_t i = 0; i < field.size(); ++i) {
    const char c = field[i];
    if (0 != escape && c == quote && escape != quote) {
      // The quote is escaped by the escape character.
      buffer->push_back(escape);
      buffer->push_back(quote);
    } else if (c == quote) {
      // The quote is doubled, the reader of the file understands both
      // notations.
      buffer->push_back(quote);
      buffer->push_back(quote);
    } else {
      buffer->push_back(c);
    }
  }
  buffer->push_back(quote);
}

Status CopyWriter::write_line(const std::string &line, Error *error)
{
  buffer_.append(line);
  if (buffer_.size() >= COPY_WRITE_BUFFER_SIZE) {
    return flush_buffer(error);
  }
  return Status::OK;
}

Status CopyWriter::flush_buffer(Error *error)
{
  if (!buffer_.empty()) {
    // fwrite returns the number of the written items, a short write is a
    // failure.
    const size_t written = (NULL == file_)
                             ? 0
                             : std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
    if (written != buffer_.size()) {
      error->set(Status::IO_ERROR, "failed to write to the target file of the copy statement: " + path_);
      return Status::IO_ERROR;
    }
    buffer_.clear();
  }
  return Status::OK;
}

Status CopyWriter::close(Error *error)
{
  if (!opened_) {
    return Status::OK;
  }
  Status status = flush_buffer(error);
  if (Status::OK == status) {
    if (NULL == file_ || 0 != std::fflush(file_)) {
      error->set(Status::IO_ERROR, "failed to flush the target file of the copy statement: " + path_);
      status = Status::IO_ERROR;
    }
  }
  if (NULL != file_) {
    if (0 != std::fclose(file_)) {
      if (Status::OK == status) {
        error->set(Status::IO_ERROR, "failed to close the target file of the copy statement: " + path_);
        status = Status::IO_ERROR;
      }
    }
    file_ = nullptr;
  }
  opened_ = false;
  if (Status::OK != status) {
    // A failed export must not leave a half written file behind.
    std::remove(path_.c_str());
  }
  return status;
}

void CopyWriter::abort()
{
  if (NULL != file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  if (!path_.empty()) {
    std::remove(path_.c_str());
  }
  opened_ = false;
  buffer_.clear();
}

} // namespace file_scan
} // namespace sql
} // namespace oceanbase
