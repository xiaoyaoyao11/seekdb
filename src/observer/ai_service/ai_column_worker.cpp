/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ai_service/ai_column_worker_value.h"
#include "observer/ob_inner_sql_connection.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ai_column/ai_column_dml.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "common/rowkey/ob_rowkey.h"
#include <algorithm>

namespace oceanbase { namespace observer {
using namespace common;
using namespace share::schema;
namespace {
const char *TASKS = "oceanbase.__all_ai_column_task";
const char *NOW_US = "CAST(UNIX_TIMESTAMP(NOW(6))*1000000 AS SIGNED)";
// HEX doubles bytes. Keep the existing 8 MiB field/32 MiB query bounds;
// inputs above 4 MiB are a typed per-task failure, never a truncated prompt.
constexpr size_t MAX_INPUT_BYTES = 4 * 1024 * 1024;
using Rows = std::vector<std::vector<std::string>>;
std::string bytes(const ObString &s) { return s.empty() ? std::string() : std::string(s.ptr(), s.length()); }
std::string hex(const std::string &s)
{
  static const char *digits = "0123456789abcdef";
  std::string out; out.reserve(s.size() * 2);
  for (unsigned char c : s) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}
std::string literal(const std::string &s) { return "X'" + hex(s) + "'"; }
std::string text(const std::string &s) { return "CONVERT(" + literal(s) + " USING utf8mb4)"; }
std::string ident(const std::string &s)
{
  std::string out = "`";
  for (char c : s) { out += c; if (c == '`') out += '`'; }
  return out + '`';
}
int unhex(const std::string &s, std::string &out)
{
  if (s.size() % 2 || s.size() > 8 * 1024 * 1024) return OB_SIZE_OVERFLOW;
  out.clear(); out.reserve(s.size() / 2);
  auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (size_t i = 0; i < s.size(); i += 2) {
    int a = digit(s[i]), b = digit(s[i + 1]);
    if (a < 0 || b < 0) return OB_INVALID_DATA;
    out += static_cast<char>((a << 4) | b);
  }
  return OB_SUCCESS;
}
// All fields are explicitly CAST/HEXed so native LOB locators never escape the
// result's lifetime. Bound both row count and field size before owning copies.
int query(ObISQLClient &client, const std::string &sql, int columns, size_t limit, Rows &out)
{
  int ret = OB_SUCCESS;
  ObISQLClient::ReadResult result;
  if (OB_FAIL(client.read(result, sql.c_str()))) return ret;
  auto *r = result.get_result();
  if (!r) return OB_ERR_UNEXPECTED;
  size_t total_bytes = 0;
  while (OB_SUCC(ret)) {
    const int next_ret = r->next();
    if (next_ret == OB_ITER_END) break;
    if (next_ret != OB_SUCCESS) { ret = next_ret; break; }
    if (out.size() >= limit) return OB_SIZE_OVERFLOW;
    std::vector<std::string> row;
    for (int i = 0; OB_SUCC(ret) && i < columns; ++i) {
      ObString value;
      if (OB_FAIL(r->get_varchar(static_cast<int64_t>(i), value))) {}
      else if (value.length() > 8 * 1024 * 1024) ret = OB_SIZE_OVERFLOW;
      else if ((total_bytes += value.length()) > 32 * 1024 * 1024) ret = OB_SIZE_OVERFLOW;
      else row.push_back(bytes(value));
    }
    if (OB_SUCC(ret)) out.push_back(std::move(row));
  }
  // next() must be checked explicitly: OB_SUCC(expression) doesn't assign ret.
  return ret;
}
int write(ObISQLClient &client, const std::string &sql, int64_t &affected)
{ return client.write(sql.c_str(), affected); }
std::string n(int64_t value) { return std::to_string(value); }
std::string key(int64_t table, int64_t column, const std::string &rowkey, const std::string &request)
{
  return "table_id=" + n(table) + " AND column_id=" + n(column)
      + " AND `rowkey`=" + literal(rowkey) + " AND request_id=" + literal(request);
}
std::string group_key(int64_t table, const std::string &rowkey, const std::string &group)
{ return "table_id=" + n(table) + " AND `rowkey`=" + literal(rowkey) + " AND group_id=" + literal(group); }
} // namespace

int AiColumnWorker::init(ObISQLClient &proxy, ObMultiVersionSchemaService &schemas,
                         int64_t concurrency, int64_t lease_us)
{
  int ret = OB_SUCCESS;
  if (proxy_) ret = OB_INIT_TWICE;
  else if (concurrency < 2 || concurrency > 4 || lease_us < 30000000 || lease_us > 900000000) ret = OB_INVALID_ARGUMENT;
  else if (OB_FAIL(create("AiColumn", concurrency))) {}
  else { proxy_ = &proxy; schemas_ = &schemas; lease_us_ = lease_us; }
  return ret;
}
void AiColumnWorker::destroy()
{
  // Base destroy wakes/stops/joins before any dependencies are released.
  if (proxy_) rootserver::ObServerThreadHelper::destroy();
  proxy_ = nullptr; schemas_ = nullptr;
}
void AiColumnWorker::do_work()
{
  while (!has_set_stop()) {
    const int ret = run_once();
    if (ret != OB_SUCCESS && ret != OB_EAGAIN) LOG_WARN("AI COLUMN worker pass failed", KR(ret));
    idle(100000);
  }
}

int AiColumnWorker::load(ObISQLClient &client, const std::string &where, std::vector<Task> &tasks)
{
  Rows rows;
  int ret = query(client,
      "SELECT /*+ USE_PLAN_CACHE(NONE) */ CAST(table_id AS CHAR),CAST(column_id AS CHAR),CAST(definition_version AS CHAR),"
      "CAST(state AS CHAR),CAST(retry_count AS CHAR),HEX(`rowkey`),HEX(request_id),"
      "HEX(IFNULL(group_id,'')),HEX(IFNULL(rowkey_predicate,'')),HEX(IFNULL(group_membership,'')),"
      // Input is fetched only for one claimed task, outside this bounded
      // metadata batch (publication does not need the model input at all).
      "HEX(IFNULL(result_spec,'')),"
      "HEX(IFNULL(lease_token,'')),CAST(input IS NULL AS CHAR) FROM " + std::string(TASKS) + " WHERE " + where, 13, 4096, rows);
  for (const auto &r : rows) {
    if (OB_FAIL(ret)) break;
    Task t;
    t.input_null = r[12] == "1";
    if (!ai_column_value::integer(r[0], t.table) || !ai_column_value::integer(r[1], t.column)
        || !ai_column_value::integer(r[2], t.version) || !ai_column_value::integer(r[3], t.state)
        || !ai_column_value::integer(r[4], t.retries)) { ret = OB_INVALID_DATA; break; }
    std::string *fields[] = {&t.rowkey, &t.request, &t.group, &t.predicate, &t.members, &t.spec, &t.token};
    for (int i = 0; OB_SUCC(ret) && i < 7; ++i) ret = unhex(r[i + 5], *fields[i]);
    if (OB_SUCC(ret)) tasks.push_back(std::move(t));
  }
  return ret;
}

int AiColumnWorker::target(const Task &task, Target &out, bool require_version,
                           const sql::ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *db = nullptr;
  ObArenaAllocator arena("AiColTarget");
  ObSEArray<AiColumnDefinition, 4> definitions;
  if (OB_FAIL(schemas_->get_runtime_schema_guard(guard))) {}
  else if (OB_FAIL(guard.get_table_schema(task.table, table))) {}
  else if (!table || table->is_in_recyclebin()) ret = OB_TABLE_NOT_EXIST;
  else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), db))) {}
  else if (!db) ret = OB_TABLE_NOT_EXIST;
  else if (OB_FAIL(list_ai_column_definitions(*table, arena, definitions))) {}
  else {
    ret = OB_ENTRY_NOT_EXIST;
    for (int64_t i = 0; i < definitions.count(); ++i) {
      const auto &d = definitions.at(i);
      if (d.column_id != static_cast<uint64_t>(task.column)) continue;
      const auto *col = table->get_column_schema(d.column_id);
      if (!col || (require_version && d.definition_version != task.version)) return OB_SCHEMA_EAGAIN;
      out.version = d.definition_version; out.max_retries = d.max_retries;
      out.dimension = d.vector_dimension; out.max_length = col->get_accuracy().get_length();
      out.table = ident(bytes(db->get_database_name_str())) + "." + ident(bytes(table->get_table_name_str()));
      out.column = ident(bytes(col->get_column_name_str()));
      out.state = ident(ai_column_helper_name(col->get_column_name_str(), 0));
      out.request = ident(ai_column_helper_name(col->get_column_name_str(), 1));
      out.spec = ident(ai_column_helper_name(col->get_column_name_str(), 2));
      out.model = bytes(d.model_name);
      if (d.embedding) out.kind = Target::VECTOR;
      else if (col->get_data_type() == ObIntType) out.kind = Target::INTEGER;
      else if (col->get_data_type() == ObJsonType) out.kind = Target::JSON;
      else if (ob_is_string_type(col->get_data_type())) {
        out.kind = Target::STRING;
        out.collation = col->get_collation_type();
        // MySQL CHAR/VARCHAR use characters; TEXT/BLOB and binary strings
        // use bytes in the destination charset, not UTF-8 character count.
        out.length_in_bytes = ob_is_text_tc(col->get_data_type()) || out.collation == CS_TYPE_BINARY;
      }
      else return OB_NOT_SUPPORTED;
      // The persisted SQL predicate is diagnostic only. Derive the actual row
      // predicate from the native identity and current PK column IDs, so SQL
      // mode changes, renamed keys, or a stale predicate cannot target a row.
      ObRowkey rowkey;
      int64_t pos = 0;
      if (OB_FAIL(rowkey.deserialize(arena, task.rowkey.data(), task.rowkey.size(), pos))) return ret;
      const auto &pk = table->get_rowkey_info();
      if (pos != task.rowkey.size() || rowkey.get_obj_cnt() != pk.get_size()) return OB_INVALID_DATA;
      // Rendering must use the eventual transaction's SQL mode/time zone,
      // never the enqueueing session's mode or a detached session's defaults.
      if (session == nullptr) { ret = OB_SUCCESS; break; }
      ObSqlString predicate;
      for (int64_t k = 0; OB_SUCC(ret) && k < pk.get_size(); ++k) {
        const auto *pk_column = table->get_column_schema(pk.get_column(k)->column_id_);
        if (!pk_column) return OB_SCHEMA_EAGAIN;
        const ObObj &cell = rowkey.get_obj_ptr()[k];
        char *buf = nullptr; int64_t length = 0, written = 0;
        ObObjPrintParams params = CREATE_OBJ_PRINT_PARAM(session);
        params.need_cast_expr_ = true;
        GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES, session->get_sql_mode(), params.skip_escape_);
        if (k && OB_FAIL(predicate.append(" AND "))) {}
        else if (OB_FAIL(predicate.append(ident(bytes(pk_column->get_column_name_str())).c_str()))) {}
        else if (OB_FAIL(predicate.append(cell.is_null() ? " IS " : " = "))) {}
        else if (OB_FAIL(cell.print_sql_literal(buf, length, written, arena, params))) {}
        else ret = predicate.append(buf, written);
      }
      if (OB_SUCC(ret)) out.predicate = bytes(predicate.string());
      break;
    }
  }
  return ret;
}

