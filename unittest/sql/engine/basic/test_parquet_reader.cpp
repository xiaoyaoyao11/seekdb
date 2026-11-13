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
 * @file test_parquet_reader.cpp
 *
 * Unit tests of the Parquet reader that backs the read_parquet() table
 * function.
 *
 * The tests build small Parquet files with a writer that is part of the
 * test, they cover the footer parsing, the schema derivation, the value
 * encodings, the optional columns, the several row groups, the column
 * pruning and the diagnostics of a malformed file.
 */

#include <gtest/gtest.h>
#include "sql/engine/basic/ob_parquet_reader.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
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

/// The Parquet enumerations that the tests use.
enum PhysicalType
{
  PHYSICAL_BOOLEAN    = 0,
  PHYSICAL_INT32      = 1,
  PHYSICAL_INT64      = 2,
  PHYSICAL_DOUBLE     = 5,
  PHYSICAL_BYTE_ARRAY = 6,
};

enum RepetitionType
{
  REPETITION_REQUIRED = 0,
  REPETITION_OPTIONAL = 1,
};

enum ConvertedType
{
  CONVERTED_UTF8 = 0,
  CONVERTED_DATE = 6,
};

/// A writer that emits the Thrift compact protocol.
class CompactWriter
{
public:
  void write_byte(uint8_t value) { buffer_.push_back(static_cast<char>(value)); }

  void write_varint(uint64_t value)
  {
    while (true) {
      if ((value & ~0x7FULL) == 0) {
        write_byte(static_cast<uint8_t>(value));
        return;
      }
      write_byte(static_cast<uint8_t>((value & 0x7F) | 0x80));
      value >>= 7;
    }
  }

  void write_zigzag(int64_t value)
  {
    write_varint(static_cast<uint64_t>((value << 1) ^ (value >> 63)));
  }

  void write_i32(int32_t value) { write_zigzag(value); }
  void write_i64(int64_t value) { write_zigzag(value); }

  void write_binary(const std::string &value)
  {
    write_varint(value.size());
    buffer_.append(value);
  }

  void write_list_header(uint8_t size, uint8_t element_type)
  {
    write_byte(static_cast<uint8_t>((size << 4) | (element_type & 0x0F)));
  }

  void write_field(uint8_t type, int16_t field_id, int16_t previous_id)
  {
    const int16_t delta = static_cast<int16_t>(field_id - previous_id);
    if (delta > 0 && delta <= 15) {
      write_byte(static_cast<uint8_t>((delta << 4) | (type & 0x0F)));
    } else {
      write_byte(static_cast<uint8_t>(type & 0x0F));
      write_zigzag(field_id);
    }
  }

  void write_stop() { write_byte(0x00); }

  std::string &buffer() { return buffer_; }
  const std::string &buffer() const { return buffer_; }

private:
  std::string buffer_;
};

/// The description of one column of a generated file.
struct TestColumn
{
  std::string name;
  int physical_type = PHYSICAL_INT64;
  int repetition    = REPETITION_REQUIRED;
  int converted     = -1;
};

/// The values of one column of one row group.
struct TestColumnData
{
  std::vector<std::string> values;
  std::vector<char> nulls;
};

