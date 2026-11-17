#pragma once
#include "lib/string/ob_sql_string.h"
#include "sql/parser/parse_node.h"

namespace oceanbase {
namespace share { namespace schema { class ObSchemaGetterGuard; class ObTableSchema; } }
namespace sql {
class ObSQLSessionInfo;
class ObExecContext;
class ObDMLStmt;
class ObRawExpr;
struct ObResolverParams;
// Neither interface evaluates a user predicate or performs autonomous writes.
class AiColumnStrictRead
{
public:
  // Validate bound references after the real statement snapshot is acquired.
  static int check_statement(ObExecContext &exec_ctx);
  // Only our same-session nested readiness probe reuses the complete outer
  // snapshot; the general internal-write capability is insufficient.
  static bool is_snapshot_probe(ObExecContext &exec_ctx);
  static int check_column(ObSQLSessionInfo &session, share::schema::ObSchemaGetterGuard &guard,
                          ObDMLStmt &stmt, uint64_t table_id, uint64_t column_id);
  static int check_expr(ObSQLSessionInfo &session, share::schema::ObSchemaGetterGuard &guard,
                        ObDMLStmt &stmt, const ObRawExpr &expr);
  static int check_select(ObSQLSessionInfo &session, share::schema::ObSchemaGetterGuard &guard,
                          ObDMLStmt &stmt);
};
class AiColumnRetry
{
public:
  static bool contains_retry(const ParseNode *actions);
  // Returns a native UPDATE statement. Only resolution is privileged; execution
  // goes through normal DAS + transactional AiColumnDml::after_row.
  static int resolve(ObResolverParams &params, const ParseNode &actions,
                     const share::schema::ObTableSchema &table, ObDMLStmt *&stmt);
  // AST substitution retains native name binding, expression semantics and quote
  // provenance. Only an unqualified, unquoted STATUS is the retry pseudo column.
  static int bind_status(ParseNode *&node, ParseNode *state_case);
};
}}