int AiColumnWorker::claim(Task &task, bool &claimed)
{
  claimed = false;
  Rows rows;
  int ret = query(*proxy_, "SELECT /*+ USE_PLAN_CACHE(NONE) */ REPLACE(UUID(),'-','')", 1, 1, rows);
  if (OB_FAIL(ret)) return ret;
  if (rows.size() != 1) return OB_ERR_UNEXPECTED;
  task.token = rows[0][0];
  const bool recovered = task.state == 1;
  int64_t affected = 0;
  std::string sql = "UPDATE /*+ USE_PLAN_CACHE(NONE) */ " + std::string(TASKS) + " SET state=1,lease_token=" + literal(task.token)
      + ",lease_expiry=" + NOW_US + "+" + n(lease_us_) + " WHERE "
      + key(task.table, task.column, task.rowkey, task.request)
      + " AND definition_version=" + n(task.version) + " AND retry_count=" + n(task.retries)
      + " AND state=" + n(task.state)
      + " AND ((state IN (0,3) AND eligible_ts<=NOW(6))"
      + " OR (state=1 AND lease_expiry<=" + NOW_US + "))";
  // A lost lease consumed an attempt too. Persist the increment atomically
  // with ownership, otherwise repeated crashes evade max_retries forever.
  if (recovered) sql.insert(sql.find(" WHERE "), ",retry_count=retry_count+1");
  if (OB_FAIL(write(*proxy_, sql, affected))) {}
  else if ((claimed = affected == 1) && recovered) ++task.retries;
  return ret;
}

