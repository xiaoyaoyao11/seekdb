// The round trip test of the COPY statement: the file that the writer
// produces must be read back by the reader of the read_csv() table function.
#include "sql/engine/basic/ob_file_copy_writer.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace oceanbase::sql::file_scan;

static int failures = 0;

#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cout << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl; \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

static void round_trip(const std::string &path,
                       const CopyOptions &copy_opts,
                       const ReadOptions &read_opts)
{
  Error err;
  CopyWriter w;
  if (Status::OK != w.open(path, copy_opts, &err)) {
    std::cout << "FAIL: open [" << err.message << "]" << std::endl;
    ++failures;
    return;
  }
  const std::vector<std::string> names = {"id", "name", "score"};
  CHECK(Status::OK == w.write_header(names, &err), "header");
  std::vector<std::string> f1 = {"1", "alice", "9.5"};
  std::vector<char> n1 = {0, 0, 0};
  CHECK(Status::OK == w.write_row(f1, n1, &err), "row1");
  std::vector<std::string> f2 = {"2", "bob", ""};
  std::vector<char> n2 = {0, 1, 1};   // name and score are NULL
  CHECK(Status::OK == w.write_row(f2, n2, &err), "row2");
  std::vector<std::string> f3 = {"3", "a,b", "1"};
  std::vector<char> n3 = {0, 0, 0};
  CHECK(Status::OK == w.write_row(f3, n3, &err), "row3");
  CHECK(Status::OK == w.close(&err), "close");

  // Read the file back with the reader of read_csv().
  CsvScanner scanner;
  ReadOptions ropts = read_opts;
  ropts.format = Format::CSV;
  if (Status::OK != scanner.open(path, ropts, &err)) {
    std::cout << "FAIL: scanner open [" << err.message << "]" << std::endl;
    ++failures;
    return;
  }
  std::vector<ColumnSchema> schema;
  CHECK(Status::OK == scanner.infer_schema(&schema, &err), "infer_schema");
  CHECK(3 == schema.size(), "the schema must have three columns");
  if (3 == schema.size()) {
    CHECK("id" == schema[0].name, "the name of the first column");
    CHECK("name" == schema[1].name, "the name of the second column");
    CHECK("score" == schema[2].name, "the name of the third column");
  }

  std::vector<std::string> fields;
  std::vector<char> nulls;
  CHECK(scanner.next_row(&fields, &nulls, &err), "the first row");
  CHECK(3 == fields.size(), "the first row has three fields");
  if (3 == fields.size()) {
    CHECK("1" == fields[0] && !nulls[0], "the first field of the first row");
    CHECK("alice" == fields[1] && !nulls[1], "the second field of the first row");
    CHECK("9.5" == fields[2] && !nulls[2], "the third field of the first row");
  }
  CHECK(scanner.next_row(&fields, &nulls, &err), "the second row");
  if (3 == fields.size()) {
    CHECK("2" == fields[0] && !nulls[0], "the first field of the second row");
    CHECK(nulls[1], "the second field of the second row must be NULL");
    CHECK(nulls[2], "the third field of the second row must be NULL");
  }
  CHECK(scanner.next_row(&fields, &nulls, &err), "the third row");
  if (3 == fields.size()) {
    CHECK("3" == fields[0] && !nulls[0], "the first field of the third row");
    CHECK("a,b" == fields[1] && !nulls[1], "the quoted field must survive the round trip");
    CHECK("1" == fields[2] && !nulls[2], "the third field of the third row");
  }
  CHECK(!scanner.next_row(&fields, &nulls, &err), "there must be no fourth row");
  scanner.close();
  std::remove(path.c_str());
}

int main()
{
  {
    // The default: a NULL value is written as an unquoted empty field, the
    // reader of the file interprets it as a NULL value.
    CopyOptions copy_opts;
    ReadOptions read_opts;
    read_opts.header = true;
    round_trip("copy_roundtrip_default.csv", copy_opts, read_opts);
  }
  {
    // The NULL text is configured on both sides.
    CopyOptions copy_opts;
    copy_opts.null_string_ = "NULL";
    ReadOptions read_opts;
    read_opts.header = true;
    read_opts.null_string = "NULL";
    round_trip("copy_roundtrip_nullstr.csv", copy_opts, read_opts);
  }
  if (0 == failures) {
    std::cout << "ALL ROUND TRIP TESTS PASSED" << std::endl;
    return 0;
  }
  std::cout << failures << " TEST(S) FAILED" << std::endl;
  return 1;
}
