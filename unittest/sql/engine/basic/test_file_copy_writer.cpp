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
 * @file test_file_copy_writer.cpp
 *
 * Unit tests of the CSV writer of the COPY statement.
 *
 * The tests cover the shape of the produced file, the quoting of the fields
 * that hold a delimiter or a quote, the representation of a NULL value, the
 * refusal to overwrite an existing file and the removal of a half written
 * file when the export fails.
 *
 * The last test reads the produced file back with the reader of the
 * read_csv() table function, so that the round trip of a NULL value is
 * covered as well.
 */

#include <gtest/gtest.h>
#include "sql/engine/basic/ob_file_copy_writer.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace oceanbase;
using namespace oceanbase::sql::file_scan;

namespace oceanbase
{
namespace sql
{
namespace
{

/// The content of a file, an empty string when the file cannot be read.
std::string read_all(const std::string &path)
{
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (NULL == file) {
    return std::string();
  }
  std::string out;
  char buffer[4096];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.append(buffer, count);
  }
  std::fclose(file);
  return out;
}

/// True when the file exists.
bool file_present(const std::string &path)
{
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (NULL == file) {
    return false;
  }
  std::fclose(file);
  return true;
}

/// A temporary directory that holds the files of a single test.
class CopyWriterTest : public ::testing::Test
{
public:
  void SetUp() override
  {
    dir_ = std::string("file_copy_writer_test_")
           + ::testing::UnitTest::GetInstance()->current_test_info()->name();
    mkdir_p(dir_);
  }

  void TearDown() override
  {
    const std::string path = dir_ + "/";
    // the files of the test are removed one by one
    for (int i = 0; i < 8; ++i) {
      std::remove((path + "f" + std::to_string(i) + ".csv").c_str());
    }
  }

  std::string path(const char *name) const { return dir_ + "/" + name; }

private:
  static void mkdir_p(const std::string &path)
  {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
      current.push_back(path[i]);
      if ('/' == path[i]) {
        mkdir(current.c_str(), 0755);
      }
    }
    mkdir(path.c_str(), 0755);
  }

  std::string dir_;
};

TEST_F(CopyWriterTest, a_row_and_a_header_are_written)
{
  const std::string file = path("f0.csv");
  CopyOptions options;
  options.has_header_ = true;
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, options, &error));
  ASSERT_EQ(Status::OK, writer.write_header({"id", "name"}, &error));

  const std::vector<std::string> first = {"1", "alice"};
  const std::vector<char> first_nulls = {0, 0};
  ASSERT_EQ(Status::OK, writer.write_row(first, first_nulls, &error));

  // the name of the second row is a NULL value
  const std::vector<std::string> second = {"2", "bob"};
  const std::vector<char> second_nulls = {0, 1};
  ASSERT_EQ(Status::OK, writer.write_row(second, second_nulls, &error));
  ASSERT_EQ(Status::OK, writer.close(&error));

  EXPECT_EQ("id,name\n1,alice\n2,\n", read_all(file));
  EXPECT_EQ(2, writer.row_count());
}

TEST_F(CopyWriterTest, an_existing_file_is_not_overwritten)
{
  const std::string file = path("f1.csv");
  std::FILE *created = std::fopen(file.c_str(), "wb");
  ASSERT_NE(nullptr, created);
  std::fputs("x", created);
  std::fclose(created);

  CopyOptions options;
  Error error;
  CopyWriter writer;
  EXPECT_NE(Status::OK, writer.open(file, options, &error));
  EXPECT_EQ(Status::IO_ERROR, error.status);
  // the pre-existing file must not be touched
  EXPECT_EQ("x", read_all(file));
}

TEST_F(CopyWriterTest, a_field_that_holds_a_delimiter_or_a_quote_is_enclosed)
{
  const std::string file = path("f2.csv");
  CopyOptions options;
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, options, &error));

  const std::vector<std::string> fields = {"a,b", "say \"hi\"", "line\nbreak"};
  const std::vector<char> nulls = {0, 0, 0};
  ASSERT_EQ(Status::OK, writer.write_row(fields, nulls, &error));
  ASSERT_EQ(Status::OK, writer.close(&error));

  EXPECT_EQ("\"a,b\",\"say \"\"hi\"\"\",\"line\nbreak\"\n", read_all(file));
}

TEST_F(CopyWriterTest, a_null_value_is_written_as_the_configured_text)
{
  const std::string file = path("f3.csv");
  CopyOptions options;
  options.null_string_ = "NULL";
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, options, &error));

  const std::vector<std::string> fields = {"1", ""};
  const std::vector<char> nulls = {0, 1};
  ASSERT_EQ(Status::OK, writer.write_row(fields, nulls, &error));
  ASSERT_EQ(Status::OK, writer.close(&error));

  EXPECT_EQ("1,NULL\n", read_all(file));
}

