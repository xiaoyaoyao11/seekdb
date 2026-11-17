#define USING_LOG_PREFIX SQL_RESV
#include "sql/engine/ai_column/ai_column_strict_retry.h"
#include "sql/engine/ai_column/ai_column_dml.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "sql/parser/ob_parser.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int quote_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name.ptr()[i] == '`' ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) ret = sql.append("`");
  return ret;
}
int qualified_table(ObSqlString &sql, const ObTableSchema &table, ObSchemaGetterGuard &guard)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  if (OB_FAIL(guard.get_database_schema(table.get_database_id(), database))) {}
  else if (!database) ret = OB_ERR_UNEXPECTED;
  else if (OB_FAIL(quote_identifier(sql, database->get_database_name_str()))) {}
  else if (OB_FAIL(sql.append("."))) {}
  else ret = quote_identifier(sql, table.get_table_name_str());
  return ret;
}
ParseNode *find_node(ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) return node;
  ParseNode *found = nullptr;
  for (int i = 0; !found && i < node->num_child_; ++i) found = find_node(node->children_[i], type);
  return found;
}
int unfinished(ObSQLSessionInfo &session, const ObSqlString &sql, bool &found)
{
  int ret = OB_SUCCESS;
  found = false;
  sqlclient::ObISQLConnectionGuard owned;
  sqlclient::ObISQLConnection *connection = session.get_inner_conn();
  if (!connection) ret = query::ObInnerSQLConnectionAccess::create_connection_with_external_session(&session, owned);
  if (OB_SUCC(ret) && !connection) connection = owned.get_ptr();
  ObIInnerSQLConnection *native = as_inner_sql_connection(connection);
  ObSQLSessionInfo *native_session = connection ? query::ObInnerSQLConnectionAccess::get_session(connection) : nullptr;
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to open AI COLUMN strict read connection", K(ret));
    return ret;
  }
  // Native nesting must use the caller's session and a real execution context;
  // a resolver context need not have a parent of its own. stmt_query links the
  // probe below it. Do not fabricate a parent or borrow FK/statistics flags.
  if (!native || native_session != &session || !session.get_cur_exec_ctx()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid AI COLUMN strict read session context", K(ret));
    return ret;
  }
  // Resolution precedes ObSqlTransControl::start_stmt: nested_count is still
  // -1, whereas begin_nested_session requires >= 0. Establish only the native
  // statement scope (not a transaction), and balance it before resolution
  // resumes. Execution-time callers already have a scope and must retain it.
  const bool own_stmt = !session.has_start_stmt();
  if (own_stmt && OB_FAIL(session.set_start_stmt())) {
    LOG_WARN("failed to start AI COLUMN strict read statement scope", K(ret));
    return ret;
  }
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  if (OB_FAIL(native->begin_nested_session(saved_session, saved_connection, true))) {
    LOG_WARN("failed to begin AI COLUMN strict read nested session", K(ret));
  } else {
    // This guard must outlive result.close(): closing the probe must not end
    // its parent statement. All reads retain the caller's transaction/snapshot.
    AiColumnInternalWriteGuard internal(*native_session);
    {
      ObMySQLProxy::MySQLResult result;
      if (OB_FAIL(connection->execute_read(sql.ptr(), result))) {
        LOG_WARN("failed to execute AI COLUMN strict read probe", K(ret));
      } else if (!result.get_result()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) ret = OB_SUCCESS;
        else if (OB_SUCC(ret)) found = true;
        else LOG_WARN("failed to fetch AI COLUMN strict read probe", K(ret));
      }
      const int close_ret = result.close();
      if (OB_SUCCESS != close_ret) {
        LOG_WARN("failed to close AI COLUMN strict read probe", K(close_ret));
        if (OB_SUCC(ret)) ret = close_ret;
      }
    }
    const int end_ret = native->end_nested_session(saved_session, saved_connection);
    if (OB_SUCCESS != end_ret) {
      LOG_WARN("failed to end AI COLUMN strict read nested session", K(end_ret));
      if (OB_SUCC(ret)) ret = end_ret;
    }
  }
  if (own_stmt) {
    const int end_ret = session.set_end_stmt();
    if (OB_SUCCESS != end_ret) {
      LOG_WARN("failed to end AI COLUMN strict read statement scope", K(end_ret));
      if (OB_SUCC(ret)) ret = end_ret;
    }
  }
  return ret;
}
}

