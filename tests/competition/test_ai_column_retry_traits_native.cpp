#include "sql/parser/ob_parser.h"
#include "sql/ob_sql_utils.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/engine/ai_column/ai_column_strict_retry.h"
#include "share/schema/ob_table_schema.h"
#include "lib/allocator/page_arena.h"
#include <cstdio>
#include <cstring>
using namespace oceanbase;
int main()
{
  common::ObArenaAllocator allocator;
  sql::ObParser parser(allocator, 0);
  struct Case { const char *query; ObItemType expected; bool retry; bool mixed; };
  const Case cases[] = {
    {"ALTER TABLE docs RETRY AI COLUMN result", T_UPDATE, true, false},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE status='READY' AND id=1", T_UPDATE, true, false},
    {"ALTER TABLE docs RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT", T_UPDATE, true, true},
    {"ALTER TABLE docs ADD COLUMN extra INT, RETRY AI COLUMN result", T_UPDATE, true, true},
    {"ALTER TABLE docs RETRY AI COLUMN result, RETRY AI COLUMN label", T_UPDATE, true, true},
    {"ALTER TABLE docs ADD COLUMN extra INT", T_ALTER_TABLE, false, false},
    {"UPDATE docs SET category='news' WHERE id=1", T_UPDATE, false, false},
    {"SELECT * FROM docs", T_SELECT, false, false}
  };
  int failures = 0;
  for (const auto &c : cases) {
    ParseResult result;
    int ret = parser.parse(common::ObString::make_string(c.query), result);
    ParseNode *root = ret == 0 && result.result_tree_ ? result.result_tree_->children_[0] : nullptr;
    const ObItemType type = sql::ObSQLUtils::get_sql_item_type(result);
    sql::stmt::StmtType resolved_type = sql::stmt::T_NONE;
    bool ok = ret == 0 && root && type == c.expected
        && sql::ObSQLUtils::get_sql_item_type(*root) == type
        && sql::ObResolverUtils::resolve_stmt_type(result, resolved_type) == 0
        && resolved_type == sql::ObResolverUtils::get_stmt_type_by_item_type(type);
    if (c.retry && root) {
      ok = ok && root->type_ == T_ALTER_TABLE
          && !sql::ObSQLUtils::cause_implicit_commit(result)
          && !sql::ObSQLUtils::is_readonly_stmt(result)
          && !sql::ObSQLUtils::is_commit_stmt(result)
          && sql::ObResolverUtils::get_stmt_type_by_item_type(type) == sql::stmt::T_UPDATE;
      if (c.mixed) {
        // Validation must still reject mixed actions before touching session/schema.
        sql::ObResolverParams params;
        share::schema::ObTableSchema table;
        sql::ObDMLStmt *stmt = nullptr;
        ok = sql::AiColumnRetry::resolve(params, *root->children_[1], table, stmt)
              == common::OB_NOT_SUPPORTED && stmt == nullptr && ok;
      }
    }
    printf("%s traits=%d raw=%d implicit_commit=%d %s\n", ok ? "PASS" : "FAIL",
           int(type), root ? int(root->type_) : -1,
           sql::ObSQLUtils::cause_implicit_commit(result), c.query);
    failures += !ok;
    parser.free_result(result);
  }
  return failures ? 1 : 0;
}
