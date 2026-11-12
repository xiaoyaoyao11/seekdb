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
 * @file test_file_scan_utils.cpp
 *
 * Unit tests of the scanning helpers that back the file table functions.
 *
 * The tests cover the CSV inference and the read options, the union of the
 * JSONL fields, the detection of a file that changed while it was scanned
 * and the diagnostics of malformed input.
 */

#include <gtest/gtest.h>
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

/// A temporary directory that holds the files of a single test.
class FileScanTest : public ::testing::Test
{
public:
  void SetUp() override
  {
    dir_ = std::string("file_scan_utils_test_")
           + ::testing::UnitTest::GetInstance()->current_test_info()->name();
    mkdir_p(dir_);
  }

  void TearDown() override {}

  std::string path(const char *name) const { return dir_ + "/" + name; }

  void write_file(const char *name, const std::string &content)
  {
    std::FILE *file = std::fopen(path(name).c_str(), "wb");
    if (nullptr != file) {
      std::fwrite(content.data(), 1, content.size(), file);
      std::fclose(file);
    }
  }

private:
  static void mkdir_p(const std::string &path)
  {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
      current.push_back(path[i]);
      if ('/' == path[i] && i > 0) {
        ::mkdir(current.c_str(), 0755);
      }
    }
    ::mkdir(current.c_str(), 0755);
  }

  static void remove_all()
  {
    // The files of a test live in a directory that is named after the test,
    // so a rerun simply overwrites them.
  }

  std::string dir_;
};

TEST_F(FileScanTest, infer_csv_schema)
{
  write_file("basic.csv",
             "id,name,score,active,day,created,note,id\n"
             "1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,\"hello,\n"
             "world\",101\n"
             "2,Bob,87,false,,,plain,102\n"
             "3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14,\"say \"\"hi\"\"\",103\n");

  ReadOptions options;
  options.format = Format::CSV;
  CsvScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("basic.csv"), options, &error)) << error.message;
  std::vector<ColumnSchema> columns;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&columns, &error)) << error.message;
  ASSERT_EQ(8U, columns.size());
  EXPECT_EQ("id", columns[0].name);
  EXPECT_EQ("BIGINT", std::string(to_string(columns[0].type)));
  EXPECT_FALSE(columns[0].nullable);
  EXPECT_EQ("name", columns[1].name);
  EXPECT_EQ("VARCHAR", std::string(to_string(columns[1].type)));
  EXPECT_EQ("score", columns[2].name);
  EXPECT_EQ("DOUBLE", std::string(to_string(columns[2].type)));
  EXPECT_EQ("active", columns[3].name);
  EXPECT_EQ("BOOLEAN", std::string(to_string(columns[3].type)));
  EXPECT_EQ("day", columns[4].name);
  EXPECT_EQ("DATE", std::string(to_string(columns[4].type)));
  EXPECT_TRUE(columns[4].nullable);
  EXPECT_EQ("created", columns[5].name);
  EXPECT_EQ("DATETIME", std::string(to_string(columns[5].type)));
  EXPECT_TRUE(columns[5].nullable);
  EXPECT_EQ("note", columns[6].name);
  EXPECT_EQ("VARCHAR", std::string(to_string(columns[6].type)));
  EXPECT_FALSE(columns[6].nullable);
  // The duplicated header name is made unique.
  EXPECT_EQ("id__2", columns[7].name);
  EXPECT_EQ("BIGINT", std::string(to_string(columns[7].type)));
}

TEST_F(FileScanTest, scan_csv_rows)
{
  write_file("basic.csv",
             "id,name,note\n"
             "1,Alice,\"hello,\n"
             "world\"\n"
             "2,Bob,\"say \"\"hi\"\"\"\n");

  ReadOptions options;
  CsvScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("basic.csv"), options, &error)) << error.message;
  std::vector<ColumnSchema> columns;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&columns, &error)) << error.message;

  std::vector<std::string> fields;
  std::vector<char> nulls;
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  ASSERT_EQ(3U, fields.size());
  EXPECT_EQ("1", fields[0]);
  EXPECT_EQ("Alice", fields[1]);
  EXPECT_EQ("hello,\nworld", fields[2]);
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ("2", fields[0]);
  EXPECT_EQ("say \"hi\"", fields[2]);
  EXPECT_FALSE(scanner.next_row(&fields, &nulls, &error));
}