TEST_F(CopyWriterTest, an_aborted_export_removes_the_file)
{
  const std::string file = path("f4.csv");
  CopyOptions options;
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, options, &error));

  const std::vector<std::string> fields = {"1"};
  const std::vector<char> nulls = {0};
  ASSERT_EQ(Status::OK, writer.write_row(fields, nulls, &error));
  writer.abort();

  EXPECT_FALSE(file_present(file));
}

TEST_F(CopyWriterTest, a_custom_delimiter_and_quote_are_used)
{
  const std::string file = path("f5.csv");
  CopyOptions options;
  options.delimiter_ = '\t';
  options.quote_    = '\'';
  options.escape_   = '\'';
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, options, &error));

  const std::vector<std::string> fields = {"a", "b'c"};
  const std::vector<char> nulls = {0, 0};
  ASSERT_EQ(Status::OK, writer.write_row(fields, nulls, &error));
  ASSERT_EQ(Status::OK, writer.close(&error));

  EXPECT_EQ("a\t'b''c'\n", read_all(file));
}

TEST_F(CopyWriterTest, an_empty_string_is_not_a_null_value)
{
  const std::string file = path("f6.csv");
  CopyOptions options;
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, options, &error));

  const std::vector<std::string> fields = {"", "x"};
  const std::vector<char> nulls = {0, 0};
  ASSERT_EQ(Status::OK, writer.write_row(fields, nulls, &error));
  ASSERT_EQ(Status::OK, writer.close(&error));

  // an empty string is written as an empty field, it is not the NULL value
  EXPECT_EQ(",x\n", read_all(file));
}

TEST_F(CopyWriterTest, the_produced_file_is_read_back_by_the_csv_reader)
{
  const std::string file = path("f7.csv");
  CopyOptions copy_options;
  Error error;
  CopyWriter writer;
  ASSERT_EQ(Status::OK, writer.open(file, copy_options, &error));
  ASSERT_EQ(Status::OK, writer.write_header({"id", "name", "score"}, &error));

  const std::vector<std::string> first = {"1", "alice", "9.5"};
  const std::vector<char> first_nulls = {0, 0, 0};
  ASSERT_EQ(Status::OK, writer.write_row(first, first_nulls, &error));

  // the name and the score of the second row are NULL values
  const std::vector<std::string> second = {"2", "bob", ""};
  const std::vector<char> second_nulls = {0, 1, 1};
  ASSERT_EQ(Status::OK, writer.write_row(second, second_nulls, &error));

  // the third row holds a value that has to be quoted
  const std::vector<std::string> third = {"3", "a,b", "1"};
  const std::vector<char> third_nulls = {0, 0, 0};
  ASSERT_EQ(Status::OK, writer.write_row(third, third_nulls, &error));
  ASSERT_EQ(Status::OK, writer.close(&error));

  // read the file back with the reader of the read_csv() table function
  ReadOptions read_options;
  read_options.format = Format::CSV;
  read_options.header = true;
  CsvScanner scanner;
  ASSERT_EQ(Status::OK, scanner.open(file, read_options, &error));

  std::vector<ColumnSchema> schema;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&schema, &error));
  ASSERT_EQ(3U, schema.size());
  EXPECT_EQ("id", schema[0].name);
  EXPECT_EQ("name", schema[1].name);
  EXPECT_EQ("score", schema[2].name);

  std::vector<std::string> fields;
  std::vector<char> nulls;

  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error));
  ASSERT_EQ(3U, fields.size());
  EXPECT_EQ("1", fields[0]);
  EXPECT_FALSE(nulls[0]);
  EXPECT_EQ("alice", fields[1]);
  EXPECT_FALSE(nulls[1]);
  EXPECT_EQ("9.5", fields[2]);
  EXPECT_FALSE(nulls[2]);

  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error));
  ASSERT_EQ(3U, fields.size());
  EXPECT_EQ("2", fields[0]);
  EXPECT_FALSE(nulls[0]);
  // a NULL value survives the round trip
  EXPECT_TRUE(nulls[1]);
  EXPECT_TRUE(nulls[2]);

  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error));
  ASSERT_EQ(3U, fields.size());
  EXPECT_EQ("3", fields[0]);
  EXPECT_FALSE(nulls[0]);
  EXPECT_EQ("a,b", fields[1]);
  EXPECT_FALSE(nulls[1]);
  EXPECT_EQ("1", fields[2]);
  EXPECT_FALSE(nulls[2]);

  EXPECT_FALSE(scanner.next_row(&fields, &nulls, &error));
  scanner.close();
}

} // namespace
} // namespace sql
} // namespace oceanbase