namespace {
int probe_column(ObSQLSessionInfo &session, ObSchemaGetterGuard &guard,
                 uint64_t table_id, uint64_t column_id)
{
  if (session.is_ai_column_internal_write() || table_id == OB_INVALID_ID || table_id == 0
      || column_id == OB_INVALID_ID) return OB_SUCCESS;
  int ret = OB_SUCCESS;
  const ObTableSchema *table = nullptr;
  if (OB_FAIL(guard.get_table_schema(table_id, table))) return ret;
  if (!table) return OB_SUCCESS; // generated/CTE columns are checked in their own resolver
  const ObColumnSchemaV2 *column = table->get_column_schema(column_id);
  if (!column || !column->is_ai_column()) return OB_SUCCESS;

  ObSqlString sql;
  const std::string state_name = ai_column_helper_name(column->get_column_name_str(), 0);
  const ObColumnSchemaV2 *state = table->get_column_schema(ObString(state_name.size(), state_name.data()));
  if (!state || !state->is_ai_helper_column()) return OB_INVALID_DATA;
  if (OB_FAIL(sql.append("SELECT /*+ USE_PLAN_CACHE(NONE) */ 1 FROM "))) {}
  else if (OB_FAIL(qualified_table(sql, *table, guard))) {}
  else if (OB_FAIL(sql.append(" WHERE "))) {}
  else if (OB_FAIL(quote_identifier(sql, state->get_column_name_str()))) {}
  else if (OB_FAIL(sql.append(" IS NULL OR "))) {}
  else if (OB_FAIL(quote_identifier(sql, state->get_column_name_str()))) {}
  else ret = sql.append(" <> 1 LIMIT 1");
  bool found = false;
  if (OB_SUCC(ret)) ret = unfinished(session, sql, found);
  if (OB_SUCC(ret) && found) {
    ret = OB_AI_COLUMN_UNFINISHED_TASKS;
    LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASKS, table_id, column_id);
  }
  return ret;
}

} // namespace

int AiColumnStrictRead::check_column(ObSQLSessionInfo &session, ObSchemaGetterGuard &guard,
                                    ObDMLStmt &stmt, uint64_t table_id, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (session.is_ai_column_internal_write() || table_id == OB_INVALID_ID || table_id == 0
      || column_id == OB_INVALID_ID) return ret;
  const ObTableSchema *table = nullptr;
  if (OB_FAIL(guard.get_table_schema(table_id, table))) return ret;
  const ObColumnSchemaV2 *column = table ? table->get_column_schema(column_id) : nullptr;
  if (!column || !column->is_ai_column()) return ret;
  ObQueryCtx *query_ctx = stmt.get_query_ctx();
  if (!query_ctx) return OB_ERR_UNEXPECTED;
  // Keep references even with strict OFF. Do not collect UPDATE/DELETE's
  // implicit storage projections here: these are bound user references only.
  query_ctx->get_query_hint_for_update().get_global_hint().merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
  for (int64_t i = 0; i < query_ctx->ai_column_strict_read_refs_.count(); ++i) {
    const auto &ref = query_ctx->ai_column_strict_read_refs_.at(i);
    if (ref.table_id_ == table_id && ref.column_id_ == column_id) return ret;
  }
  const ObQueryCtx::AiColumnStrictReference ref{table_id, column_id};
  return query_ctx->ai_column_strict_read_refs_.push_back(ref);
}

bool AiColumnStrictRead::is_snapshot_probe(ObExecContext &exec_ctx)
{
  ObExecContext *parent = exec_ctx.get_parent_ctx();
  ObSQLSessionInfo *session = exec_ctx.get_my_session();
  return parent && session && parent->get_my_session() == session
      && session->is_ai_column_internal_write() && parent->get_query_ctx()
      && parent->get_query_ctx()->ai_column_strict_probe_active_;
}