/// Write the PLAIN encoding of the values of one column.
void write_plain_values(CompactWriter *writer, const TestColumn &column, const TestColumnData &data)
{
  for (size_t i = 0; i < data.values.size(); ++i) {
    if (i < data.nulls.size() && data.nulls[i]) {
      continue;
    }
    const std::string &value = data.values[i];
    switch (column.physical_type) {
    case PHYSICAL_INT32: {
      const int32_t number = static_cast<int32_t>(std::strtoll(value.c_str(), nullptr, 10));
      writer->buffer().append(reinterpret_cast<const char *>(&number), 4);
      break;
    }
    case PHYSICAL_INT64: {
      const int64_t number = static_cast<int64_t>(std::strtoll(value.c_str(), nullptr, 10));
      writer->buffer().append(reinterpret_cast<const char *>(&number), 8);
      break;
    }
    case PHYSICAL_DOUBLE: {
      const double number = std::strtod(value.c_str(), nullptr);
      writer->buffer().append(reinterpret_cast<const char *>(&number), 8);
      break;
    }
    case PHYSICAL_BOOLEAN: {
      const bool truth = ("true" == value);
      const size_t index = i;
      const size_t byte  = index / 8;
      if (byte >= writer->buffer().size()) {
        writer->buffer().push_back('\0');
      }
      if (truth) {
        writer->buffer()[byte] = static_cast<char>(
            static_cast<uint8_t>(writer->buffer()[byte]) | (1 << (index % 8)));
      }
      break;
    }
    case PHYSICAL_BYTE_ARRAY: {
      const uint32_t length = static_cast<uint32_t>(value.size());
      writer->buffer().append(reinterpret_cast<const char *>(&length), 4);
      writer->buffer().append(value);
      break;
    }
    default:
      break;
    }
  }
}

/// Write the definition levels of one optional column.
void write_def_levels(CompactWriter *writer, const TestColumnData &data)
{
  const size_t count = data.values.size();
  if (0 == count) {
    return;
  }
  const uint64_t groups = (count + 7) / 8;
  const uint32_t header = static_cast<uint32_t>((groups << 1) | 1);
  writer->buffer().append(reinterpret_cast<const char *>(&header), 4);
  std::string packed((count + 7) / 8, '\0');
  for (size_t i = 0; i < count; ++i) {
    const bool present = !(i < data.nulls.size() && data.nulls[i]);
    if (present) {
      packed[i / 8] = static_cast<char>(static_cast<uint8_t>(packed[i / 8]) | (1 << (i % 8)));
    }
  }
  writer->buffer().append(packed);
}

