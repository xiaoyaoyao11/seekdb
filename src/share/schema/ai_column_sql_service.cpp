/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/schema/ai_column_sql_service.h"
#include "share/schema/ai_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_schema_utils.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{
namespace
{
// JSON is a versioned definition payload, not executable SQL. SQL literals below
// are independently hex escaped by the ordinary schema DML splicer.
int append_json_string(ObSqlString &out, const ObString &value)
{
  int ret = out.append("\"");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    if (c == '"' || c == '\\') {
      ret = out.append_fmt("\\%c", c);
    } else if (c < 0x20) {
      ret = out.append_fmt("\\u%04x", static_cast<unsigned int>(c));
    } else {
      ret = out.append(value.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) { ret = out.append("\""); }
  return ret;
}

int definition_payload(const AiColumnDefinition &def, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append_fmt("{\"format_version\":1,\"embedding\":%s,\"vector_dimension\":%ld,"
                            "\"column_id\":%lu,\"state_column_id\":%lu,\"request_column_id\":%lu,"
                            "\"result_spec_column_id\":%lu,\"max_retries\":%ld,\"column_name\":",
                            def.embedding ? "true" : "false", def.vector_dimension,
                            def.column_id, def.state_column_id, def.request_column_id,
                            def.result_spec_column_id, def.max_retries))) {
  } else if (OB_FAIL(append_json_string(out, def.column_name))) {
  } else if (OB_FAIL(out.append(",\"normalized_expr\":"))) {
  } else if (OB_FAIL(append_json_string(out, def.normalized_expr))) {
  } else if (OB_FAIL(out.append(",\"input_expr\":"))) {
  } else if (OB_FAIL(append_json_string(out, def.input_expr))) {
  } else if (OB_FAIL(out.append(",\"model_name\":"))) {
  } else if (OB_FAIL(append_json_string(out, def.model_name))) {
  } else if (OB_FAIL(out.append(",\"config_expr\":"))) {
  } else if (OB_FAIL(append_json_string(out, def.config_expr))) {
  } else if (OB_FAIL(out.append(",\"dependency_ids\":["))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < def.dependency_ids.count(); ++i) {
    ret = out.append_fmt("%s%lu", i == 0 ? "" : ",", def.dependency_ids.at(i));
  }
  if (OB_SUCC(ret)) { ret = out.append("]}"); }
  return ret;
}

int replace_definition(ObISQLClient &client, const ObTableSchema &table,
                       const AiColumnDefinition &def)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer dml;
  ObSqlString payload;
  ObSqlString sql;
  int64_t affected_rows = 0;
  const ObColumnSchemaV2 *column = table.get_column_schema(def.column_id);
  const ObColumnSchemaV2 *state = table.get_column_schema(def.state_column_id);
  const ObColumnSchemaV2 *request = table.get_column_schema(def.request_column_id);
  const ObColumnSchemaV2 *result_spec = table.get_column_schema(def.result_spec_column_id);
  if (OB_ISNULL(column) || OB_ISNULL(state) || OB_ISNULL(request) || OB_ISNULL(result_spec)
      || column->is_hidden() || !state->is_hidden() || !request->is_hidden() || !result_spec->is_hidden()
      || def.column_id == def.state_column_id || def.column_id == def.request_column_id
      || def.column_id == def.result_spec_column_id || def.state_column_id == def.request_column_id
      || def.state_column_id == def.result_spec_column_id || def.request_column_id == def.result_spec_column_id) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(definition_payload(def, payload))) {
  } else if (OB_FAIL(dml.add_pk_column("table_id", ObSchemaUtils::get_extract_schema_id(table.get_table_id())))
      || OB_FAIL(dml.add_pk_column("column_id", def.column_id))
      || OB_FAIL(dml.add_column("schema_version", table.get_schema_version()))
      || OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(column->get_column_name_str())))
      || OB_FAIL(dml.add_column("max_retries", def.max_retries))
      || OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(def.normalized_expr)))
      || OB_FAIL(dml.add_column("state_column_id", def.state_column_id))
      || OB_FAIL(dml.add_column("request_column_id", def.request_column_id))
      || OB_FAIL(dml.add_column("result_spec_column_id", def.result_spec_column_id))
      || OB_FAIL(dml.add_column("definition_version", def.definition_version))
      || OB_FAIL(dml.add_column("definition_payload", ObHexEscapeSqlStr(payload.string())))) {
  } else if (OB_FAIL(dml.splice_replace_sql(OB_ALL_AI_COLUMN_TNAME, sql))) {
  } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
  } else if (affected_rows < 0 || affected_rows > 2) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(dml.add_column("is_deleted", 0))) {
  } else if (FALSE_IT(sql.reuse())) {
  } else if (OB_FAIL(dml.splice_replace_sql(OB_ALL_AI_COLUMN_HISTORY_TNAME, sql))) {
  } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
  } else if (affected_rows < 0 || affected_rows > 2) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// The predicate is built only from numeric IDs, never user supplied SQL text.
