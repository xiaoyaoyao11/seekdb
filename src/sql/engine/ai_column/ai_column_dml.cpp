#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/ob_sql_context.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "common/rowkey/ob_rowkey.h"
#include "common/sql_mode/ob_sql_mode_utils.h"
#include "common/mysqlclient/ob_mysql_result.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') ret = sql.append("``");
    else ret = sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) ret = sql.append("`");
  return ret;
}

// SQL byte literals cannot be affected by connection escaping modes, quotes,
// NULs or character-set conversion of the SQL text itself.
int bytes_literal(ObSqlString &sql, const ObString &value)
{
  static const char digits[] = "0123456789abcdef";
  int ret = sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = value.ptr()[i];
    char hex[2] = {digits[c >> 4], digits[c & 15]};
    ret = sql.append(hex, 2);
  }
  if (OB_SUCC(ret)) ret = sql.append("'");
  return ret;
}

int text_literal(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("CONVERT(");
  if (OB_SUCC(ret)) ret = bytes_literal(sql, value);
  if (OB_SUCC(ret)) ret = sql.append(" USING utf8mb4)");
  return ret;
}

int column_index(const ObDMLBaseCtDef &ctdef, uint64_t id, int64_t &idx)
{
  idx = -1;
  for (int64_t i = 0; idx < 0 && i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == id) idx = i;
  }
  return idx < 0 ? OB_ERR_UNEXPECTED : OB_SUCCESS;
}

int changed(const ObDMLBaseCtDef &ctdef, uint64_t id, ObEvalCtx &ctx, bool &value)
{
  int ret = OB_SUCCESS;
  int64_t idx = -1;
  value = false;
  // Non-projected/non-assigned dependencies are unchanged, not SQL NULL.
  for (int64_t i = 0; idx < 0 && i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == id) idx = i;
  }
  if (idx >= 0) {
    if (idx >= ctdef.old_row_.count() || idx >= ctdef.new_row_.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (ctdef.old_row_.at(idx) != ctdef.new_row_.at(idx)) {
      ObDatum *old_value = nullptr, *new_value = nullptr;
      if (OB_FAIL(ctdef.old_row_.at(idx)->eval(ctx, old_value))) {}
      else if (OB_FAIL(ctdef.new_row_.at(idx)->eval(ctx, new_value))) {}
      else value = !ObDatum::binary_equal(*old_value, *new_value);
    }
  }
  return ret;
}

// Inspect the native evaluated result before INSERT's LONGTEXT destination
// coerces it. Never infer type from JSON-looking bytes or expression spelling.
// SQL NULL has no prompt to render, so its runtime NULL kind is harmless.
int input_is_json(ObTableModifyOp &op, const AiColumnDefinition &d,
                  const ObSqlString &target, const ObSqlString &predicate, bool &is_json)
{
  ObSqlString sql;
  ObMySQLProxy::MySQLResult result;
  ObObj value;
  int ret = sql.append_fmt("SELECT /*+ USE_PLAN_CACHE(NONE) */ (%.*s) FROM %s WHERE %s",
      d.input_expr.length(), d.input_expr.ptr(), target.ptr(), predicate.ptr());
  is_json = false;
  if (OB_SUCC(ret)) ret = op.execute_read(sql.ptr(), result);
  if (OB_SUCC(ret) && result.get_result() == nullptr) ret = OB_ERR_UNEXPECTED;
  if (OB_SUCC(ret)) ret = result.get_result()->next();
  if (OB_SUCC(ret)) ret = result.get_result()->get_obj(static_cast<int64_t>(0), value);
  if (OB_SUCC(ret)) is_json = value.get_type() == ObJsonType;
  const int close_ret = result.close();
  return OB_SUCC(ret) ? close_ret : ret;
}

int result_spec(ObSqlString &sql, const AiColumnDefinition &d, const ObTableSchema &table,
                bool input_json)
{
  const ObColumnSchemaV2 *col = table.get_column_schema(d.column_id);
  int ret = col == nullptr ? OB_ERR_UNEXPECTED : OB_SUCCESS;
  if (OB_SUCC(ret)) ret = sql.append_fmt(
      "JSON_OBJECT('version',2,'input_kind','%s','definition_version',%ld,'column_id',%lu,"
      "'state_column_id',%lu,'request_column_id',%lu,'result_spec_column_id',%lu,"
      "'embedding',%s,'vector_dimension',%ld,'max_retries',%ld,'data_type',%d,'model_name',",
      input_json ? "json" : "text", d.definition_version, d.column_id, d.state_column_id, d.request_column_id,
      d.result_spec_column_id, d.embedding ? "true" : "false", d.vector_dimension,
      d.max_retries, static_cast<int>(col->get_data_type()));
  if (OB_SUCC(ret)) ret = text_literal(sql, d.model_name);
  if (OB_SUCC(ret)) ret = sql.append(",'config_expr',");
  if (OB_SUCC(ret)) ret = text_literal(sql, d.config_expr);
  if (OB_SUCC(ret)) ret = sql.append(",'column_name',");
  if (OB_SUCC(ret)) ret = text_literal(sql, d.column_name);
  if (OB_SUCC(ret)) ret = sql.append(")");
  return ret;
}