int AiColumnWorker::load_input(Task &task)
{
  Rows rows;
  // Gate in SQL before HEX/materialization. SUBSTR is an additional hard
  // bound, not permission to infer on a partial input. The length is checked
  // independently and overflow follows the ordinary stage/retry/publication
  // path. A single large LONGTEXT cannot poison the oldest metadata batch.
  int ret = query(*proxy_,
      "SELECT /*+ USE_PLAN_CACHE(NONE) */ CASE WHEN IFNULL(OCTET_LENGTH(input),0)<=" + n(MAX_INPUT_BYTES)
      + " THEN HEX(SUBSTR(CAST(IFNULL(input,'') AS BINARY),1," + n(MAX_INPUT_BYTES) + ")) ELSE '' END,"
      "CAST(IFNULL(OCTET_LENGTH(input),0) AS CHAR),CAST(input IS NULL AS CHAR),"
      "CAST(IFNULL(JSON_EXTRACT(CAST(result_spec AS JSON),'$.version')=2 AND "
      "JSON_UNQUOTE(JSON_EXTRACT(CAST(result_spec AS JSON),'$.input_kind'))='json',0) AS CHAR) FROM " + std::string(TASKS)
      + " WHERE " + key(task.table, task.column, task.rowkey, task.request)
      + " AND state=1 AND lease_token=" + literal(task.token)
      + " AND definition_version=" + n(task.version), 4, 1, rows);
  if (OB_FAIL(ret)) return ret;
  if (rows.size() != 1) return OB_EAGAIN;
  int64_t length = 0;
  if (!ai_column_value::integer(rows[0][1], length) || length < 0) return OB_INVALID_DATA;
  if (length > MAX_INPUT_BYTES) {
    task.input_too_large = true;
    return OB_SIZE_OVERFLOW;
  }
  task.input_null = rows[0][2] == "1";
  task.input_json = rows[0][3] == "1";
  if (OB_FAIL(unhex(rows[0][0], task.input))) return ret;
  return task.input.size() == static_cast<size_t>(length) ? OB_SUCCESS : OB_INVALID_DATA;
}

