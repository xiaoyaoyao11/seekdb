#include "sql/parser/ob_parser.h"
#include "lib/allocator/page_arena.h"
#include <cstdio>
using namespace oceanbase;
static int definitions(const ParseNode *node) {
  if(node==nullptr) return 0;
  if(node->type_==T_AI_COLUMN) {
    if(node->num_child_!=2 || node->children_[0]==nullptr ||
       node->children_[0]->str_value_==nullptr || node->children_[0]->str_len_<=0) return -100;
    const ParseNode *options=node->children_[1];
    if(options!=nullptr && options->type_!=T_EXPR_LIST) return -100;
    return 1;
  }
  int count=0;
  for(int i=0;i<node->num_child_;++i) count+=definitions(node->children_[i]);
  return count;
}
int main() {
  common::ObArenaAllocator allocator;
  sql::ObParser parser(allocator,0);
  const char *valid[] = {
    "CREATE TABLE docs(id BIGINT PRIMARY KEY, body VARCHAR(200), tags JSON AI COLUMN (AI_COMPLETE('m',CONCAT('Classify: ',body))) WITH(sync_mode='async',max_retries=2))",
    "ALTER TABLE docs ADD COLUMN emb VECTOR(4) AI COLUMN(AI_EMBED('e',body)) WITH(sync_mode='async',max_retries=3)",
    "CREATE TABLE docs(id BIGINT PRIMARY KEY, body TEXT, answer VARCHAR(500) AI COLUMN(AI_COMPLETE('m',body)))",
    "CREATE TABLE docs(id BIGINT PRIMARY KEY, body TEXT, a JSON AI COLUMN(AI_COMPLETE('m',body)), b BIGINT AI COLUMN(AI_COMPLETE('m',body)) WITH(max_retries=0))",
    "CREATE TABLE ordinary(ai INT, body VARCHAR(20), g INT GENERATED ALWAYS AS (ai+1) STORED)",
    "SELECT ai FROM ordinary"
  };
  int failures=0;
  const int expected_definitions[]={1,1,1,2,0,0};
  int index=0;
  for(const char *query:valid) {
    ParseResult result;
    const int ret=parser.parse(common::ObString::make_string(query),result);
    const bool ok=ret==0 && definitions(result.result_tree_)==expected_definitions[index++];
    std::printf("%s ret=%d %s\n",ok?"PASS":"FAIL",ret,query);
    failures+=!ok;
    parser.free_result(result);
  }
  return failures?1:0;
}