TEST_F(FileScanTest, csv_read_options)
{
  // A file without a header row produces generated column names.
  write_file("no_header.csv", "1,Alice\n2,Bob\n");
  ReadOptions options;
  options.header = false;
  CsvScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("no_header.csv"), options, &error)) << error.message;
  std::vector<ColumnSchema> columns;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&columns, &error)) << error.message;
  ASSERT_EQ(2U, columns.size());
  EXPECT_EQ("column0", columns[0].name);
  EXPECT_EQ("column1", columns[1].name);

  // An empty field is a NULL value.
  write_file("nulls.csv", "a,b\n1,\n,2\n");
  ReadOptions plain;
  CsvScanner null_scanner;
  ASSERT_EQ(Status::OK, null_scanner.open(path("nulls.csv"), plain, &error)) << error.message;
  ASSERT_EQ(Status::OK, null_scanner.infer_schema(&columns, &error)) << error.message;
  EXPECT_TRUE(columns[0].nullable);
  EXPECT_TRUE(columns[1].nullable);
  std::vector<std::string> fields;
  std::vector<char> nulls;
  ASSERT_TRUE(null_scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ(1, nulls[1]);
  EXPECT_EQ(0, nulls[0]);

  // The null string option turns a literal into a NULL value.
  write_file("nullstr.csv", "a,b\n1,NULL\n");
  ReadOptions with_null;
  with_null.null_string = "NULL";
  CsvScanner nullstr_scanner;
  ASSERT_EQ(Status::OK, nullstr_scanner.open(path("nullstr.csv"), with_null, &error)) << error.message;
  ASSERT_EQ(Status::OK, nullstr_scanner.infer_schema(&columns, &error)) << error.message;
  ASSERT_TRUE(nullstr_scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ(1, nulls[1]);
}

TEST_F(FileScanTest, infer_jsonl_fields)
{
  write_file("basic.jsonl",
             "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
             "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\n"
             "{\"id\":3,\"name\":\"Chen\",\"score\":90,\"active\":true,\"note\":\"ok\"}\n");

  ReadOptions options;
  options.format = Format::JSONL;
  JsonlScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("basic.jsonl"), options, &error)) << error.message;
  std::vector<ColumnSchema> columns;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&columns, &error)) << error.message;
  ASSERT_EQ(6U, columns.size());
  // The fields are the union of all the records in the order they appear.
  EXPECT_EQ("id", columns[0].name);
  EXPECT_EQ("name", columns[1].name);
  EXPECT_EQ("score", columns[2].name);
  EXPECT_EQ("active", columns[3].name);
  EXPECT_EQ("day", columns[4].name);
  EXPECT_EQ("note", columns[5].name);
  EXPECT_EQ("BIGINT", std::string(to_string(columns[0].type)));
  EXPECT_EQ("DOUBLE", std::string(to_string(columns[2].type)));
  EXPECT_EQ("BOOLEAN", std::string(to_string(columns[3].type)));
  EXPECT_EQ("DATE", std::string(to_string(columns[4].type)));
  EXPECT_EQ("VARCHAR", std::string(to_string(columns[5].type)));

  std::vector<std::string> fields;
  std::vector<char> nulls;
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  ASSERT_EQ(6U, fields.size());
  EXPECT_EQ("1", fields[0]);
  EXPECT_EQ(1, nulls[4]);
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ("2", fields[0]);
  // The missing score is a NULL value.
  EXPECT_EQ(1, nulls[2]);
  EXPECT_EQ("2026-08-02", fields[4]);
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ("90", fields[2]);
  EXPECT_EQ("ok", fields[5]);
  EXPECT_FALSE(scanner.next_row(&fields, &nulls, &error));
}

TEST_F(FileScanTest, jsonl_rejects_nested_values)
{
  write_file("nested.jsonl", "{\"a\":1}\n{\"a\":{\"b\":2}}\n");
  ReadOptions options;
  JsonlScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("nested.jsonl"), options, &error)) << error.message;
  std::vector<ColumnSchema> columns;
  EXPECT_EQ(Status::PARSE_ERROR, scanner.infer_schema(&columns, &error));
  EXPECT_EQ(2, error.line);

  write_file("nested_array.jsonl", "{\"a\":[1,2]}\n");
  JsonlScanner array_scanner;
  ASSERT_EQ(Status::OK, array_scanner.open(path("nested_array.jsonl"), options, &error))
      << error.message;
  EXPECT_EQ(Status::PARSE_ERROR, array_scanner.infer_schema(&columns, &error));
}