int AiColumnWorker::load_staged(ObISQLClient &client, const Task &task, std::string &value)
{
  Rows rows;
  // Scheduler/group discovery stays metadata-only. Fetch at most one complete
  // staged value per query; a group never multiplies the result field budget.
  int ret = query(client,
      "SELECT /*+ USE_PLAN_CACHE(NONE) */ CASE WHEN IFNULL(OCTET_LENGTH(staged_result),-1) BETWEEN 0 AND "
      + n(MAX_INPUT_BYTES) + " THEN HEX(SUBSTR(CAST(IFNULL(staged_result,'') AS BINARY),1,"
      + n(MAX_INPUT_BYTES) + ")) ELSE '' END,CAST(IFNULL(OCTET_LENGTH(staged_result),-1) AS CHAR) FROM "
      + std::string(TASKS) + " WHERE " + key(task.table, task.column, task.rowkey, task.request)
      + " AND state=2 AND definition_version=" + n(task.version)
      + " AND lease_token=" + text(task.token) + " AND result_spec <=> " + text(task.spec), 2, 1, rows);
  if (ret != OB_SUCCESS) return ret;
  if (rows.size() != 1) return OB_EAGAIN;
  int64_t length = 0;
  if (!ai_column_value::integer(rows[0][1], length) || length < 0
      || length > static_cast<int64_t>(MAX_INPUT_BYTES)) return OB_SIZE_OVERFLOW;
  if (OB_FAIL(unhex(rows[0][0], value))) return ret;
  return value.size() == static_cast<size_t>(length) ? OB_SUCCESS : OB_INVALID_DATA;
}