int delete_tasks(ObTableModifyOp &op, uint64_t table_id, const ObString &key,
                 const ObIArray<AiColumnDefinition> &defs, const ObIArray<int64_t> &affected)
{
  ObSqlString sql;
  int ret = sql.append_fmt("DELETE /*+ USE_PLAN_CACHE(NONE) */ FROM oceanbase.__all_ai_column_task "
                          "WHERE table_id=%lu AND `rowkey`=", table_id);
  if (OB_SUCC(ret)) ret = bytes_literal(sql, key);
  if (OB_SUCC(ret)) ret = sql.append(" AND column_id IN (");
  for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
    ret = sql.append_fmt("%s%lu", i ? "," : "", defs.at(affected.at(i)).column_id);
  }
  if (OB_SUCC(ret)) ret = sql.append(")");
  if (OB_SUCC(ret) && OB_FAIL(op.execute_write(sql.ptr()))) {
    LOG_WARN("failed to delete obsolete AI COLUMN tasks", K(ret), K(table_id));
  }
  return ret;
}
} // namespace

int AiColumnDml::row_identity(const ObTableSchema &table, const ObDMLBaseCtDef &ctdef,
                             const ExprFixedArray &row, ObEvalCtx &eval_ctx,
                             ObIAllocator &allocator, ObString &binary, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 4> cells;
  const ObRowkeyInfo &info = table.get_rowkey_info();
  if (info.get_size() <= 0) ret = OB_NOT_SUPPORTED;
  for (int64_t i = 0; OB_SUCC(ret) && i < info.get_size(); ++i) {
    const ObRowkeyColumn *key = info.get_column(i);
    int64_t idx = -1;
    ObDatum *datum = nullptr;
    ObObj obj;
    const ObColumnSchemaV2 *col = key == nullptr ? nullptr : table.get_column_schema(key->column_id_);
    if (col == nullptr) ret = OB_ERR_UNEXPECTED;
    else if (OB_FAIL(column_index(ctdef, key->column_id_, idx))) {}
    else if (idx >= row.count()) ret = OB_ERR_UNEXPECTED;
    else if (OB_FAIL(row.at(idx)->eval(eval_ctx, datum))) {}
    else if (OB_FAIL(datum->to_obj(obj, row.at(idx)->obj_meta_))) {}
    else if (OB_FAIL(cells.push_back(obj))) {}
    else {
      char *buf = nullptr;
      int64_t len = 0, pos = 0;
      ObObjPrintParams params = CREATE_OBJ_PRINT_PARAM(eval_ctx.exec_ctx_.get_my_session());
      params.need_cast_expr_ = true;
      GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES,
          eval_ctx.exec_ctx_.get_my_session()->get_sql_mode(), params.skip_escape_);
      if (i && OB_FAIL(predicate.append(" AND "))) {}
      else if (OB_FAIL(identifier(predicate, col->get_column_name_str()))) {}
      else if (OB_FAIL(predicate.append(obj.is_null() ? " IS " : " = "))) {}
      else if (OB_FAIL(obj.print_sql_literal(buf, len, pos, allocator, params))) {}
      else ret = predicate.append(buf, pos);
    }
  }
  if (OB_SUCC(ret)) {
    ObRowkey rowkey(cells.get_data(), cells.count());
    const int64_t size = rowkey.get_serialize_size();
    char *buf = static_cast<char *>(allocator.alloc(size));
    int64_t pos = 0;
    if (buf == nullptr) ret = OB_ALLOCATE_MEMORY_FAILED;
    else if (size > OB_MAX_USER_ROW_KEY_LENGTH) ret = OB_SIZE_OVERFLOW;
    else if (OB_FAIL(rowkey.serialize(buf, size, pos))) {}
    else binary.assign_ptr(buf, pos);
  }
  return ret;
}

