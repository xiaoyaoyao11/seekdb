#include "sql/engine/basic/file_sql_reader.h"
#include "lib/ob_errno.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <filesystem>
using namespace oceanbase::sql;
using namespace oceanbase::common;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " #x " failed\n"; std::exit(1); } } while (0)
static std::string root;
static void put(const std::string &name, const std::string &text) {
  std::ofstream f(root + "/" + name, std::ios::binary); f << text; CHECK(f.good());
}
static void expect_bad(FileSqlReader &r,const std::string &text,const std::string &format,const std::string &message="") {
  FileSqlOptions o; o.format=format; FileSqlSchema s; put("reject",text);
  int ret=r.infer(root+"/reject",o,root,s);
  if(ret==OB_SUCCESS || (!message.empty() && r.error().find(message)==std::string::npos))
    std::cerr << "expected rejection " << message << ", got " << ret << " " << r.error() << "\n";
  CHECK(ret!=OB_SUCCESS && !r.error().empty());
  CHECK(message.empty() || r.error().find(message)!=std::string::npos);
}
static void csv_features() {
  FileSqlReader r; FileSqlOptions o; o.format="csv"; FileSqlSchema s; FileSqlRow row;
  put("features.csv","id,name,score,active,day,created,note,empty,n\r\n-9223372036854775808,中文,2,TRUE,2024-02-29,2024-02-29T23:59:59.123456,\"hello,\r\nworld\",,NULL\r\n9223372036854775807,Bob,2.5,false,,,\"say \"\"hi\"\"\",\"\",NULL");
  CHECK(r.infer(root+"/features.csv",o,root,s)==OB_SUCCESS && s.row_count==2);
  CHECK(s.columns[0].type==FileSqlType::BIGINT && s.columns[2].type==FileSqlType::DOUBLE);
  CHECK(s.columns[3].type==FileSqlType::BOOLEAN && s.columns[4].type==FileSqlType::DATE && s.columns[5].type==FileSqlType::DATETIME);
  CHECK(s.columns[4].nullable && s.columns[7].nullable && s.columns[8].type==FileSqlType::NULL_VALUE);
  CHECK(r.open(root+"/features.csv",o,root,s,{0,1,2,3,4,5,6,7,8})==OB_SUCCESS);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[0].text=="-9223372036854775808" && row.cells[1].text=="中文");
  CHECK(row.cells[3].text=="true" && row.cells[5].text=="2024-02-29 23:59:59.123456" && row.cells[6].text=="hello,\nworld");
  CHECK(row.cells[7].is_null && row.cells[8].is_null);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[6].text=="say \"hi\"" && !row.cells[7].is_null && row.cells[7].text.empty());
  CHECK(r.next(row)==OB_ITER_END); r.close();
  o.header=false; o.delimiter='|'; o.quote='\''; o.escape='\\'; o.nullstr="nil";
  put("custom.csv","1|'a\\'b'|nil\n2|'x|y'|3\n");
  CHECK(r.infer(root+"/custom.csv",o,root,s)==OB_SUCCESS && s.row_count==2 && s.columns[0].name=="column0");
  CHECK(r.open(root+"/custom.csv",o,root,s,{1,2})==OB_SUCCESS);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[1].text=="a'b" && row.cells[2].is_null);
  CHECK(r.rescan()==OB_SUCCESS && r.next(row)==OB_SUCCESS); r.close();
  expect_bad(r,"a,b\n1\n","csv","width");
  expect_bad(r,"a\n\"unclosed\n","csv","unterminated");
  expect_bad(r,"a\n\"ok\"bad\n","csv","closing quote");
  expect_bad(r,"a\nx\"y\n","csv","unquoted");
  expect_bad(r,"a\rx\r","csv","bare CR");
  expect_bad(r,std::string("a\n\xc0\xaf\n",5),"csv","UTF-8");
  expect_bad(r,std::string("a\nx\0y\n",6),"csv","NUL");
  for(const std::string value:{"9223372036854775808","-9223372036854775809","1e309","NaN","Infinity","2023-02-29","2024-13-01","0000-01-01","2024-01-01T24:00:00","2024-01-01T12:60:00","2024-01-01T12:00:60","2024-01-01T12:00:00.1234567"})
    expect_bad(r,"x\n"+value+"\n","csv","scalar");
  o=FileSqlOptions(); o.format="csv"; o.sample_size=10;
  CHECK(r.infer(root+"/features.csv",o,root,s)==OB_NOT_SUPPORTED);
  o.sample_size=-1; o.delimiter='"'; CHECK(r.infer(root+"/features.csv",o,root,s)==OB_INVALID_ARGUMENT);
  o=FileSqlOptions(); o.format="csv";
  put("bom.csv",std::string("\xef\xbb\xbf")+"id\n1\n");
  CHECK(r.infer(root+"/bom.csv",o,root,s)==OB_SUCCESS && s.columns[0].name=="id");
}
static void json_features() {
  FileSqlReader r; FileSqlOptions o; o.format="ndjson"; FileSqlSchema s; FileSqlRow row;
  put("json-values","{\"n\":-9223372036854775808,\"s\":\"123\",\"u\":\"\\uD83D\\uDE03\"}\n{\"n\":9223372036854775807,\"s\":\"true\",\"d\":2e2}\n");
  CHECK(r.infer(root+"/json-values",o,root,s)==OB_SUCCESS && s.row_count==2);
  CHECK(s.columns[1].type==FileSqlType::VARCHAR && s.columns[3].type==FileSqlType::DOUBLE);
  CHECK(r.open(root+"/json-values",o,root,s,{1,2})==OB_SUCCESS);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[1].text=="123" && row.cells[2].text=="😃");
  CHECK(r.next(row)==OB_SUCCESS && row.cells[1].text=="true" && row.cells[2].is_null);
  CHECK(r.next(row)==OB_ITER_END); r.close();
  expect_bad(r,"{\"a\":1}\n{\"a\":\"1\"}\n","json","conflicting");
  expect_bad(r,"{\"a\":1}\n{\"a\":true}\n","json","conflicting");
  expect_bad(r,"{\"a\":[]}\n","json","nested");
  expect_bad(r,"{\"a\":{}}\n","json","nested");
  expect_bad(r,"[1]\n","json"); expect_bad(r,"1\n","json");
  expect_bad(r,"{\"a\":1,\"a\":2}\n","json","duplicate");
  expect_bad(r,"{\"a\":1}\n{\"A\":2}\n","json","case-conflicting");
  expect_bad(r,"{\"a\":9223372036854775808}\n","json","range");
  expect_bad(r,"{\"a\":1e309}\n","json","range");
  expect_bad(r,"{\"a\":\"\\uD800\"}\n","json","syntax");
  expect_bad(r,"{\"a\":1} trailing\n","json","syntax");
  expect_bad(r,"{\"a\":01}\n","json","syntax");
  expect_bad(r,"{\"a\":1,}\n","json","syntax");
}
int main() {
  char tmp[] = "/tmp/file-reader-native-XXXXXX";
  char *p = mkdtemp(tmp); CHECK(p != nullptr); root=p;
  put("basic.csv", "id,name,id\r\n1,Alice,2\r\n3,Bob,4\n");
  FileSqlReader r; FileSqlOptions o; o.format="csv"; FileSqlSchema s;
  int ret=r.infer(root+"/basic.csv", o, root, s);
  if (ret) std::cerr << r.error() << " (" << ret << ")\n";
  CHECK(ret == OB_SUCCESS);
  CHECK(s.row_count==2 && s.columns.size()==3);
  CHECK(s.columns[0].type==FileSqlType::BIGINT && s.columns[2].name=="id__2");
  CHECK(r.open(root+"/basic.csv",o,root,s,{2})==OB_SUCCESS);
  FileSqlRow row; CHECK(r.next(row)==OB_SUCCESS);
  CHECK(row.cells.size()==3 && row.cells[0].is_null && row.cells[2].text=="2");
  CHECK(r.next(row)==OB_SUCCESS && row.cells[2].text=="4");
  CHECK(r.next(row)==OB_ITER_END);
  CHECK(r.rescan()==OB_SUCCESS && r.next(row)==OB_SUCCESS && row.cells[2].text=="2");
  r.close();
  put("union.jsonl", "{\"id\":1,\"name\":\"中文\",\"score\":1.5,\"active\":true}\r\n\n{\"id\":2,\"day\":\"2024-02-29\",\"active\":false}\n{\"id\":3,\"name\":null,\"score\":2}\n");
  o.format="json";
  ret=r.infer(root+"/union.jsonl",o,root,s);
  if(ret) std::cerr << r.error() << " (" << ret << ")\n";
  CHECK(ret==OB_SUCCESS);
  CHECK(s.row_count==3 && s.columns.size()==5 && s.columns[4].name=="day");
  CHECK(s.columns[2].type==FileSqlType::DOUBLE && s.columns[4].type==FileSqlType::DATE);
  CHECK(s.columns[1].nullable && s.columns[4].nullable && !s.columns[0].nullable);
  CHECK(r.open(root+"/union.jsonl",o,root,s,{0,1,4})==OB_SUCCESS);
  CHECK(r.next(row)==OB_SUCCESS && row.cells.size()==5 && row.cells[1].text=="中文" && row.cells[4].is_null);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[1].is_null && row.cells[4].text=="2024-02-29");
  CHECK(r.next(row)==OB_SUCCESS && row.cells[1].is_null);
  CHECK(r.next(row)==OB_ITER_END && r.rescan()==OB_SUCCESS && r.next(row)==OB_SUCCESS);
  r.close();
  CHECK(::mkdir((root+"/g").c_str(),0700)==0);
  CHECK(::mkdir((root+"/g/sub").c_str(),0700)==0);
  put("g/z.csv","x\n2\n"); put("g/a.csv","x\n1\n"); put("g/sub/q.csv","x\n3\n");
  o.format="glob";
  ret=r.infer(root+"/g/*.csv",o,root,s);
  if(ret) std::cerr << r.error() << " (" << ret << ")\n";
  CHECK(ret==OB_SUCCESS && s.row_count==2 && s.columns.size()==1 && s.columns[0].name=="file");
  CHECK(r.open(root+"/g/*.csv",o,root,s,{0})==OB_SUCCESS);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[0].text==root+"/g/a.csv");
  CHECK(r.next(row)==OB_SUCCESS && row.cells[0].text==root+"/g/z.csv");
  CHECK(r.next(row)==OB_ITER_END && r.rescan()==OB_SUCCESS);
  put("g/new.csv","x\n4\n"); CHECK(r.next(row)!=OB_SUCCESS && !r.error().empty()); r.close();
  CHECK(r.infer(root+"/g/**/*.csv",o,root,s)==OB_SUCCESS && s.row_count==4);
  CHECK(r.open(root+"/g/**/*.csv",o,root,s,{0})==OB_SUCCESS);
  CHECK(r.next(row)==OB_SUCCESS && row.cells[0].text==root+"/g/a.csv");
  r.close();
  CHECK(r.infer(root+"/g/nothing*",o,root,s)==OB_SUCCESS && s.row_count==0);
  CHECK(r.open(root+"/g/nothing*",o,root,s,{0})==OB_SUCCESS && r.next(row)==OB_ITER_END);
  r.close();
  unlink((root+"/g/a.csv").c_str()); unlink((root+"/g/z.csv").c_str());
  unlink((root+"/g/sub/q.csv").c_str()); unlink((root+"/g/new.csv").c_str());
  rmdir((root+"/g/sub").c_str()); rmdir((root+"/g").c_str());
  o=FileSqlOptions(); o.format="csv";
  CHECK(r.infer(root+"/basic.csv",o,"",s)!=OB_SUCCESS);
  CHECK(r.infer(root+"/basic.csv",o,"NULL",s)!=OB_SUCCESS);
  CHECK(r.infer("/etc/passwd",o,root,s)!=OB_SUCCESS);
  CHECK(r.infer(root+"/../basic.csv",o,root,s)!=OB_SUCCESS);
  CHECK(r.infer(root,o,root,s)!=OB_SUCCESS);
  CHECK(::symlink((root+"/basic.csv").c_str(),(root+"/link.csv").c_str())==0);
  CHECK(r.infer(root+"/link.csv",o,root,s)!=OB_SUCCESS);
  CHECK(::symlink(root.c_str(),(root+"-link").c_str())==0);
  CHECK(r.infer(root+"/basic.csv",o,root+"-link/",s)!=OB_SUCCESS);
  unlink((root+"-link").c_str()); unlink((root+"/link.csv").c_str());
  CHECK(::mkfifo((root+"/fifo").c_str(),0600)==0);
  CHECK(r.infer(root+"/fifo",o,root,s)!=OB_SUCCESS); unlink((root+"/fifo").c_str());
  CHECK(r.infer(root+"/basic.csv",o,root,s)==OB_SUCCESS);
  put("basic.csv","id,name,id\n1,Alice,2\n");
  CHECK(r.open(root+"/basic.csv",o,root,s,{0})!=OB_SUCCESS);
  CHECK(r.infer(root+"/basic.csv",o,root,s)==OB_SUCCESS);
  CHECK(r.open(root+"/basic.csv",o,root,s,{0})==OB_SUCCESS);
  put("basic.csv","id,name,id\n9,Alice,2\n");
  // Some filesystems reuse a coarse timestamp within one tick. Make the
  // metadata mutation deterministic; the contract fingerprint is not a hash.
  struct timespec changed[2]={{s.fingerprint.mtime_sec+1,0},{s.fingerprint.mtime_sec+1,0}};
  CHECK(::utimensat(AT_FDCWD,(root+"/basic.csv").c_str(),changed,0)==0);
  ret=r.next(row);
  CHECK(ret!=OB_SUCCESS); CHECK(r.rescan()!=OB_SUCCESS); r.close();
  put("headers.csv","NULL,id,id,id__2\n1,2,3,4\n");
  CHECK(r.infer(root+"/headers.csv",o,root,s)==OB_SUCCESS);
  CHECK(s.columns[0].name=="NULL" && s.columns[2].name=="id__2" && s.columns[3].name=="id__2__2");
  put("bad.csv","a,b\n1,2,3\n");
  CHECK(r.infer(root+"/bad.csv",o,root,s)==OB_INVALID_DATA);
  CHECK(r.error().find("record 2")!=std::string::npos && r.error().find("column 3")!=std::string::npos);
  unlink((root+"/headers.csv").c_str()); unlink((root+"/bad.csv").c_str());
  csv_features(); json_features();
  if (const char *fixture = std::getenv("FILE_SQL_PARQUET_FIXTURE")) {
    std::ifstream input(fixture, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    put("public.parquet", bytes);
    FileSqlReader parquet; FileSqlSchema ps; FileSqlRow pr; FileSqlOptions po; po.format="parquet";
    CHECK(parquet.infer(root+"/public.parquet",po,root,ps)==OB_SUCCESS);
    CHECK(ps.row_count==7 && ps.columns.size()==1 && ps.columns[0].type==FileSqlType::BIGINT);
    CHECK(parquet.open(root+"/public.parquet",po,root,ps,{0})==OB_SUCCESS);
    for(int i=0;i<7;++i) CHECK(parquet.next(pr)==OB_SUCCESS && pr.cells[0].text==std::to_string(i));
    CHECK(parquet.next(pr)==OB_ITER_END);
    CHECK(parquet.rescan()==OB_SUCCESS && parquet.next(pr)==OB_SUCCESS && pr.cells[0].text=="0");
  }
  unlink((root+"/union.jsonl").c_str());
  std::filesystem::remove_all(root);
  std::cout << "PASS " << checks << " checks\n";
}