/// A temporary directory that holds the files of a single test.
class ParquetReaderTest : public ::testing::Test
{
public:
  void SetUp() override
  {
    dir_ = std::string("parquet_reader_test_")
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

  /**
   * Write a Parquet file that holds the given columns.
   *
   * The file uses the UNCOMPRESSED codec and the PLAIN encoding, it holds
   * one row group per entry of `row_groups`.
   */
  void write_parquet(const char *name,
                     const std::vector<TestColumn> &columns,
                     const std::vector<std::vector<TestColumnData>> &row_groups)
  {
    std::string file;
    file.append("PAR1", 4);

    std::vector<std::vector<int64_t>> offsets;
    std::vector<std::vector<int64_t>> sizes;
    for (size_t g = 0; g < row_groups.size(); ++g) {
      std::vector<int64_t> group_offsets;
      std::vector<int64_t> group_sizes;
      for (size_t c = 0; c < columns.size(); ++c) {
        const TestColumn &column = columns[c];
        const TestColumnData &data = row_groups[g][c];
        CompactWriter payload;
        if (REPETITION_OPTIONAL == column.repetition) {
          write_def_levels(&payload, data);
        }
        write_plain_values(&payload, column, data);

        CompactWriter header;
        header.write_field(0x05, 1, 0);
        header.write_i32(0);
        header.write_field(0x05, 2, 1);
        header.write_i32(static_cast<int32_t>(payload.buffer().size()));
        header.write_field(0x05, 3, 2);
        header.write_i32(static_cast<int32_t>(payload.buffer().size()));
        header.write_field(0x0C, 5, 3);
        header.write_field(0x05, 1, 0);
        header.write_i32(static_cast<int>(data.values.size()));
        header.write_field(0x05, 2, 1);
        header.write_i32(0); // PLAIN
        header.write_field(0x05, 3, 2);
        header.write_i32(3); // RLE
        header.write_field(0x05, 4, 3);
        header.write_i32(3); // RLE
        header.write_stop();
        header.write_stop();

        group_offsets.push_back(static_cast<int64_t>(file.size()));
        group_sizes.push_back(static_cast<int64_t>(header.buffer().size()
                                                   + payload.buffer().size()));
        file.append(header.buffer());
        file.append(payload.buffer());
      }
      offsets.push_back(group_offsets);
      sizes.push_back(group_sizes);
    }

    int64_t num_rows = 0;
    for (size_t g = 0; g < row_groups.size(); ++g) {
      num_rows += static_cast<int64_t>(row_groups[g][0].values.size());
    }

    CompactWriter footer;
    footer.write_field(0x05, 1, 0);
    footer.write_i32(1);
    footer.write_field(0x09, 2, 1);
    footer.write_list_header(static_cast<uint8_t>(columns.size() + 1), 0x0C);
    {
      footer.write_field(0x05, 3, 0);
      footer.write_i32(REPETITION_REQUIRED);
      footer.write_field(0x08, 4, 3);
      footer.write_binary("root");
      footer.write_field(0x05, 5, 4);
      footer.write_i32(static_cast<int32_t>(columns.size()));
      footer.write_stop();
      for (size_t c = 0; c < columns.size(); ++c) {
        const TestColumn &column = columns[c];
        footer.write_field(0x05, 1, 0);
        footer.write_i32(column.physical_type);
        footer.write_field(0x05, 3, 1);
        footer.write_i32(column.repetition);
        footer.write_field(0x08, 4, 3);
        footer.write_binary(column.name);
        if (column.converted >= 0) {
          footer.write_field(0x05, 6, 4);
          footer.write_i32(column.converted);
        }
        footer.write_stop();
      }
    }
    footer.write_field(0x06, 3, 2);
    footer.write_i64(num_rows);
    footer.write_field(0x09, 4, 3);
    footer.write_list_header(static_cast<uint8_t>(row_groups.size()), 0x0C);
    for (size_t g = 0; g < row_groups.size(); ++g) {
      footer.write_field(0x09, 1, 0);
      footer.write_list_header(static_cast<uint8_t>(columns.size()), 0x0C);
      for (size_t c = 0; c < columns.size(); ++c) {
        footer.write_field(0x08, 1, 0);
        footer.write_binary("");
        footer.write_field(0x06, 2, 1);
        footer.write_i64(offsets[g][c]);
        footer.write_field(0x0C, 3, 2);
        footer.write_field(0x05, 1, 0);
        footer.write_i32(columns[c].physical_type);
        footer.write_field(0x09, 2, 1);
        footer.write_list_header(1, 0x05);
        footer.write_i32(0); // PLAIN
        footer.write_field(0x09, 3, 2);
        footer.write_list_header(1, 0x08);
        footer.write_binary(columns[c].name);
        footer.write_field(0x05, 4, 3);
        footer.write_i32(0); // UNCOMPRESSED
        footer.write_field(0x06, 5, 4);
        footer.write_i64(static_cast<int64_t>(row_groups[g][c].values.size()));
        footer.write_field(0x06, 6, 5);
        footer.write_i64(sizes[g][c]);
        footer.write_field(0x06, 7, 6);
        footer.write_i64(sizes[g][c]);
        footer.write_field(0x06, 8, 7);
        footer.write_i64(offsets[g][c]);
        footer.write_stop();
        footer.write_stop();
      }
      footer.write_field(0x06, 2, 1);
      footer.write_i64(0);
      footer.write_field(0x06, 3, 2);
      footer.write_i64(static_cast<int64_t>(row_groups[g][0].values.size()));
      footer.write_stop();
    }
    footer.write_field(0x08, 6, 4);
    footer.write_binary("parquet_reader_test");
    footer.write_stop();

    const uint32_t footer_size = static_cast<uint32_t>(footer.buffer().size());
    file.append(footer.buffer());
    const uint8_t size_bytes[4] = {
      static_cast<uint8_t>(footer_size & 0xFF),
      static_cast<uint8_t>((footer_size >> 8) & 0xFF),
      static_cast<uint8_t>((footer_size >> 16) & 0xFF),
      static_cast<uint8_t>((footer_size >> 24) & 0xFF),
    };
    file.append(reinterpret_cast<const char *>(size_bytes), 4);
    file.append("PAR1", 4);
    write_file(name, file);
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

  std::string dir_;
};

TEST_F(ParquetReaderTest, read_a_single_int64_column)
{
  std::vector<TestColumn> columns;
  TestColumn c1;
  c1.name          = "c1";
  c1.physical_type = PHYSICAL_INT64;
  columns.push_back(c1);

  std::vector<TestColumnData> group;
  TestColumnData data;
  for (int i = 0; i < 7; ++i) {
    data.values.push_back(std::to_string(i));
    data.nulls.push_back(0);
  }
  group.push_back(data);
  std::vector<std::vector<TestColumnData>> groups;
  groups.push_back(group);
  write_parquet("simple.parquet", columns, groups);

  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("simple.parquet"), options, &error)) << error.message;