int AiColumnDml::after_row(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event)
{
  int ret = OB_SUCCESS;
  ObExecContext &ctx = op.get_exec_ctx();
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (!ctdef.ai_column_dml_ || !ctdef.is_primary_index_
      || session == nullptr || session->is_ai_column_internal_write()) return OB_SUCCESS;
  if (event != ObDmlEventType::DE_INSERTING && event != ObDmlEventType::DE_UPDATING
      && event != ObDmlEventType::DE_DELETING) return OB_SUCCESS;
  const bool inserting = event == ObDmlEventType::DE_INSERTING;
  const bool deleting = event == ObDmlEventType::DE_DELETING;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSEArray<AiColumnDefinition, 4> defs;
  ObSEArray<int64_t, 4> affected;
  ObIAllocator &allocator = ctx.get_allocator();
  const uint64_t table_id = ctdef.das_base_ctdef_.index_tid_;
  if (ctx.get_sql_ctx() == nullptr || ctx.get_sql_ctx()->schema_guard_ == nullptr) return OB_ERR_UNEXPECTED;
  ObSchemaGetterGuard &guard = *ctx.get_sql_ctx()->schema_guard_;
  if (OB_FAIL(guard.get_table_schema(table_id, table))) {}
  else if (table == nullptr) ret = OB_TABLE_NOT_EXIST;
  else if (OB_FAIL(list_ai_column_definitions(*table, allocator, defs))) {}
  bool pk_changed = false;
  for (int64_t i = 0; OB_SUCC(ret) && !inserting && !deleting && i < table->get_rowkey_info().get_size(); ++i) {
    bool one = false;
    ret = changed(ctdef, table->get_rowkey_info().get_column(i)->column_id_, op.get_eval_ctx(), one);
    pk_changed |= one;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
    bool needs = inserting || deleting || pk_changed;
    // Native RETRY changes only this definition's protected request helper.
    // A business SQL cannot assign it; unrelated AI columns remain untouched.
    if (!needs) {
      ret = changed(ctdef, defs.at(i).request_column_id, op.get_eval_ctx(), needs);
    }
    for (int64_t j = 0; OB_SUCC(ret) && !needs && j < defs.at(i).dependency_ids.count(); ++j) {
      ret = changed(ctdef, defs.at(i).dependency_ids.at(j), op.get_eval_ctx(), needs);
    }
    if (OB_SUCC(ret) && needs) ret = affected.push_back(i);
  }
  if (OB_FAIL(ret) || affected.empty()) return ret;
  ObString old_key, new_key;
  ObSqlString old_predicate, new_predicate, target;
  if (!inserting) ret = row_identity(*table, ctdef, ctdef.old_row_, op.get_eval_ctx(), allocator, old_key, old_predicate);
  if (OB_SUCC(ret) && !deleting) ret = row_identity(*table, ctdef, ctdef.new_row_, op.get_eval_ctx(), allocator, new_key, new_predicate);
  if (OB_SUCC(ret)) ret = guard.get_database_schema(table->get_database_id(), database);
  if (OB_SUCC(ret) && database == nullptr) ret = OB_ERR_UNEXPECTED;
  if (OB_SUCC(ret)) ret = identifier(target, database->get_database_name_str());
  if (OB_SUCC(ret)) ret = target.append(".");
  if (OB_SUCC(ret)) ret = identifier(target, table->get_table_name_str());
  if (OB_FAIL(ret)) {
    LOG_WARN("failed to prepare AI COLUMN row identity", K(ret), K(table_id));
    return ret;
  }
  if (OB_FAIL(op.open_inner_conn())) {
    LOG_WARN("failed to open AI COLUMN caller connection", K(ret), K(table_id));
    return ret;
  }
  if (OB_FAIL(op.begin_nested_session(true))) {
    LOG_WARN("failed to begin AI COLUMN nested session", K(ret), K(table_id));
    return ret;
  }
  {
    AiColumnInternalWriteGuard internal(*session);
    if (!inserting) ret = delete_tasks(op, table_id, old_key, defs, affected);
    if (OB_SUCC(ret) && !deleting) ret = delete_tasks(op, table_id, new_key, defs, affected);
    ObString request;
    if (OB_SUCC(ret) && !deleting) {
      ObMySQLProxy::MySQLResult result;
      ObString borrowed;
      if (OB_FAIL(op.execute_read("SELECT /*+ USE_PLAN_CACHE(NONE) */ UUID() AS request_id", result))) {}
      else if (result.get_result() == nullptr) ret = OB_ERR_UNEXPECTED;
      else if (OB_FAIL(result.get_result()->next())) {}
      else if (OB_FAIL(result.get_result()->get_varchar("request_id", borrowed))) {}
      else ret = ob_write_string(allocator, borrowed, request);
    }
    ObSqlString membership;
    if (OB_SUCC(ret)) ret = membership.append("[");
    for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
      ret = membership.append_fmt("%s%lu", i ? "," : "", defs.at(affected.at(i)).column_id);
    }
    if (OB_SUCC(ret)) ret = membership.append("]");
    ObSEArray<bool, 4> input_kinds;
    for (int64_t i = 0; OB_SUCC(ret) && !deleting && i < affected.count(); ++i) {
      const AiColumnDefinition &d = defs.at(affected.at(i));
      ObSqlString spec, sql;
      bool input_json = false;
      if (OB_FAIL(input_is_json(op, d, target, new_predicate, input_json))) {}
      else if (OB_FAIL(input_kinds.push_back(input_json))) {}
      else if (OB_FAIL(result_spec(spec, d, *table, input_json))) {}
      else if (OB_FAIL(sql.append_fmt("INSERT /*+ USE_PLAN_CACHE(NONE) */ INTO oceanbase.__all_ai_column_task "
          "(table_id,column_id,`rowkey`,request_id,rowkey_predicate,definition_version,group_id,group_membership,"
          "input,result_spec,state,retry_count,eligible_ts,lease_expiry,error_code) SELECT %lu,%lu,", table_id, d.column_id))) {}
      else if (OB_FAIL(bytes_literal(sql, new_key))) {}
      else if (OB_FAIL(sql.append(","))) {}
      else if (OB_FAIL(bytes_literal(sql, request))) {}
      else if (OB_FAIL(sql.append(","))) {}
      else if (OB_FAIL(text_literal(sql, new_predicate.string()))) {}
      else if (OB_FAIL(sql.append_fmt(",%ld,", d.definition_version))) {}
      else if (OB_FAIL(bytes_literal(sql, request))) {}
      else if (OB_FAIL(sql.append(","))) {}
      else if (OB_FAIL(text_literal(sql, membership.string()))) {}
      else if (OB_FAIL(sql.append_fmt(",(%.*s),%s,0,0,NOW(6),0,0 FROM %s WHERE %s",
          d.input_expr.length(), d.input_expr.ptr(), spec.ptr(), target.ptr(), new_predicate.ptr()))) {}
      else if (OB_FAIL(op.execute_write(sql.ptr()))) {
        LOG_WARN("failed to enqueue AI COLUMN task", K(ret), K(table_id), K(d.column_id));
      }
    }
    // The ordinary UPDATE path invalidates all secondary/vector indexes. Never
    // mutate storage cells directly or publish a result outside this transaction.
    if (OB_SUCC(ret) && !deleting) {
      ObSqlString sql;
      ret = sql.append_fmt("UPDATE /*+ USE_PLAN_CACHE(NONE) */ %s SET ", target.ptr());
      for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
        const AiColumnDefinition &d = defs.at(affected.at(i));
        if (i) ret = sql.append(",");
        if (OB_SUCC(ret)) ret = identifier(sql, d.column_name);
        if (OB_SUCC(ret)) ret = sql.append("=NULL,");
        const ObColumnSchemaV2 *state = table->get_column_schema(d.state_column_id);
        const ObColumnSchemaV2 *req = table->get_column_schema(d.request_column_id);
        const ObColumnSchemaV2 *spec_col = table->get_column_schema(d.result_spec_column_id);
        if (OB_SUCC(ret) && (state == nullptr || req == nullptr || spec_col == nullptr)) ret = OB_ERR_UNEXPECTED;
        if (OB_SUCC(ret)) ret = identifier(sql, state->get_column_name_str());
        if (OB_SUCC(ret)) ret = sql.append("=0,");
        if (OB_SUCC(ret)) ret = identifier(sql, req->get_column_name_str());
        if (OB_SUCC(ret)) ret = sql.append("=");
        if (OB_SUCC(ret)) ret = bytes_literal(sql, request);
        if (OB_SUCC(ret)) ret = sql.append(",");
        if (OB_SUCC(ret)) ret = identifier(sql, spec_col->get_column_name_str());
        if (OB_SUCC(ret)) ret = sql.append("=");
        if (OB_SUCC(ret)) ret = result_spec(sql, d, *table, input_kinds.at(i));
      }
      if (OB_SUCC(ret)) ret = sql.append_fmt(" WHERE %s", new_predicate.ptr());
      if (OB_SUCC(ret) && OB_FAIL(op.execute_write(sql.ptr()))) {
        LOG_WARN("failed to initialize AI COLUMN result helpers", K(ret), K(table_id));
      }
    }
  }
  const int end_ret = op.end_nested_session();
  if (OB_SUCCESS != end_ret) {
    LOG_WARN("failed to end AI COLUMN nested session", K(ret), K(end_ret), K(table_id));
  }
  if (OB_SUCC(ret)) ret = end_ret;
  return ret;
}
}} // namespace oceanbase::sql