int AiColumnWorker::infer(const Task &task, const Target &target, std::string &value)
{
  // Match SQL NULL propagation; do not send a different (empty) prompt.
  if (task.input_null) return OB_ERR_NULL_VALUE;
  // Absolutely no transaction/row lock exists here. This uses the same native
  // expression/provider/endpoint path as an ordinary AI_COMPLETE/AI_EMBED SELECT.
  std::string expr;
  if (target.kind == Target::VECTOR) {
    expr = "AI_EMBED(" + text(target.model) + "," + text(task.input) + "," + n(target.dimension) + ")";
  } else {
    const std::string input = task.input_json ? "CAST(" + text(task.input) + " AS JSON)" : text(task.input);
    expr = "AI_COMPLETE(" + text(target.model) + "," + input;
    const char *instruction = target.kind == Target::JSON ? "Return exactly one valid JSON value"
        : target.kind == Target::INTEGER ? "Return exactly one base-10 integer" : nullptr;
    if (instruction) {
      std::string user = task.input;
      if (task.input_json) {
        // Explicit messages override AI_COMPLETE's default user message. Use
        // exactly its native renderer, not the persisted LONGTEXT envelope.
        ObArenaAllocator arena("AiColPrompt");
        ObIJsonBase *node = nullptr;
        ObString rendered;
        int ret = ObJsonBaseFactory::get_json_base(&arena,
            ObString(task.input.size(), task.input.data()),
            ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, node);
        if (ret != OB_SUCCESS) return ret;
        if (!node || node->json_type() != ObJsonNodeType::J_OBJECT) return OB_INVALID_ARGUMENT;
        ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
            arena, static_cast<ObJsonObject *>(node), rendered);
        if (ret != OB_SUCCESS) return ret;
        user = bytes(rendered);
      }
      expr += ",JSON_OBJECT('messages',JSON_ARRAY(JSON_OBJECT('role','system','content',"
          + text(instruction) + "),JSON_OBJECT('role','user','content'," + text(user) + ")))";
    }
    expr += ")";
  }
  Rows rows;
  int ret = query(*proxy_, "SELECT /*+ USE_PLAN_CACHE(NONE) QUERY_TIMEOUT(120000000) */ HEX(" + expr + ")", 1, 1, rows);
  if (OB_FAIL(ret)) return ret;
  if (rows.size() != 1) return OB_INVALID_DATA;
  if (OB_FAIL(unhex(rows[0][0], value))) return ret;
  return validate(target, value);
}