  std::vector<ColumnSchema> schema;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&schema, &error)) << error.message;
  ASSERT_EQ(1U, schema.size());
  EXPECT_EQ("c1", schema[0].name);
  EXPECT_EQ("BIGINT", std::string(to_string(schema[0].type)));
  EXPECT_FALSE(schema[0].nullable);
  EXPECT_EQ(7, scanner.row_count());
  EXPECT_EQ(1U, scanner.header_names().size());
  EXPECT_EQ("c1", scanner.header_names()[0]);

  // The aggregation of the reference data, computed on the textual values.
  std::vector<std::string> fields;
  std::vector<char> nulls;
  int64_t count = 0;
  int64_t sum   = 0;
  int64_t min   = 0;
  int64_t max   = 0;
  while (scanner.next_row(&fields, &nulls, &error)) {
    ASSERT_EQ(1U, fields.size());
    const int64_t value = std::strtoll(fields[0].c_str(), nullptr, 10);
    if (0 == count) {
      min = value;
      max = value;
    } else {
      if (value < min) { min = value; }
      if (value > max) { max = value; }
    }
    sum += value;
    ++count;
  }
  EXPECT_EQ(7, count);
  EXPECT_EQ(0, min);
  EXPECT_EQ(6, max);
  EXPECT_EQ(21, sum);

  // A rescan has to produce the very same rows again.
  ASSERT_EQ(Status::OK, scanner.rewind(&error)) << error.message;
  count = 0;
  while (scanner.next_row(&fields, &nulls, &error)) {
    ++count;
  }
  EXPECT_EQ(7, count);
}