TEST_F(FileScanTest, csv_reports_a_broken_row)
{
  write_file("bad.csv", "a,b\n1,2\n3\n");
  ReadOptions options;
  CsvScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("bad.csv"), options, &error)) << error.message;
  std::vector<ColumnSchema> columns;
  EXPECT_EQ(Status::PARSE_ERROR, scanner.infer_schema(&columns, &error));
  EXPECT_EQ(3, error.line);
}

TEST_F(FileScanTest, detect_a_changed_file)
{
  write_file("changing.csv", "a\n1\n");
  FileStamp first;
  FileStamp second;
  Error error;
  ASSERT_EQ(Status::OK, take_file_stamp(path("changing.csv"), &first, &error)) << error.message;
  ASSERT_EQ(Status::OK, take_file_stamp(path("changing.csv"), &second, &error)) << error.message;
  EXPECT_TRUE(file_stamp_equal(first, second));

  write_file("changing.csv", "a\n1\n2\n");
  FileStamp third;
  ASSERT_EQ(Status::OK, take_file_stamp(path("changing.csv"), &third, &error)) << error.message;
  EXPECT_FALSE(file_stamp_equal(first, third));
}

TEST_F(FileScanTest, glob_lists_the_matching_files)
{
  write_file("a.csv", "a\n");
  write_file("b.csv", "b\n");
  write_file("c.txt", "c\n");
  std::vector<std::string> files;
  Error error;
  ASSERT_EQ(Status::OK, glob_files(path("*.csv"), &files, &error)) << error.message;
  ASSERT_EQ(2U, files.size());
  EXPECT_EQ("a.csv", files[0].substr(files[0].find_last_of('/') + 1));
  EXPECT_EQ("b.csv", files[1].substr(files[1].find_last_of('/') + 1));
}

TEST_F(FileScanTest, type_inference_rules)
{
  TypeInferrer inferrer;
  inferrer.observe("1", false);
  EXPECT_EQ("BIGINT", std::string(to_string(inferrer.type())));
  inferrer.observe("2.5", false);
  EXPECT_EQ("DOUBLE", std::string(to_string(inferrer.type())));
  inferrer.observe("", true);
  EXPECT_TRUE(inferrer.has_null());
  EXPECT_EQ("DOUBLE", std::string(to_string(inferrer.type())));

  TypeInferrer all_null;
  all_null.observe("", true);
  EXPECT_EQ("NULL", std::string(to_string(all_null.type())));

  TypeInferrer boundary;
  boundary.observe("9223372036854775807", false);
  EXPECT_EQ("BIGINT", std::string(to_string(boundary.type())));

  TypeInferrer overflow;
  overflow.observe("9223372036854775808", false);
  EXPECT_EQ("DOUBLE", std::string(to_string(overflow.type())));

  TypeInferrer temporal;
  temporal.observe("2026-08-01", false);
  temporal.observe("2026-08-03T12:13:14", false);
  EXPECT_EQ("DATETIME", std::string(to_string(temporal.type())));

  TypeInferrer mixed;
  mixed.observe("2026-08-01", false);
  mixed.observe("1", false);
  EXPECT_EQ("VARCHAR", std::string(to_string(mixed.type())));
}

TEST_F(FileScanTest, unique_column_names)
{
  std::vector<std::string> names;
  names.push_back("id");
  names.push_back("name");
  names.push_back("id");
  const std::vector<std::string> unique = make_unique_names(names);
  ASSERT_EQ(3U, unique.size());
  EXPECT_EQ("id", unique[0]);
  EXPECT_EQ("name", unique[1]);
  EXPECT_EQ("id__2", unique[2]);
}

TEST_F(FileScanTest, missing_file_is_reported)
{
  ReadOptions options;
  CsvScanner scanner;
  Error error;
  EXPECT_NE(Status::OK, scanner.open(path("does_not_exist.csv"), options, &error));
  EXPECT_EQ(Status::NOT_FOUND, error.status);
}

} // namespace
} // namespace sql
} // namespace oceanbase