int AiColumnWorker::validate(const Target &target, std::string &value)
{
  if (value.size() > 4 * 1024 * 1024) return OB_SIZE_OVERFLOW;
  int64_t integer = 0;
  size_t chars = 0;
  if (target.kind == Target::INTEGER) {
    if (!ai_column_value::integer(value, integer)) return OB_INVALID_DATA;
    value = n(integer);
  } else if (target.kind == Target::VECTOR) {
    if (!ai_column_value::vector(value, target.dimension)) return OB_INVALID_DATA;
  } else {
    if (!ai_column_value::utf8_length(value, chars)) return OB_INVALID_DATA;
    if (target.kind == Target::STRING) {
      // Validate the same UTF-8 -> target charset conversion native UPDATE
      // will perform. Reject lossy conversion even in a non-strict session,
      // and check TEXT's encoded-byte limit before publication locks.
      size_t length = chars;
      if (target.collation == CS_TYPE_BINARY) {
        length = value.size();
      } else {
        int64_t mbmaxlen = 0;
        int ret = ObCharset::get_mbmaxlen_by_coll(target.collation, mbmaxlen);
        if (ret != OB_SUCCESS) return ret;
        if (mbmaxlen <= 0 || mbmaxlen > 4) return OB_NOT_SUPPORTED;
        std::string converted(std::max<size_t>(1, value.size() * mbmaxlen), '\0');
        uint32_t converted_length = 0;
        ret = ObCharset::charset_convert(CS_TYPE_UTF8MB4_BIN, value.data(), value.size(),
            target.collation, &converted[0], converted.size(), converted_length, false, true);
        if (ret != OB_SUCCESS) return ret;
        if (target.length_in_bytes) length = converted_length;
      }
      if (target.max_length < 0 || length > static_cast<size_t>(target.max_length)) return OB_SIZE_OVERFLOW;
    } else {
      Rows rows;
      int ret = query(*proxy_, "SELECT /*+ USE_PLAN_CACHE(NONE) */ CAST(JSON_VALID(" + text(value) + ") AS CHAR)", 1, 1, rows);
      if (ret != OB_SUCCESS) return ret;
      if (rows.size() != 1 || rows[0][0] != "1") return OB_INVALID_DATA;
    }
  }
  return OB_SUCCESS;
}

int AiColumnWorker::stage(const Task &task, const Target &target, int error, const std::string &value)
{
  int64_t state = error == OB_SUCCESS ? 2 : task.retries < target.max_retries ? 3 : 4;
  int64_t retries = std::min(task.retries + (error == OB_SUCCESS ? 0 : 1), target.max_retries + 1);
  int64_t affected = 0;
  return write(*proxy_, "UPDATE /*+ USE_PLAN_CACHE(NONE) */ " + std::string(TASKS) + " SET state=" + n(state)
      + ",retry_count=" + n(retries) + ",eligible_ts=TIMESTAMPADD(MICROSECOND,"
      + n(ai_column_value::backoff_us(retries)) + ",NOW(6)),staged_result="
      + (error == OB_SUCCESS ? text(value) : "NULL") + ",error_code=" + n(error)
      + ",error=" + (error == OB_SUCCESS ? "NULL" : text(task.input_too_large
          ? "AI COLUMN input exceeds the 4194304-byte worker limit"
          : "native AI inference or typed validation failed"))
      + " WHERE " + key(task.table, task.column, task.rowkey, task.request)
      + " AND state=1 AND lease_token=" + literal(task.token) + " AND definition_version=" + n(task.version)
      + " AND lease_expiry>" + NOW_US, affected);
}

