#include "sql/parser/ob_parser.h"
#include "lib/allocator/page_arena.h"
#include "sql/engine/ai_column/ai_column_strict_retry.h"
static int replacements(const ParseNode *node, const ParseNode *replacement) {
  if (!node) return 0;
  if (node == replacement) return 1;
  int n = 0;
  for (int i = 0; i < node->num_child_; ++i) n += replacements(node->children_[i], replacement);
  return n;
}
#include <cstdio>
#include <cstring>
using namespace oceanbase;
int main()
{
  common::ObArenaAllocator allocator;
  sql::ObParser parser(allocator, 0);
  struct Case { const char *sql; int actions; const char *predicate; };
  const Case cases[] = {
    {"ALTER TABLE docs RETRY AI COLUMN result", 1, nullptr},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE status='FAILED' AND category='news'", 1, "status='FAILED' AND category='news'"},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE `status`='active' AND id IN (1,3)", 1, "`status`='active' AND id IN (1,3)"},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE id IN (SELECT id FROM other_docs)", 1, "id IN (SELECT id FROM other_docs)"},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE status='READY' AND `status`='active' AND docs.status='active' AND 'status'='status'", 1, "status='READY' AND `status`='active' AND docs.status='active' AND 'status'='status'"},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT", 2, "id=1"}
  };
  int failures = 0;
  for (const auto &c : cases) {
    ParseResult result;
    int ret = parser.parse(common::ObString::make_string(c.sql), result);
    const ParseNode *root = ret == 0 ? result.result_tree_ : nullptr;
    while (root && root->type_ != T_ALTER_TABLE && root->num_child_ == 1) root = root->children_[0];
    const ParseNode *actions = root && root->type_ == T_ALTER_TABLE && root->num_child_ > 1 ? root->children_[1] : nullptr;
    const ParseNode *action = actions && actions->num_child_ ? actions->children_[0] : nullptr;
    const ParseNode *predicate = action && action->num_child_ == 2 ? action->children_[1] : nullptr;
    bool ok = ret == 0 && actions && actions->num_child_ == c.actions && action
        && action->num_child_ == 2 && action->children_[0]
        && action->children_[0]->str_len_ == 6
        && !memcmp(action->children_[0]->str_value_, "result", 6);
    if (c.predicate) ok = ok && predicate && predicate->str_len_ == strlen(c.predicate)
        && !memcmp(predicate->str_value_, c.predicate, strlen(c.predicate));
    else ok = ok && !predicate;
    ParseNode replacement{};
    replacement.type_ = T_CASE;
    ParseNode *bound = const_cast<ParseNode *>(predicate);
    if (ok && bound) {
      const int expected = c.predicate && !strncmp(c.predicate, "status=", 7) ? 1 : 0;
      ok = sql::AiColumnRetry::bind_status(bound, &replacement) == 0
          && replacements(bound, &replacement) == expected;
    }
    printf("%s ret=%d %s\n", ok ? "PASS" : "FAIL", ret, c.sql);
    failures += !ok;
    parser.free_result(result);
  }
  const char *invalid[] = {"ALTER TABLE docs nonsense AI COLUMN result", "ALTER TABLE docs `retry` AI COLUMN result"};
  for (const char *query : invalid) {
    ParseResult result;
    const int ret = parser.parse(common::ObString::make_string(query), result);
    printf("%s ret=%d reject %s\n", ret != 0 ? "PASS" : "FAIL", ret, query);
    failures += ret == 0;
    parser.free_result(result);
  }
  return failures ? 1 : 0;
}
