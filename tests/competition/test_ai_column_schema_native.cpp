#include "sql/parser/ob_parser.h"
#include "sql/resolver/ddl/ai_column_resolver.h"
#include "lib/allocator/page_arena.h"
#include "share/schema/ai_column_sql_service.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "common/mysqlclient/ob_isql_client.h"
#include <vector>
#include <cstdio>
#include <cstring>
using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::share::schema;
static int failed = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); ++failed; } } while (0)
static ParseNode *find(ParseNode *n, ObItemType t) {
  if (!n || n->type_ == t) return n;
  for (int i=0; i<n->num_child_; ++i) { ParseNode *r=find(n->children_[i],t); if(r)return r; }
  return nullptr;
}
static void add(ObTableSchema &t, const char *name, uint64_t id, uint64_t flag=0) {
  ObColumnSchemaV2 c; c.set_column_id(id); c.set_data_type(ObVarcharType);
  c.set_data_length(200); c.set_nullable(true); c.add_column_flag(flag);
  c.set_charset_type(CHARSET_UTF8MB4); c.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  CHECK(c.set_column_name(name)==0); int ret=t.add_column(c); std::printf("add %s ret=%d valid=%d\n",name,ret,c.is_valid()); CHECK(ret==0);
}
// Records real service output; it is not a database or SQL integration test.
struct RecordingDDLClient : ObISQLClient {
  std::vector<std::string> statements;
  int fail_at = -1;
  int escape(const char *, int64_t, char *, int64_t, int64_t &) override { return OB_NOT_SUPPORTED; }
  int read(ReadResult &, const char *, int32_t) override { return OB_NOT_SUPPORTED; }
  int write(const char *sql, int32_t, int64_t &affected) override {
    statements.emplace_back(sql); affected = 1;
    return static_cast<int>(statements.size()) == fail_at ? OB_TIMEOUT : OB_SUCCESS;
  }
  sqlclient::ObISQLConnection *get_connection() override { return nullptr; }
};
static void check_persistence(const ObTableSchema &source, ObArenaAllocator &allocator) {
  ObTableSchema table;
  CHECK(table.assign(source) == OB_SUCCESS);
  table.set_table_id(600001); table.set_table_type(USER_TABLE); table.set_schema_version(123456);
  RecordingDDLClient client;
  CHECK(AiColumnSqlService::sync_definitions(client, table) == OB_SUCCESS);
  CHECK(client.statements.size() == 5);
  if (client.statements.size() == 5) {
    CHECK(client.statements[0].find("REPLACE INTO __all_ai_column_history") == 0);
    CHECK(client.statements[2].find("DELETE FROM __all_ai_column_task") == 0);
    CHECK(client.statements[3].find("REPLACE INTO __all_ai_column ") == 0);
    CHECK(client.statements[4].find("REPLACE INTO __all_ai_column_history ") == 0);
    CHECK(client.statements[3].find("definition_payload") != std::string::npos);
  }
  // Recreate exactly the stored column-flags / extended-type-info fields, then
  // derive definitions again. This exercises the real codec, not copied logic.
  ObTableSchema reloaded;
  for (int64_t i = 0; i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 &original = *table.get_column_schema_by_idx(i);
    std::vector<char> rpc_bytes(original.get_serialize_size()); int64_t rpc_pos = 0;
    CHECK(original.serialize(rpc_bytes.data(), rpc_bytes.size(), rpc_pos) == OB_SUCCESS);
    ObColumnSchemaV2 column; int64_t rpc_read = 0;
    CHECK(column.deserialize(rpc_bytes.data(), rpc_pos, rpc_read) == OB_SUCCESS);
    CHECK(rpc_read == rpc_pos);
    CHECK(column.get_column_flags() == original.get_column_flags());
    column.set_column_flags(original.get_stored_column_flags());
    if (column.is_ai_helper_column()) {
      std::vector<char> bytes(65536); int64_t pos = 0;
      CHECK(original.serialize_extended_type_info(bytes.data(), bytes.size(), pos) == OB_SUCCESS);
      ObSEArray<ObString, 1> empty;
      CHECK(column.set_extended_type_info(empty) == OB_SUCCESS);
      int64_t read_pos = 0;
      CHECK(column.deserialize_extended_type_info(bytes.data(), pos, read_pos) == OB_SUCCESS);
      CHECK(read_pos == pos);
    }
    CHECK(reloaded.add_column(column) == OB_SUCCESS);
  }
  ObSEArray<AiColumnDefinition, 4> defs;
  CHECK(list_ai_column_definitions(reloaded, allocator, defs) == OB_SUCCESS);
  CHECK(defs.count() == 1);
  if (defs.count()) CHECK(defs.at(0).state_column_id == 18 && defs.at(0).dependency_ids.at(0) == 16);
  RecordingDDLClient failing_client; failing_client.fail_at = 2;
  CHECK(AiColumnSqlService::sync_definitions(failing_client, table) == OB_TIMEOUT);
  CHECK(failing_client.statements.size() == 2);
  ObTableSchema ordinary; ordinary.set_table_type(USER_TABLE); ordinary.set_table_id(600001);
  ordinary.set_schema_version(123457);
  RecordingDDLClient cleanup;
  CHECK(AiColumnSqlService::sync_definitions(cleanup, ordinary) == OB_SUCCESS);
  CHECK(cleanup.statements.size() == 3);
}
static void check_catalog_factories() {
  using share::ObInnerTableSchema;
  ObTableSchema definition, history, task;
  CHECK(ObInnerTableSchema::all_ai_column_schema(definition) == OB_SUCCESS);
  CHECK(ObInnerTableSchema::all_ai_column_history_schema(history) == OB_SUCCESS);
  CHECK(ObInnerTableSchema::all_ai_column_task_schema(task) == OB_SUCCESS);
  CHECK(definition.get_table_id() == share::OB_ALL_AI_COLUMN_TID);
  CHECK(history.get_table_id() == share::OB_ALL_AI_COLUMN_HISTORY_TID);
  CHECK(task.get_table_id() == share::OB_ALL_AI_COLUMN_TASK_TID);
  CHECK(definition.get_table_type() == SYSTEM_TABLE && history.get_table_type() == SYSTEM_TABLE
        && task.get_table_type() == SYSTEM_TABLE);
  CHECK(definition.is_valid() && history.is_valid() && task.is_valid());
  CHECK(definition.get_rowkey_column_num() == 2 && history.get_rowkey_column_num() == 3
        && task.get_rowkey_column_num() == 4);
  const char *names[] = {"rowkey_predicate", "definition_version", "group_id", "group_membership",
    "input", "result_spec", "state", "retry_count", "eligible_ts", "lease_token", "lease_expiry",
    "staged_result", "error_code", "error"};
  for (const char *name : names) CHECK(task.get_column_schema(ObString::make_string(name)) != nullptr);
  const auto *rowkey = task.get_column_schema(ObString::make_string("rowkey"));
  const auto *eligible = task.get_column_schema(ObString::make_string("eligible_ts"));
  CHECK(rowkey && rowkey->get_data_type() == ObVarcharType && rowkey->get_collation_type() == CS_TYPE_BINARY);
  CHECK(eligible && eligible->get_data_type() == ObDateTimeType && eligible->get_data_scale() == 6);
  const char *keys[] = {"table_id", "column_id", "rowkey", "request_id"};
  for (int i = 0; i < 4; ++i) {
    const auto *column = task.get_column_schema(ObString::make_string(keys[i]));
    CHECK(column && !column->is_nullable() && column->get_rowkey_position() == i + 1);
  }
  const char *nullable[] = {"input", "lease_token", "staged_result", "error"};
  for (const char *name : nullable) {
    const auto *column = task.get_column_schema(ObString::make_string(name));
    CHECK(column && column->is_nullable());
  }
}
int main() {
  check_catalog_factories();
  ObArenaAllocator alloc; sql::ObParser parser(alloc,0);
  ObTableSchema schema; add(schema,"body",16);
  const char *queries[] = {
    "CREATE TABLE t(body varchar(200), tags json AI COLUMN(AI_COMPLETE('model',concat('Classify: ',body))) WITH(sync_mode='async',max_retries=2))",
    "CREATE TABLE t(body varchar(200), tags bigint AI COLUMN(AI_COMPLETE('model',body)))"
  };
  for (auto q:queries) {
    ParseResult r; CHECK(parser.parse(ObString::make_string(q),r)==0);
    ParseNode *attr=find(r.result_tree_,T_AI_COLUMN); CHECK(attr!=nullptr);
    AiColumnDefinition def; ObSEArray<ObString,4> deps;
    ObColumnSchemaV2 result; result.set_column_id(17); result.set_nullable(true);
    result.set_data_type(q==queries[0]?ObJsonType:ObIntType); CHECK(result.set_column_name("tags")==0);
    CHECK(sql::resolve_ai_column_attribute(*attr,result,alloc,def,deps)==0);
    CHECK(def.model_name=="model"); CHECK(deps.count()==1 && deps.at(0)=="body");
    CHECK(std::string(def.normalized_expr.ptr(),def.normalized_expr.length()).find(q==queries[0]?"Return exactly one valid JSON value":"base-10 integer")!=std::string::npos);
    ObTableSchema t; CHECK(t.assign(schema)==0); result.add_column_flag(AI_COLUMN_FLAG); CHECK(t.add_column(result)==0);
    uint64_t id=17; CHECK(sql::append_ai_column_helpers(t,result,def,deps,alloc,id)==0);
    CHECK(id==20 && t.get_column_count()==5);
    check_persistence(t, alloc);
    ObSEArray<AiColumnDefinition,4> defs; CHECK(list_ai_column_definitions(t,alloc,defs)==0);
    CHECK(defs.count()==1); if(defs.count()) { CHECK(defs.at(0).column_id==17); CHECK(defs.at(0).dependency_ids.count()==1); CHECK(defs.at(0).dependency_ids.at(0)==16); CHECK(defs.at(0).state_column_id==18); }
    // The DDL root service assigns different IDs during ALTER. Names are durable associations.
    ObTableSchema reassigned;
    for(int i=0;i<t.get_column_count();++i) { ObColumnSchemaV2 c; CHECK(c.assign(*t.get_column_schema_by_idx(i))==0); c.set_column_id(c.get_column_id()+100); CHECK(reassigned.add_column(c)==0); }
    defs.reset(); CHECK(list_ai_column_definitions(reassigned,alloc,defs)==0);
    CHECK(defs.count()==1); if(defs.count()) { CHECK(defs.at(0).column_id==117); CHECK(defs.at(0).state_column_id==118); CHECK(defs.at(0).dependency_ids.at(0)==116); }
    CHECK(result.get_cur_default_value().is_null()); CHECK(!result.is_generated_column());
    parser.free_result(r);
  }
  const char *bad[] = {
    "AI_COMPLETE(body,body)", "AI_COMPLETE('m')", "AI_COMPLETE('m',rand())",
    "AI_COMPLETE('m',now())", "AI_COMPLETE('m',@x)", "AI_COMPLETE('m',(SELECT body FROM t))",
    "AI_COMPLETE('m',AI_COMPLETE('m',body))", "AI_EMBED('m',body,3)"
  };
  for(auto expr:bad) {
    std::string q="CREATE TABLE t(body varchar(20), tags json AI COLUMN(";q+=expr;q+="))";
    ParseResult r; CHECK(parser.parse(ObString(q.size(),q.data()),r)==0);
    ParseNode *attr=find(r.result_tree_,T_AI_COLUMN); CHECK(attr!=nullptr);
    ObColumnSchemaV2 result; result.set_data_type(ObJsonType); CHECK(result.set_column_name("tags")==0);
    AiColumnDefinition d; ObSEArray<ObString,4> deps;
    CHECK(sql::resolve_ai_column_attribute(*attr,result,alloc,d,deps)!=0);
    parser.free_result(r);
  }
  for (const int dimension : {4, 3}) {
    std::string q = "CREATE TABLE t(body varchar(200), emb vector(4) AI COLUMN(AI_EMBED('model',body,";
    q += std::to_string(dimension); q += ")))";
    ParseResult parsed; CHECK(parser.parse(ObString(q.size(), q.data()), parsed) == OB_SUCCESS);
    ParseNode *attr = find(parsed.result_tree_, T_AI_COLUMN);
    ParseNode *type = find(parsed.result_tree_, T_COLLECTION);
    CHECK(attr != nullptr && type != nullptr);
    ObColumnSchemaV2 column; column.set_nullable(true); column.set_data_type(ObCollectionSQLType);
    CHECK(column.set_column_name("emb") == OB_SUCCESS);
    AiColumnDefinition def; ObSEArray<ObString, 4> deps;
    if (attr && type) {
      const int ret = sql::resolve_ai_column_attribute(*attr, column, alloc, def, deps, type);
      CHECK(ret == (dimension == 4 ? OB_SUCCESS : OB_ERR_INVALID_VECTOR_DIM));
      if (dimension == 4) CHECK(def.embedding && def.vector_dimension == 4 && deps.count() == 1);
    }
    parser.free_result(parsed);
  }
  std::printf("AI column schema native: %d failures\n",failed); return failed?1:0;
}