int AiColumnStrictRead::check_statement(ObExecContext &exec_ctx)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = exec_ctx.get_my_session();
  ObQueryCtx *query_ctx = exec_ctx.get_query_ctx();
  if (!session) return OB_ERR_UNEXPECTED;
  if (session->is_ai_column_internal_write() || !query_ctx
      || query_ctx->ai_column_strict_read_refs_.empty()) return ret;
  bool enabled = false;
  if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) return ret;
  if (!enabled) return ret;
  ObSqlCtx *sql_ctx = exec_ctx.get_sql_ctx();
  const auto &snapshot = exec_ctx.get_das_ctx().get_snapshot();
  if (!sql_ctx || !sql_ctx->schema_guard_ || !session->has_start_stmt()
      || !snapshot.is_valid() || snapshot.is_none_read()) return OB_ERR_UNEXPECTED;
  // Context-owned scope, not TLS: native execution may yield during SQL.
  // Restore after result.close and end_nested_session have both completed.
  struct ProbeScope
  {
    explicit ProbeScope(ObQueryCtx &ctx) : ctx_(ctx), saved_(ctx.ai_column_strict_probe_active_)
    { ctx_.ai_column_strict_probe_active_ = true; }
    ~ProbeScope() { ctx_.ai_column_strict_probe_active_ = saved_; }
    ObQueryCtx &ctx_;
    bool saved_;
  } scope(*query_ctx);
  for (int64_t i = 0; OB_SUCC(ret) && i < query_ctx->ai_column_strict_read_refs_.count(); ++i) {
    const auto ref = query_ctx->ai_column_strict_read_refs_.at(i);
    ret = probe_column(*session, *sql_ctx->schema_guard_, ref.table_id_, ref.column_id_);
  }
  return ret;
}

int AiColumnStrictRead::check_expr(ObSQLSessionInfo &session, ObSchemaGetterGuard &guard,
                                  ObDMLStmt &stmt, const ObRawExpr &expr)
{
  int ret = OB_SUCCESS;
  if (session.is_ai_column_internal_write()) return ret;
  if (expr.is_column_ref_expr()) {
    const auto &column = static_cast<const ObColumnRefRawExpr &>(expr);
    const ColumnItem *item = stmt.get_column_item_by_id(column.get_table_id(), column.get_column_id());
    if (item) ret = check_column(session, guard, stmt, item->base_tid_, item->base_cid_);
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.get_param_count(); ++i) {
    if (expr.get_param_expr(i)) ret = check_expr(session, guard, stmt, *expr.get_param_expr(i));
  }
  return ret;
}
int AiColumnStrictRead::check_select(ObSQLSessionInfo &session, ObSchemaGetterGuard &guard, ObDMLStmt &stmt)
{
  int ret = OB_SUCCESS;
  // SELECT column items are the bound references (including expanded stars,
  // predicates and ordering), not UPDATE's implicitly projected storage row.
  for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_column_size(); ++i) {
    const ColumnItem *item = stmt.get_column_item(i);
    if (item) ret = check_column(session, guard, stmt, item->base_tid_, item->base_cid_);
  }
  return ret;
}

bool AiColumnRetry::contains_retry(const ParseNode *actions)
{
  if (!actions) return false;
  if (actions->type_ == T_AI_COLUMN_RETRY) return true;
  for (int i = 0; i < actions->num_child_; ++i) if (contains_retry(actions->children_[i])) return true;
  return false;
}
int AiColumnRetry::bind_status(ParseNode *&node, ParseNode *state_case)
{
  if (!node) return OB_SUCCESS;
  if (!state_case) return OB_INVALID_ARGUMENT;
  if (node->type_ == T_COLUMN_REF && node->num_child_ == 3
      && !node->children_[0] && !node->children_[1] && node->children_[2]) {
    const ParseNode &name = *node->children_[2];
    if (!name.is_input_quoted_ && name.str_len_ == 6 && !strncasecmp(name.str_value_, "status", 6)) {
      node = state_case;
      return OB_SUCCESS;
    }
  }
  int ret = OB_SUCCESS;
  for (int i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) ret = bind_status(node->children_[i], state_case);
  return ret;
}