int AiColumnWorker::publish(const Task &seed)
{
  // Lock order matches user DML: user row, then its task group. Two publishers
  // may race, but row locking plus generation predicates makes publication once.
  std::vector<Task> snapshot;
  int ret = load(*proxy_, group_key(seed.table, seed.rowkey, seed.group) + " ORDER BY column_id", snapshot);
  if (ret != OB_SUCCESS || snapshot.empty()) return ret;
  std::vector<int64_t> members;
  if (!ai_column_value::membership(snapshot[0].members, members)) return OB_INVALID_DATA;
  std::sort(members.begin(), members.end());
  std::vector<Target> targets;
  for (size_t i = 0; i < snapshot.size(); ++i) {
    const Task &t = snapshot[i]; Target target_info;
    if (!std::binary_search(members.begin(), members.end(), t.column)
        || t.request != seed.request || t.members != snapshot[0].members
        || t.predicate != seed.predicate || (t.state != 2 && t.state != 4)) return OB_EAGAIN;
    if (OB_FAIL(target(t, target_info))) return ret;
    if (t.state == 2) {
      std::string value;
      if (OB_FAIL(load_staged(*proxy_, t, value))) return ret;
      if (OB_FAIL(validate(target_info, value))) return ret;
    }
    targets.push_back(std::move(target_info));
  }
  // DML cancels only affected columns. A newer c0 generation can remove its
  // old task while c1 is still a valid member of the original group. Missing
  // members may be ignored only after their row request is proven superseded.
  std::vector<Target> missing;
  for (int64_t column : members) {
    if (std::any_of(snapshot.begin(), snapshot.end(), [column](const Task &t) { return t.column == column; })) continue;
    Task old = seed; old.column = column;
    Target d;
    const int rc = target(old, d, false);
    if (rc == OB_ENTRY_NOT_EXIST) continue; // dropped AI column
    if (rc != OB_SUCCESS) return rc;
    missing.push_back(std::move(d));
  }
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(proxy_))) return ret;
  ObInnerSQLConnection *conn = static_cast<ObInnerSQLConnection *>(trans.get_connection());
  if (!conn || !conn->is_inner_session()) ret = OB_ERR_UNEXPECTED;
  else {
  sql::AiColumnInternalWriteGuard internal(conn->get_session());
  if (OB_SUCC(ret)) ret = target(snapshot[0], targets[0], true, &conn->get_session());
  Rows row;
  std::string row_sql = "SELECT /*+ USE_PLAN_CACHE(NONE) */ ";
  for (size_t i = 0; i < targets.size(); ++i) {
    if (i) row_sql += ',';
    row_sql += "HEX(IFNULL(" + targets[i].request + ",''))";
  }
  for (const auto &d : missing) row_sql += ",HEX(IFNULL(" + d.request + ",''))";
  row_sql += " FROM " + targets[0].table + " WHERE " + targets[0].predicate + " FOR UPDATE";
  if (OB_SUCC(ret)) ret = query(trans, row_sql, targets.size() + missing.size(), 1, row);
  std::vector<bool> active(targets.size(), false);
  if (row.size() == 1) {
    for (size_t i = 0; OB_SUCC(ret) && i < targets.size() + missing.size(); ++i) {
      std::string decoded;
      if (OB_FAIL(unhex(row[0][i], decoded))) {}
      else if (i < targets.size()) active[i] = decoded == snapshot[i].request;
      else if (decoded == seed.request) ret = OB_EAGAIN; // still-current member's task is missing
    }
  }
  std::vector<Task> locked;
  if (OB_SUCC(ret)) ret = load(trans, group_key(seed.table, seed.rowkey, seed.group) + " ORDER BY column_id FOR UPDATE", locked);
  if (OB_SUCC(ret) && locked.size() != snapshot.size()) ret = OB_EAGAIN;
  for (size_t i = 0; OB_SUCC(ret) && i < locked.size(); ++i) {
    const auto &a = locked[i]; const auto &b = snapshot[i];
    if (a.column != b.column || a.version != b.version || a.state != b.state || a.token != b.token
        || a.request != b.request || a.group != b.group || a.members != b.members
        || a.spec != b.spec || a.predicate != b.predicate) ret = OB_EAGAIN;
    Rows definition;
    if (OB_SUCC(ret)) ret = query(trans, "SELECT /*+ USE_PLAN_CACHE(NONE) */ CAST(definition_version AS CHAR) FROM oceanbase.__all_ai_column WHERE table_id="
        + n(a.table) + " AND column_id=" + n(a.column) + " FOR UPDATE", 1, 1, definition);
    if (OB_SUCC(ret) && (definition.size() != 1 || definition[0][0] != n(a.version))) active[i] = false;
  }
  int64_t affected = 0;
  if (OB_SUCC(ret) && std::any_of(active.begin(), active.end(), [](bool value) { return value; })) {
    // One bounded native UPDATE per active column, under the same row lock and
    // transaction. Readers see all results/states only at the single commit;
    // no giant SQL literal or in-memory concatenation for the whole group.
    for (size_t i = 0; OB_SUCC(ret) && i < locked.size(); ++i) {
      const auto &t = locked[i]; const auto &d = targets[i];
      if (!active[i]) continue;
      std::string value;
      if (t.state == 2 && OB_FAIL(load_staged(trans, t, value))) break;
      if (t.state == 2 && OB_FAIL(validate(d, value))) break;
      std::string update = "UPDATE /*+ USE_PLAN_CACHE(NONE) */ " + targets[0].table + " SET ";
      std::string expr = t.state == 4 ? "NULL" : d.kind == Target::INTEGER ? value
          : d.kind == Target::JSON ? "CAST(" + text(value) + " AS JSON)"
          : d.kind == Target::VECTOR ? value : text(value);
      update += d.column + "=" + expr + "," + d.state + "=" + (t.state == 2 ? "1" : "2");
      std::string guards = " AND " + d.request + "=" + literal(t.request) + " AND " + d.state + "=0 AND " + d.column + " IS NULL";
      // Preserve the hidden result specification as another optimistic fence.
      guards += " AND " + d.spec + " <=> " + text(t.spec);
      if (OB_SUCC(ret)) ret = write(trans, update + " WHERE " + targets[0].predicate + guards, affected);
      // A failed fence rolls back every earlier column UPDATE in this group.
      if (OB_SUCC(ret) && affected != 1) ret = OB_EAGAIN;
    }
  }
  if (OB_SUCC(ret)) ret = write(trans, "DELETE /*+ USE_PLAN_CACHE(NONE) */ FROM " + std::string(TASKS) + " WHERE "
      + group_key(seed.table, seed.rowkey, seed.group), affected);
  } // restore capability while this exact pooled connection is still owned
  const int end_ret = trans.end(OB_SUCC(ret));
  return ret == OB_SUCCESS ? end_ret : ret;
}