int delete_matching(ObISQLClient &client, const ObSqlString &predicate,
                    const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  // Preserve the complete previous definition in its tombstone. REPLACE makes
  // repeated reconciliation within the same DDL schema version idempotent.
  if (OB_FAIL(sql.append_fmt("REPLACE INTO %s (table_id,column_id,schema_version,is_deleted,"
                            "column_name,max_retries,normalized_expr,state_column_id,request_column_id,"
                            "result_spec_column_id,definition_version,definition_payload) "
                            "SELECT table_id,column_id,%ld,1,column_name,max_retries,normalized_expr,"
                            "state_column_id,request_column_id,result_spec_column_id,definition_version,"
                            "definition_payload FROM %s WHERE %s",
                            OB_ALL_AI_COLUMN_HISTORY_TNAME, schema_version,
                            OB_ALL_AI_COLUMN_TNAME, predicate.ptr()))) {
  } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
  } else if (FALSE_IT(sql.reuse())) {
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE %s", OB_ALL_AI_COLUMN_TNAME, predicate.ptr()))) {
  } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
  } else if (FALSE_IT(sql.reuse())) {
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE %s", OB_ALL_AI_COLUMN_TASK_TNAME, predicate.ptr()))) {
  } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
  }
  return ret;
}
} // namespace

int AiColumnSqlService::sync_definitions(ObISQLClient &client, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnSQL");
  ObSEArray<AiColumnDefinition, 4> definitions;
  // Inner schemas are bootstrapped before these catalogs exist, and neither
  // indexes nor LOB auxiliaries own AI definitions.
  if (!is_inner_table(table.get_table_id()) && table.is_user_table()) {
    if (OB_FAIL(list_ai_column_definitions(table, allocator, definitions))) {
    } else if (definitions.empty()) {
      // Reconciliation must also remove the last definition and its tasks. A
      // partially replayed ALTER must not leave orphan catalog generations.
      ret = delete_definitions(client, table.get_table_id(), table.get_schema_version());
    } else {
      ObSqlString predicate;
      if (OB_FAIL(predicate.append_fmt("table_id=%lu AND column_id NOT IN (",
              ObSchemaUtils::get_extract_schema_id(table.get_table_id())))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < definitions.count(); ++i) {
        ret = predicate.append_fmt("%s%lu", i == 0 ? "" : ",", definitions.at(i).column_id);
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(predicate.append(")"))) {
      } else if (OB_FAIL(delete_matching(client, predicate, table.get_schema_version()))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < definitions.count(); ++i) {
        ret = replace_definition(client, table, definitions.at(i));
      }
    }
  }
  return ret;
}

int AiColumnSqlService::delete_definitions(ObISQLClient &client, uint64_t table_id, int64_t schema_version)
{
  int ret = OB_SUCCESS;
  ObSqlString predicate;
  if (is_inner_table(table_id)) {
  } else if (OB_FAIL(predicate.append_fmt("table_id=%lu", ObSchemaUtils::get_extract_schema_id(table_id)))) {
  } else {
    ret = delete_matching(client, predicate, schema_version);
  }
  return ret;
}

int AiColumnSqlService::delete_definition(ObISQLClient &client, uint64_t table_id,
                                         uint64_t column_id, int64_t schema_version)
{
  int ret = OB_SUCCESS;
  ObSqlString predicate;
  if (is_inner_table(table_id)) {
  } else if (OB_FAIL(predicate.append_fmt("table_id=%lu AND column_id=%lu",
              ObSchemaUtils::get_extract_schema_id(table_id), column_id))) {
  } else {
    ret = delete_matching(client, predicate, schema_version);
  }
  return ret;
}
} // namespace schema
} // namespace share
} // namespace oceanbase
