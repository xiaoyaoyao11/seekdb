#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/file_sql_resolver.h"
#include "lib/allocator/page_arena.h"
#include <cstdio>
#include <cstring>
using namespace oceanbase;
static bool has_table(const ParseNode *n) {
  if (n == nullptr) return false;
  if (n->type_ == T_TABLE_COLLECTION_EXPRESSION) {
    sql::FileSqlPlan plan;
    if (!sql::file_sql_is_call(n->children_[0]) || sql::file_sql_parse_call(*n->children_[0],plan)!=0) return false;
    plan.schema.columns.push_back({"id",sql::FileSqlType::BIGINT,false});
    plan.schema.columns.push_back({"flag",sql::FileSqlType::BOOLEAN,true});
    plan.schema.fingerprint.inode=123;
    const std::string encoded=plan.encode();
    sql::FileSqlPlan copy;
    if (copy.decode(common::ObString(encoded.size(),encoded.data()))!=0 || copy.encode()!=encoded) return false;
    if (copy.decode(common::ObString(encoded.size()-1,encoded.data()))==0) return false;
    return true;
  }
  for (int i=0;i<n->num_child_;++i) if (has_table(n->children_[i])) return true;
  return false;
}
static bool has_copy(const ParseNode *n) {
  if(n==nullptr)return false;
  if(n->type_==T_INTO_OUTFILE && n->num_child_==2)return true;
  for(int i=0;i<n->num_child_;++i)if(has_copy(n->children_[i]))return true;
  return false;
}
int main() {
  common::ObArenaAllocator alloc;
  sql::ObParser parser(alloc, 0);
  const char *queries[] = {
    "SELECT * FROM read_csv('/allowed/a.csv')",
    "SELECT c.name FROM read_csv('/allowed/a.csv', header=true, delim='|') AS c",
    "SELECT * FROM read_json('/allowed/a.json') j",
    "SELECT * FROM read_json('/allowed/a.jsonl', format='newline_delimited') j",
    "SELECT count(*) FROM read_ndjson('/allowed/a.jsonl') j JOIN read_csv('/allowed/a.csv') c ON j.id=c.id",
    "SELECT * FROM read_parquet('/allowed/a.parquet')",
    "SELECT * FROM glob('/allowed/*.csv')",
    "SELECT * FROM read_csv('/allowed/a.csv', header=false, delimiter='|', quote='!', escape='!', nullstr='NA', sample_size=-1)",
    "SELECT * FROM read_csv('/allowed/a.csv', header=0, sample_size=100)",
    "SELECT * FROM TABLE(read_csv('/allowed/a.csv')) c"
  };
  int failed=0;
  for (auto q : queries) {
    ParseResult result;
    const int ret=parser.parse(common::ObString::make_string(q), result);
    const bool ok=ret==0 && has_table(result.result_tree_);
    std::printf("%s ret=%d %s\n", ok?"PASS":"FAIL", ret, q);
    failed += !ok;
    parser.free_result(result);
  }
  const char *invalid[] = {
    "SELECT * FROM read_csv('/allowed/a.csv', delim=',', delimiter='|')",
    "SELECT * FROM read_csv('/allowed/a.csv', header=2)",
    "SELECT * FROM read_csv('/allowed/a.csv', mystery=true)",
    "SELECT * FROM read_csv('/allowed/a.csv', header=1+1)",
    "SELECT * FROM read_csv(@path)",
    "SELECT * FROM read_csv('/allowed/a.csv', delim='ab')",
    "SELECT * FROM read_csv('/allowed/a.csv', sample_size=0)"
  };
  for (auto q : invalid) {
    ParseResult result;
    const int ret=parser.parse(common::ObString::make_string(q),result);
    const bool rejected=ret!=0 || !has_table(result.result_tree_);
    std::printf("%s reject %s\n",rejected?"PASS":"FAIL",q);
    failed += !rejected;
    parser.free_result(result);
  }
  for (const char *q : {"DESCRIBE SELECT 1", "DESC SELECT 1", "EXPLAIN SELECT 1"}) {
    ParseResult result;
    int ret=parser.parse(common::ObString::make_string(q),result);
    ParseNode *node=ret==0 && result.result_tree_!=nullptr ? result.result_tree_->children_[0] : nullptr;
    bool expected=q[0]!='E';
    bool marker=node!=nullptr && node->num_child_==5 && node->children_[0]!=nullptr && node->children_[0]->type_==T_SHOW_COLUMNS;
    bool ok=node!=nullptr && node->type_==T_EXPLAIN && marker==expected;
    std::printf("%s describe-marker %s root=%d value=%ld child=%d childvalue=%ld\n",ok?"PASS":"FAIL",q,
      result.result_tree_?int(result.result_tree_->type_):-1,result.result_tree_?result.result_tree_->value_:-1,
      node?int(node->type_):-1,node?node->value_:-1);
    failed+=!ok;
    parser.free_result(result);
  }
  for(const char *q : {"COPY (SELECT 1 AS id) TO '/allowed/out.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL')",
      "COPY mytable TO '/allowed/out.csv'", "COPY (SELECT * FROM read_csv('/allowed/in.csv')) TO '/allowed/out.csv' (DELIMITER '|', QUOTE '!', ESCAPE '!')"}) {
    ParseResult result;int ret=parser.parse(common::ObString::make_string(q),result);
    bool ok=ret==0&&has_copy(result.result_tree_);
    std::printf("%s copy ret=%d %s\n",ok?"PASS":"FAIL",ret,q);failed+=!ok;
    parser.free_result(result);
  }
  return failed ? 1 : 0;
}