int AiColumnWorker::run_once()
{
  if (!proxy_) return OB_NOT_INIT;
  std::vector<Task> tasks;
  // Recover already-staged groups too: a crash between stage and publish must
  // not strand terminal tasks forever. Both scans are explicitly bounded.
  int ret = load(*proxy_, "state IN (2,4) ORDER BY gmt_modified LIMIT 8", tasks);
  for (const auto &t : tasks) {
    const int rc = publish(t);
    if (rc != OB_SUCCESS && rc != OB_EAGAIN) LOG_WARN("AI COLUMN publish deferred", K(rc));
  }
  tasks.clear();
  if (OB_FAIL(ret)) return ret;
  ret = load(*proxy_, "((state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_expiry<="
      + std::string(NOW_US) + ")) ORDER BY eligible_ts LIMIT 8", tasks);
  for (auto &t : tasks) {
    if (has_set_stop()) break;
    bool claimed = false;
    if (OB_FAIL(ret = claim(t, claimed))) break;
    if (!claimed) continue;
    Target d;
    if (OB_FAIL(target(t, d))) {
      // DDL cleanup normally removes these tasks atomically. Do not infer from
      // a changed/dropped definition or manufacture a replacement user row.
      LOG_WARN("AI COLUMN definition unavailable", KR(ret));
      continue;
    }
    std::string value;
    if (has_set_stop()) break; // leave the committed lease for recovery
    int infer_ret = t.retries > d.max_retries ? OB_TIMEOUT : load_input(t);
    if (infer_ret == OB_SUCCESS) infer_ret = infer(t, d, value);
    if (OB_FAIL(ret = stage(t, d, infer_ret, value))) break;
    const int pub_ret = publish(t);
    if (pub_ret != OB_SUCCESS && pub_ret != OB_EAGAIN) LOG_WARN("AI COLUMN publication deferred", K(pub_ret));
    // One external call per pass per worker, not an unbounded in-memory queue.
    break;
  }
  return ret;
}
}} // namespace oceanbase::observer