int AiColumnRetry::resolve(ObResolverParams &params, const ParseNode &actions,
                           const ObTableSchema &table, ObDMLStmt *&stmt)
{
  int ret = OB_SUCCESS;
  stmt = nullptr;
  if (actions.num_child_ != 1 || !actions.children_[0] || actions.children_[0]->type_ != T_AI_COLUMN_RETRY) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
    return OB_NOT_SUPPORTED;
  }
  const ParseNode &retry = *actions.children_[0];
  if (retry.num_child_ != 2 || !retry.children_[0] || !params.session_info_ || !params.schema_checker_
      || !params.allocator_) return OB_ERR_UNEXPECTED;
  const ParseNode &name = *retry.children_[0];
  const ObColumnSchemaV2 *column = table.get_column_schema(ObString(name.str_len_, name.str_value_));
  if (!column || !column->is_ai_column()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    return OB_NOT_SUPPORTED;
  }
  ObSEArray<AiColumnDefinition, 4> definitions;
  if (OB_FAIL(list_ai_column_definitions(table, *params.allocator_, definitions))) return ret;
  const AiColumnDefinition *definition = nullptr;
  for (int64_t i = 0; i < definitions.count(); ++i) if (definitions.at(i).column_id == column->get_column_id()) definition = &definitions.at(i);
  if (!definition) return OB_INVALID_DATA;
  const ObColumnSchemaV2 *request = table.get_column_schema(definition->request_column_id);
  const ObColumnSchemaV2 *state = table.get_column_schema(definition->state_column_id);
  if (!request || !state) return OB_INVALID_DATA;
  ObSchemaGetterGuard *guard = params.schema_checker_->get_schema_guard();
  if (!guard) return OB_ERR_UNEXPECTED;
  ObSqlString sql, case_sql;
  if (OB_FAIL(sql.append("UPDATE /*+ USE_PLAN_CACHE(NONE) */ "))) {}
  else if (OB_FAIL(qualified_table(sql, table, *guard))) {}
  else if (OB_FAIL(sql.append(" SET "))) {}
  else if (OB_FAIL(quote_identifier(sql, column->get_column_name_str()))) {}
  else if (OB_FAIL(sql.append("=NULL,"))) {}
  else if (OB_FAIL(quote_identifier(sql, request->get_column_name_str()))) {}
  else ret = sql.append("=UUID()");
  const ParseNode *predicate = retry.children_[1];
  if (OB_SUCC(ret) && predicate) {
    if (!predicate->str_value_ || predicate->str_len_ <= 0) ret = OB_INVALID_DATA;
    else ret = sql.append_fmt(" WHERE %.*s", int(predicate->str_len_), predicate->str_value_);
  }
  if (OB_SUCC(ret)) ret = case_sql.append("SELECT CASE ");
  if (OB_SUCC(ret)) ret = quote_identifier(case_sql, state->get_column_name_str());
  if (OB_SUCC(ret)) ret = case_sql.append(" WHEN 0 THEN 'PENDING' WHEN 1 THEN 'READY' WHEN 2 THEN 'FAILED' END");
  if (OB_FAIL(ret)) return ret;
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode());
  ParseResult update_result, case_result;
  if (OB_FAIL(parser.parse(sql.string(), update_result))) return ret;
  if (OB_SUCC(ret)) ret = parser.parse(case_sql.string(), case_result);
  if (OB_SUCC(ret)) {
    ParseNode *update = find_node(update_result.result_tree_, T_UPDATE);
    ParseNode *where = find_node(update, T_WHERE_CLAUSE);
    ParseNode *state_case = find_node(case_result.result_tree_, T_CASE);
    if (!update || !state_case) ret = OB_ERR_UNEXPECTED;
    else if (where && OB_FAIL(bind_status(where, state_case))) {}
    else {
      AiColumnInternalWriteGuard internal(*params.session_info_);
      ObUpdateResolver resolver(params);
      if (OB_FAIL(resolver.resolve(*update))) {}
      else {
        stmt = resolver.get_update_stmt();
        if (!stmt) ret = OB_ERR_UNEXPECTED;
        else if (stmt->get_query_ctx()) stmt->get_query_ctx()->get_query_hint_for_update().get_global_hint()
            .merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
      }
    }
  }
  parser.free_result(case_result);
  parser.free_result(update_result);
  return ret;
}
}} // namespace oceanbase::sql