TEST_F(ParquetReaderTest, read_the_supported_physical_types)
{
  std::vector<TestColumn> columns;
  {
    TestColumn c;
    c.name          = "i32";
    c.physical_type = PHYSICAL_INT32;
    columns.push_back(c);
  }
  {
    TestColumn c;
    c.name          = "dbl";
    c.physical_type = PHYSICAL_DOUBLE;
    columns.push_back(c);
  }
  {
    TestColumn c;
    c.name          = "flag";
    c.physical_type = PHYSICAL_BOOLEAN;
    columns.push_back(c);
  }
  {
    TestColumn c;
    c.name          = "txt";
    c.physical_type = PHYSICAL_BYTE_ARRAY;
    c.converted     = CONVERTED_UTF8;
    columns.push_back(c);
  }
  {
    TestColumn c;
    c.name          = "day";
    c.physical_type = PHYSICAL_INT32;
    c.converted     = CONVERTED_DATE;
    columns.push_back(c);
  }

  std::vector<TestColumnData> group;
  {
    TestColumnData d;
    d.values.push_back("-2147483648");
    d.values.push_back("2147483647");
    d.nulls.push_back(0);
    d.nulls.push_back(0);
    group.push_back(d);
  }
  {
    TestColumnData d;
    d.values.push_back("-1.5");
    d.values.push_back("3.25");
    d.nulls.push_back(0);
    d.nulls.push_back(0);
    group.push_back(d);
  }
  {
    TestColumnData d;
    d.values.push_back("true");
    d.values.push_back("false");
    d.nulls.push_back(0);
    d.nulls.push_back(0);
    group.push_back(d);
  }
  {
    TestColumnData d;
    d.values.push_back("alpha");
    d.values.push_back("");
    d.nulls.push_back(0);
    d.nulls.push_back(0);
    group.push_back(d);
  }
  {
    TestColumnData d;
    d.values.push_back("0");
    d.values.push_back("20000");
    d.nulls.push_back(0);
    d.nulls.push_back(0);
    group.push_back(d);
  }
  std::vector<std::vector<TestColumnData>> groups;
  groups.push_back(group);
  write_parquet("types.parquet", columns, groups);

  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("types.parquet"), options, &error)) << error.message;

  std::vector<ColumnSchema> schema;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&schema, &error)) << error.message;
  ASSERT_EQ(5U, schema.size());
  EXPECT_EQ("BIGINT", std::string(to_string(schema[0].type)));
  EXPECT_EQ("DOUBLE", std::string(to_string(schema[1].type)));
  EXPECT_EQ("BOOLEAN", std::string(to_string(schema[2].type)));
  EXPECT_EQ("VARCHAR", std::string(to_string(schema[3].type)));
  EXPECT_EQ("DATE", std::string(to_string(schema[4].type)));

  std::vector<std::string> fields;
  std::vector<char> nulls;
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  ASSERT_EQ(5U, fields.size());
  EXPECT_EQ("-2147483648", fields[0]);
  EXPECT_EQ("-1.5", fields[1]);
  EXPECT_EQ("true", fields[2]);
  EXPECT_EQ("alpha", fields[3]);
  EXPECT_EQ("1970-01-01", fields[4]);
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ("2147483647", fields[0]);
  EXPECT_EQ("3.25", fields[1]);
  EXPECT_EQ("false", fields[2]);
  EXPECT_EQ("", fields[3]);
  EXPECT_EQ("2024-10-04", fields[4]);
  EXPECT_FALSE(scanner.next_row(&fields, &nulls, &error));
}

TEST_F(ParquetReaderTest, an_optional_column_carries_nulls)
{
  std::vector<TestColumn> columns;
  {
    TestColumn c;
    c.name          = "c1";
    c.physical_type = PHYSICAL_INT64;
    c.repetition    = REPETITION_OPTIONAL;
    columns.push_back(c);
  }

  std::vector<TestColumnData> group;
  TestColumnData data;
  data.values.push_back("10");
  data.values.push_back("");
  data.values.push_back("30");
  data.nulls.push_back(0);
  data.nulls.push_back(1);
  data.nulls.push_back(0);
  group.push_back(data);
  std::vector<std::vector<TestColumnData>> groups;
  groups.push_back(group);
  write_parquet("optional.parquet", columns, groups);

  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("optional.parquet"), options, &error)) << error.message;

  std::vector<ColumnSchema> schema;
  ASSERT_EQ(Status::OK, scanner.infer_schema(&schema, &error)) << error.message;
  ASSERT_EQ(1U, schema.size());
  EXPECT_TRUE(schema[0].nullable);

  std::vector<std::string> fields;
  std::vector<char> nulls;
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ("10", fields[0]);
  EXPECT_EQ(0, nulls[0]);
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ(1, nulls[0]);
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  EXPECT_EQ("30", fields[0]);
  EXPECT_EQ(0, nulls[0]);
  EXPECT_FALSE(scanner.next_row(&fields, &nulls, &error));
}

