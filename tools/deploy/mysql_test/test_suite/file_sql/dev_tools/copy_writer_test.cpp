// A standalone harness that exercises the CSV writer of the COPY statement.
#include "sql/engine/basic/ob_file_copy_writer.h"

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

static std::string read_all(const std::string &path)
{
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (NULL == f) { return ""; }
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { out.append(buf, n); }
  std::fclose(f);
  return out;
}

static bool file_present(const std::string &path)
{
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (NULL == f) { return false; }
  std::fclose(f);
  return true;
}

static void test_basic()
{
  const std::string path = "copy_test_basic.csv";
  std::remove(path.c_str());
  CopyOptions opts;
  opts.has_header_ = true;
  Error err;
  CopyWriter w;
  CHECK(Status::OK == w.open(path, opts, &err), "open");
  CHECK(Status::OK == w.write_header({"id", "name"}, &err), "header");
  std::vector<std::string> f1 = {"1", "alice"};
  std::vector<char> n1 = {0, 0};
  CHECK(Status::OK == w.write_row(f1, n1, &err), "row1");
  std::vector<std::string> f2 = {"2", "bob"};
  std::vector<char> n2 = {0, 1};   // name is NULL
  CHECK(Status::OK == w.write_row(f2, n2, &err), "row2");
  CHECK(Status::OK == w.close(&err), "close");
  const std::string content = read_all(path);
  CHECK("id,name\n1,alice\n2,\n" == content, "basic content: [" + content + "]");
  CHECK(2 == w.row_count(), "row count");
  std::remove(path.c_str());
}

static void test_existing_file()
{
  const std::string path = "copy_test_exists.csv";
  std::remove(path.c_str());
  { std::FILE *f = std::fopen(path.c_str(), "wb"); std::fputs("x", f); std::fclose(f); }
  CopyOptions opts;
  Error err;
  CopyWriter w;
  CHECK(Status::OK != w.open(path, opts, &err), "must refuse to overwrite an existing file");
  CHECK(Status::IO_ERROR == err.status, "the status must be IO_ERROR");
  // The pre-existing file must be untouched.
  CHECK("x" == read_all(path), "the existing file must not be modified");
  std::remove(path.c_str());
}

static void test_quoting()
{
  const std::string path = "copy_test_quote.csv";
  std::remove(path.c_str());
  CopyOptions opts;
  Error err;
  CopyWriter w;
  CHECK(Status::OK == w.open(path, opts, &err), "open");
  std::vector<std::string> f = {"a,b", "say \"hi\"", "line\nbreak"};
  std::vector<char> n = {0, 0, 0};
  CHECK(Status::OK == w.write_row(f, n, &err), "row");
  CHECK(Status::OK == w.close(&err), "close");
  const std::string content = read_all(path);
  const std::string expected = "\"a,b\",\"say \"\"hi\"\"\",\"line\nbreak\"\n";
  CHECK(expected == content, "quoting content: [" + content + "]");
  std::remove(path.c_str());
}

static void test_null_string()
{
  const std::string path = "copy_test_null.csv";
  std::remove(path.c_str());
  CopyOptions opts;
  opts.null_string_ = "NULL";
  Error err;
  CopyWriter w;
  CHECK(Status::OK == w.open(path, opts, &err), "open");
  std::vector<std::string> f = {"1", ""};
  std::vector<char> n = {0, 1};
  CHECK(Status::OK == w.write_row(f, n, &err), "row");
  CHECK(Status::OK == w.close(&err), "close");
  const std::string content = read_all(path);
  CHECK("1,NULL\n" == content, "null string content: [" + content + "]");
  std::remove(path.c_str());
}

static void test_abort_removes_file()
{
  const std::string path = "copy_test_abort.csv";
  std::remove(path.c_str());
  CopyOptions opts;
  Error err;
  CopyWriter w;
  CHECK(Status::OK == w.open(path, opts, &err), "open");
  std::vector<std::string> f = {"1"};
  std::vector<char> n = {0};
  CHECK(Status::OK == w.write_row(f, n, &err), "row");
  w.abort();
  CHECK(!file_present(path), "the file must be removed after an abort");
  std::remove(path.c_str());
}

static void test_custom_delimiter_and_quote()
{
  const std::string path = "copy_test_delim.csv";
  std::remove(path.c_str());
  CopyOptions opts;
  opts.delimiter_ = '\t';
  opts.quote_ = '\'';
  opts.escape_ = '\'';
  Error err;
  CopyWriter w;
  CHECK(Status::OK == w.open(path, opts, &err), "open");
  std::vector<std::string> f = {"a", "b'c"};
  std::vector<char> n = {0, 0};
  CHECK(Status::OK == w.write_row(f, n, &err), "row");
  CHECK(Status::OK == w.close(&err), "close");
  const std::string content = read_all(path);
  CHECK("a\t'b''c'\n" == content, "delimiter content: [" + content + "]");
  std::remove(path.c_str());
}

static void test_empty_string_is_not_null()
{
  const std::string path = "copy_test_emptystr.csv";
  std::remove(path.c_str());
  CopyOptions opts;
  Error err;
  CopyWriter w;
  CHECK(Status::OK == w.open(path, opts, &err), "open");
  std::vector<std::string> f = {"", "x"};
  std::vector<char> n = {0, 0};
  CHECK(Status::OK == w.write_row(f, n, &err), "row");
  CHECK(Status::OK == w.close(&err), "close");
  const std::string content = read_all(path);
  CHECK(",x\n" == content, "empty string content: [" + content + "]");
  std::remove(path.c_str());
}

int main()
{
  test_basic();
  test_existing_file();
  test_quoting();
  test_null_string();
  test_abort_removes_file();
  test_custom_delimiter_and_quote();
  test_empty_string_is_not_null();
  if (0 == failures) {
    std::cout << "ALL TESTS PASSED" << std::endl;
    return 0;
  }
  std::cout << failures << " TEST(S) FAILED" << std::endl;
  return 1;
}