TEST_F(ParquetReaderTest, several_row_groups_are_read_in_order)
{
  std::vector<TestColumn> columns;
  TestColumn c1;
  c1.name          = "c1";
  c1.physical_type = PHYSICAL_INT64;
  columns.push_back(c1);

  std::vector<std::vector<TestColumnData>> groups;
  for (int g = 0; g < 3; ++g) {
    TestColumnData data;
    for (int i = 0; i < 4; ++i) {
      data.values.push_back(std::to_string(g * 4 + i));
      data.nulls.push_back(0);
    }
    std::vector<TestColumnData> group;
    group.push_back(data);
    groups.push_back(group);
  }
  write_parquet("groups.parquet", columns, groups);

  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("groups.parquet"), options, &error)) << error.message;
  EXPECT_EQ(12, scanner.row_count());

  std::vector<std::string> fields;
  std::vector<char> nulls;
  int64_t expected = 0;
  int64_t count    = 0;
  while (scanner.next_row(&fields, &nulls, &error)) {
    ASSERT_EQ(1U, fields.size());
    EXPECT_EQ(std::to_string(expected), fields[0]);
    ++expected;
    ++count;
  }
  EXPECT_EQ(12, count);
}

TEST_F(ParquetReaderTest, a_projection_reads_only_the_named_columns)
{
  std::vector<TestColumn> columns;
  {
    TestColumn c;
    c.name          = "a";
    c.physical_type = PHYSICAL_INT64;
    columns.push_back(c);
  }
  {
    TestColumn c;
    c.name          = "b";
    c.physical_type = PHYSICAL_BYTE_ARRAY;
    columns.push_back(c);
  }

  std::vector<TestColumnData> group;
  {
    TestColumnData d;
    d.values.push_back("1");
    d.nulls.push_back(0);
    group.push_back(d);
  }
  {
    TestColumnData d;
    d.values.push_back("two");
    d.nulls.push_back(0);
    group.push_back(d);
  }
  std::vector<std::vector<TestColumnData>> groups;
  groups.push_back(group);
  write_parquet("projection.parquet", columns, groups);

  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  ASSERT_EQ(Status::OK, scanner.open(path("projection.parquet"), options, &error)) << error.message;

  std::vector<std::string> wanted;
  wanted.push_back("b");
  ASSERT_EQ(Status::OK, scanner.set_projection(wanted, &error)) << error.message;

  std::vector<std::string> fields;
  std::vector<char> nulls;
  ASSERT_TRUE(scanner.next_row(&fields, &nulls, &error)) << error.message;
  ASSERT_EQ(1U, fields.size());
  EXPECT_EQ("two", fields[0]);
  EXPECT_FALSE(scanner.next_row(&fields, &nulls, &error));

  // A column that the file does not hold is reported.
  std::vector<std::string> missing;
  missing.push_back("no_such_column");
  ParquetScanner other;
  ASSERT_EQ(Status::OK, other.open(path("projection.parquet"), options, &error)) << error.message;
  EXPECT_EQ(Status::INVALID_ARGUMENT, other.set_projection(missing, &error));
}

TEST_F(ParquetReaderTest, a_file_that_is_not_a_parquet_file_is_reported)
{
  write_file("not_parquet.txt", "this is not a parquet file at all\n");
  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  EXPECT_EQ(Status::PARSE_ERROR, scanner.open(path("not_parquet.txt"), options, &error));
  EXPECT_NE(std::string::npos, error.message.find("not a parquet file"));
}

TEST_F(ParquetReaderTest, a_missing_file_is_reported)
{
  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  EXPECT_EQ(Status::NOT_FOUND, scanner.open(path("does_not_exist.parquet"), options, &error));
}

TEST_F(ParquetReaderTest, a_truncated_file_is_reported)
{
  write_file("truncated.parquet", "PAR1");
  ReadOptions options;
  options.format = Format::PARQUET;
  ParquetScanner scanner;
  Error error;
  EXPECT_EQ(Status::PARSE_ERROR, scanner.open(path("truncated.parquet"), options, &error));
}

} // namespace
} // namespace sql
} // namespace oceanbase
