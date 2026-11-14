/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SHARE

#include "share/schema/ob_ai_column_utils.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "lib/oblog/ob_log_module.h"
#include "lib/container/ob_se_array.h"
#include "lib/time/ob_time_utility.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ob_rpc_struct.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_service.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

const char *const OB_AI_COLUMN_META_TABLE = "oceanbase.__all_ai_column";
const char *const OB_AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";

namespace
{

const char *const AI_STATE_PREFIX = "__ai_state_";
const char *const AI_REQUEST_PREFIX = "__ai_request_";
const char *const AI_SPEC_PREFIX = "__ai_spec_";
const int64_t AI_HIDDEN_COLUMN_NAME_LEN = 64;
const int64_t AI_SPEC_COLUMN_LENGTH = 8192;

int make_hidden_column_name(const char *prefix,
                            const ObString &column_name,
                            char *buf,
                            const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
  int64_t name_len = column_name.length();
  if (OB_ISNULL(buf) || prefix_len >= buf_len) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    if (prefix_len + name_len >= AI_HIDDEN_COLUMN_NAME_LEN) {
      name_len = AI_HIDDEN_COLUMN_NAME_LEN - prefix_len - 1;
    }
    const int64_t pos = snprintf(buf, static_cast<size_t>(buf_len), "%s%.*s",
                                 prefix, static_cast<int>(name_len), column_name.ptr());
    if (pos <= 0 || pos >= buf_len) {
      ret = OB_BUF_NOT_ENOUGH;
    }
    buf[buf_len - 1] = '\0';
  }
  return ret;
}

int init_int_column(ObColumnSchemaV2 &column,
                    const uint64_t column_id,
                    const char *name,
                    const common::ColumnType type)
{
  int ret = OB_SUCCESS;
  column.reset();
  column.set_column_id(column_id);
  column.set_data_type(type);
  column.set_data_length(0);
  column.set_collation_type(CS_TYPE_BINARY);
  column.set_is_hidden(true);
  column.set_nullable(true);
  column.set_prev_column_id(UINT64_MAX);
  column.set_next_column_id(UINT64_MAX);
  if (OB_FAIL(column.set_column_name(name))) {
  }
  return ret;
}

int init_varchar_column(ObColumnSchemaV2 &column,
                        const uint64_t column_id,
                        const char *name)
{
  int ret = OB_SUCCESS;
  column.reset();
  column.set_column_id(column_id);
  column.set_data_type(ObVarcharType);
  column.set_data_length(AI_SPEC_COLUMN_LENGTH);
  column.set_charset_type(CHARSET_UTF8MB4);
  column.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  column.set_is_hidden(true);
  column.set_nullable(true);
  column.set_prev_column_id(UINT64_MAX);
  column.set_next_column_id(UINT64_MAX);
  if (OB_FAIL(column.set_column_name(name))) {
  }
  return ret;
}

int collect_ai_columns(const ObTableSchema &table_schema,
                       ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && NULL != it && it != table_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (column->is_ai_column()) {
      if (OB_FAIL(ai_columns.push_back(column))) {
      }
    }
  }
  return ret;
}

bool ai_expr_references_identifier(const ObString &expr, const ObString &name)
{
  bool found = false;
  const char *p = expr.ptr();
  const int64_t len = expr.length();
  const char *n = name.ptr();
  const int64_t name_len = name.length();
  for (int64_t i = 0; !found && name_len > 0 && i + name_len <= len; ++i) {
    const bool left_ok = 0 == i
        || (!isalnum(static_cast<unsigned char>(p[i - 1])) && '_' != p[i - 1]);
    const bool right_ok = i + name_len == len
        || (!isalnum(static_cast<unsigned char>(p[i + name_len])) && '_' != p[i + name_len]);
    if (left_ok && right_ok && 0 == strncasecmp(p + i, n, name_len)) {
      found = true;
    }
  }
  return found;
}

// An AI column may only depend on ordinary columns: chained AI columns would make
// the generation order ambiguous.
int check_ai_column_dependencies(const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 8> ai_columns;
  if (OB_FAIL(collect_ai_columns(table_schema, ai_columns))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
    const ObString &expr = ai_column->get_ai_column_expr();
    for (int64_t j = 0; OB_SUCC(ret) && j < ai_columns.count(); ++j) {
      if (i == j) {
        continue;
      }
      if (ai_expr_references_identifier(expr, ai_columns.at(j)->get_column_name_str())) {
        // Keep the default OB_NOT_SUPPORTED message: chained generation is not supported.
        ret = OB_NOT_SUPPORTED;
      }
    }
  }
  return ret;
}

} // namespace

int ensure_ai_column_tables(common::ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  static const char *const meta_ddl =
      "create table if not exists oceanbase.__all_ai_column("
      "table_id bigint not null,"
      "column_id bigint not null,"
      "column_name varchar(128),"
      "max_retries bigint,"
      "state_column_id bigint,"
      "request_column_id bigint,"
      "result_spec_column_id bigint,"
      "normalized_expr varchar(8192),"
      "primary key(table_id, column_id))";
  static const char *const task_ddl =
      "create table if not exists oceanbase.__all_ai_column_task("
      "table_id bigint not null,"
      "column_id bigint not null,"
      "database_name varchar(128),"
      "table_name varchar(128),"
      "column_name varchar(128),"
      "rowkey_predicate varchar(512) not null,"
      "request_id bigint not null,"
      "state bigint not null default 0,"
      "retry_count bigint not null default 0,"
      "max_retries bigint not null default 0,"
      "eligible_ts timestamp(6) not null default current_timestamp(6),"
      "primary key(table_id, column_id, rowkey_predicate))";
  int64_t affected_rows = 0;
  if (OB_FAIL(proxy.write(meta_ddl, affected_rows))) {
    LOG_WARN("failed to create ai column metadata table", K(ret));
  } else if (OB_FAIL(proxy.write(task_ddl, affected_rows))) {
    LOG_WARN("failed to create ai column task table", K(ret));
  }
  return ret;
}

int append_ai_hidden_columns(ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 8> ai_columns;
  if (OB_FAIL(check_ai_column_dependencies(table_schema))) {
  } else if (OB_FAIL(collect_ai_columns(table_schema, ai_columns))) {
  } else if (ai_columns.empty()) {
  } else {
    uint64_t next_column_id = table_schema.get_max_used_column_id();
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
      char state_name[AI_HIDDEN_COLUMN_NAME_LEN];
      char request_name[AI_HIDDEN_COLUMN_NAME_LEN];
      char spec_name[AI_HIDDEN_COLUMN_NAME_LEN];
      const ObString &column_name = ai_column->get_column_name_str();
      if (OB_FAIL(make_hidden_column_name(AI_STATE_PREFIX, column_name,
                                          state_name, sizeof(state_name)))) {
      } else if (OB_FAIL(make_hidden_column_name(AI_REQUEST_PREFIX, column_name,
                                                 request_name, sizeof(request_name)))) {
      } else if (OB_FAIL(make_hidden_column_name(AI_SPEC_PREFIX, column_name,
                                                 spec_name, sizeof(spec_name)))) {
      } else {
        ObColumnSchemaV2 state_column;
        ObColumnSchemaV2 request_column;
        ObColumnSchemaV2 spec_column;
        if (OB_FAIL(init_int_column(state_column, ++next_column_id, state_name, ObIntType))) {
        } else if (OB_FAIL(init_int_column(request_column, ++next_column_id,
                                           request_name, ObIntType))) {
        } else if (OB_FAIL(init_varchar_column(spec_column, ++next_column_id, spec_name))) {
        } else if (OB_FAIL(table_schema.add_column(state_column))) {
        } else if (OB_FAIL(table_schema.add_column(request_column))) {
        } else if (OB_FAIL(table_schema.add_column(spec_column))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      // add_column() does not maintain this counter, but the hidden columns must be
      // covered by it before the schema is sent to the root server.
      table_schema.set_max_used_column_id(next_column_id);
    }
  }
  return ret;
}

int append_ai_hidden_columns(obcall::ObAlterTableArg &arg)
{
  int ret = OB_SUCCESS;
  AlterTableSchema &alter_schema = arg.alter_table_schema_;
  ObSEArray<const ObColumnSchemaV2 *, 8> add_ai_columns;
  int tmp_ret = check_ai_column_dependencies(alter_schema);
  if (OB_SUCCESS != tmp_ret) {
    ret = tmp_ret;
  }
  for (ObTableSchema::const_column_iterator it = alter_schema.column_begin();
       OB_SUCC(ret) && NULL != it && it != alter_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    const AlterColumnSchema *alter_column = static_cast<const AlterColumnSchema *>(column);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (column->is_ai_column() && OB_DDL_ADD_COLUMN == alter_column->alter_type_) {
      if (OB_FAIL(add_ai_columns.push_back(column))) {
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < add_ai_columns.count(); ++i) {
    const ObString &column_name = add_ai_columns.at(i)->get_column_name_str();
    char state_name[AI_HIDDEN_COLUMN_NAME_LEN];
    char request_name[AI_HIDDEN_COLUMN_NAME_LEN];
    char spec_name[AI_HIDDEN_COLUMN_NAME_LEN];
    if (OB_FAIL(make_hidden_column_name(AI_STATE_PREFIX, column_name,
                                        state_name, sizeof(state_name)))) {
    } else if (OB_FAIL(make_hidden_column_name(AI_REQUEST_PREFIX, column_name,
                                               request_name, sizeof(request_name)))) {
    } else if (OB_FAIL(make_hidden_column_name(AI_SPEC_PREFIX, column_name,
                                               spec_name, sizeof(spec_name)))) {
    } else {
      AlterColumnSchema state_column;
      AlterColumnSchema request_column;
      AlterColumnSchema spec_column;
      state_column.reset();
      request_column.reset();
      spec_column.reset();
      if (OB_FAIL(init_int_column(state_column, 0, state_name, ObIntType))) {
      } else if (OB_FAIL(init_int_column(request_column, 0, request_name, ObIntType))) {
      } else if (OB_FAIL(init_varchar_column(spec_column, 0, spec_name))) {
      } else {
        // init_*() resets the column, so the alter type must be set afterwards.
        state_column.alter_type_ = OB_DDL_ADD_COLUMN;
        request_column.alter_type_ = OB_DDL_ADD_COLUMN;
        spec_column.alter_type_ = OB_DDL_ADD_COLUMN;
        if (OB_FAIL(alter_schema.add_alter_column(state_column, true))) {
        } else if (OB_FAIL(alter_schema.add_alter_column(request_column, true))) {
        } else if (OB_FAIL(alter_schema.add_alter_column(spec_column, true))) {
        }
      }
    }
  }
  return ret;
}

int get_ai_hidden_column_id(const ObTableSchema &table_schema,
                            const ObString &column_name,
                            const char *prefix,
                            uint64_t &column_id)
{
  int ret = OB_SUCCESS;
  char name_buf[AI_HIDDEN_COLUMN_NAME_LEN];
  const ObColumnSchemaV2 *column = NULL;
  column_id = OB_INVALID_ID;
  if (OB_FAIL(make_hidden_column_name(prefix, column_name, name_buf, sizeof(name_buf)))) {
  } else if (OB_ISNULL(column = table_schema.get_column_schema(ObString(name_buf)))) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    column_id = column->get_column_id();
  }
  return ret;
}

int sync_ai_column_metadata(common::ObMySQLProxy &proxy,
                            const ObTableSchema &table_schema,
                            const uint64_t table_id,
                            const ObTableSchema *source_schema)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 8> ai_columns;
  ObSqlString sql;
  // The post-DDL schema read back from the schema service may have lost the AI
  // expression of collection columns (their extended type info is reserved for the
  // type itself), so the in-memory DDL schema is the authoritative source.
  const ObTableSchema &meta_schema = NULL == source_schema ? table_schema : *source_schema;
  if (!meta_schema.has_ai_column()) {
  } else if (OB_FAIL(collect_ai_columns(meta_schema, ai_columns))) {
  } else if (ai_columns.empty()) {
  } else if (OB_FAIL(sql.append("insert into oceanbase.__all_ai_column("
                                "table_id, column_id, column_name, max_retries,"
                                " state_column_id, request_column_id, result_spec_column_id,"
                                " normalized_expr) values "))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *column = ai_columns.at(i);
      const ObColumnSchemaV2 *final_column =
          table_schema.get_column_schema(column->get_column_name_str());
      uint64_t state_id = OB_INVALID_ID;
      uint64_t request_id = OB_INVALID_ID;
      uint64_t spec_id = OB_INVALID_ID;
      const uint64_t column_id = NULL == final_column
          ? column->get_column_id() : final_column->get_column_id();
      const ObString &column_name = column->get_column_name_str();
      const ObString &expr = column->get_ai_column_expr().empty() && NULL != final_column
          ? final_column->get_ai_column_expr() : column->get_ai_column_expr();
      const int max_retries = 0 == column->get_ai_max_retries() && NULL != final_column
          ? final_column->get_ai_max_retries() : column->get_ai_max_retries();
      int tmp_ret = get_ai_hidden_column_id(table_schema, column_name,
                                            AI_STATE_PREFIX, state_id);
      if (OB_SUCCESS != tmp_ret) {
        tmp_ret = get_ai_hidden_column_id(meta_schema, column_name,
                                          AI_STATE_PREFIX, state_id);
      }
      if (OB_SUCCESS != tmp_ret) {
        ret = tmp_ret;
        LOG_WARN("failed to find ai state column", K(ret), K(column_name));
      } else if (OB_FAIL(get_ai_hidden_column_id(table_schema, column_name,
                                                 AI_REQUEST_PREFIX, request_id))) {
        LOG_WARN("failed to find ai request column", K(ret), K(column_name));
      } else if (OB_FAIL(get_ai_hidden_column_id(table_schema, column_name,
                                                 AI_SPEC_PREFIX, spec_id))) {
        LOG_WARN("failed to find ai spec column", K(ret), K(column_name));
      } else if (OB_FAIL(sql.append_fmt("%s(%lu, %lu, ",
                                       0 == i ? "" : ", ",
                                       table_id, column_id))) {
      } else if (OB_FAIL(append_sql_literal(sql, column_name))) {
      } else if (OB_FAIL(sql.append_fmt(", %d, %lu, %lu, %lu, ",
                                        max_retries,
                                        state_id, request_id, spec_id))) {
      } else if (OB_FAIL(append_sql_literal(sql, expr))) {
      } else if (OB_FAIL(sql.append(")"))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append(" on duplicate key update column_name = values(column_name),"
                             " max_retries = values(max_retries),"
                             " state_column_id = values(state_column_id),"
                             " request_column_id = values(request_column_id),"
                             " result_spec_column_id = values(result_spec_column_id),"
                             " normalized_expr = values(normalized_expr)"))) {
      }
    }
    int64_t affected_rows = 0;
    LOG_INFO("sync ai column metadata", K(ret), K(table_id), K(ai_columns.count()), K(sql));
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(proxy.write(sql.ptr(), affected_rows))) {
      LOG_WARN("failed to write ai column metadata", K(ret), K(sql));
    }
  }
  return ret;
}

int64_t next_ai_request_id()
{
  static std::atomic<int64_t> seq(0);
  const int64_t now = common::ObTimeUtility::current_time();
  const int64_t id = (now << 12) | (seq.fetch_add(1) & 0xFFF);
  return id;
}

const char *const OB_AI_COLUMN_INTERNAL_MARKER = "/*ai_column_internal*/";

bool is_ai_column_internal_sql(const ObString &sql)
{
  bool found = false;
  const int64_t marker_len = static_cast<int64_t>(strlen(OB_AI_COLUMN_INTERNAL_MARKER));
  const char *p = sql.ptr();
  for (int64_t i = 0; !found && NULL != p && i + marker_len <= sql.length(); ++i) {
    if (0 == strncasecmp(p + i, OB_AI_COLUMN_INTERNAL_MARKER, marker_len)) {
      found = true;
    }
  }
  return found;
}

int append_sql_literal(common::ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  } else {
    const char *data = value.ptr();
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      if ('\'' == data[i]) {
        ret = sql.append("''");
      } else {
        ret = sql.append(data + i, 1);
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("'");
    }
  }
  return ret;
}

int build_ai_task_delete_sql(common::ObSqlString &sql,
                             const uint64_t table_id,
                             const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task"
                             " where table_id = %lu and rowkey_predicate = ", table_id))) {
  } else if (OB_FAIL(append_sql_literal(sql, rowkey_predicate))) {
  }
  return ret;
}

int build_ai_task_upsert_sql(common::ObSqlString &sql,
                             const ObTableSchema &table_schema,
                             const ObString &database_name,
                             const uint64_t table_id,
                             const ObString &rowkey_predicate,
                             const int64_t request_id,
                             const ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  if (ai_columns.empty()) {
  } else if (OB_FAIL(sql.append("insert into oceanbase.__all_ai_column_task("
                                "table_id, column_id, database_name, table_name,"
                                " column_name, rowkey_predicate, request_id, state,"
                                " retry_count, max_retries, eligible_ts) values "))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *column = ai_columns.at(i);
      if (OB_FAIL(sql.append_fmt("%s(%lu, %lu, ",
                                 0 == i ? "" : ", ",
                                 table_id, column->get_column_id()))) {
      } else if (OB_FAIL(append_sql_literal(sql, database_name))) {
      } else if (OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(append_sql_literal(sql, table_schema.get_table_name_str()))) {
      } else if (OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(append_sql_literal(sql, column->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(append_sql_literal(sql, rowkey_predicate))) {
      } else if (OB_FAIL(sql.append_fmt(", %ld, %ld, 0, %d, now(6))",
                                        request_id, OB_AI_TASK_PENDING,
                                        static_cast<int>(column->get_ai_max_retries())))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append(" on duplicate key update request_id = values(request_id),"
                             " state = values(state), retry_count = 0,"
                             " max_retries = values(max_retries),"
                             " eligible_ts = values(eligible_ts)"))) {
      }
    }
  }
  return ret;
}

int build_ai_invalidate_sql(common::ObSqlString &sql,
                            const ObTableSchema &table_schema,
                            const ObString &database_name,
                            const ObString &rowkey_predicate,
                            const int64_t request_id,
                            const ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  if (ai_columns.empty()) {
  } else if (OB_FAIL(sql.append(OB_AI_COLUMN_INTERNAL_MARKER))) {
  } else if (OB_FAIL(sql.append_fmt("update `%.*s`.`%.*s` set ",
                                    database_name.length(), database_name.ptr(),
                                    table_schema.get_table_name_str().length(),
                                    table_schema.get_table_name_str().ptr()))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *column = ai_columns.at(i);
      char request_name[AI_HIDDEN_COLUMN_NAME_LEN];
      if (OB_FAIL(make_hidden_column_name(AI_REQUEST_PREFIX,
                                          column->get_column_name_str(),
                                          request_name, sizeof(request_name)))) {
      } else if (OB_FAIL(sql.append_fmt("%s`%.*s` = NULL, `%s` = %ld",
                                       0 == i ? "" : ", ",
                                       column->get_column_name_str().length(),
                                       column->get_column_name_str().ptr(),
                                       request_name, request_id))) {
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(sql.append(" where "))) {
      } else if (OB_FAIL(sql.append(rowkey_predicate))) {
      }
    }
  }
  return ret;
}

int build_ai_strict_check_sql(common::ObSqlString &sql,
                              const ObString &database_name,
                              const ObString &table_name,
                              const ObString &column_name)
{
  return sql.append_fmt("select 1 from `%.*s`.`%.*s` where `%.*s` is null limit 1",
                        database_name.length(), database_name.ptr(),
                        table_name.length(), table_name.ptr(),
                        column_name.length(), column_name.ptr());
}

int build_ai_normalized_expr(ObIAllocator &allocator,
                             const ObColumnSchemaV2 &column,
                             const ObString &expr,
                             ObString &normalized)
{
  int ret = OB_SUCCESS;
  normalized.reset();
  const char *p = expr.ptr();
  const int64_t len = expr.length();
  auto match_name = [&](const char *name, const int64_t name_len) -> bool {
    int64_t pos = 0;
    while (pos < len && (' ' == p[pos] || '\t' == p[pos] || '\n' == p[pos] || '\r' == p[pos])) {
      ++pos;
    }
    bool matched = len - pos >= name_len && 0 == strncasecmp(p + pos, name, name_len);
    return matched && (len - pos == name_len || '(' == p[pos + name_len]
                       || ' ' == p[pos + name_len]);
  };
  if (!match_name("ai_complete", 11)) {
    // AI_EMBED returns the vector value directly.
  } else {
    const bool is_json = ob_is_json_tc(column.get_data_type());
    const bool is_integer = column.get_meta_type().is_integer_type();
    if (!is_json && !is_integer) {
      // String results need no type contract prompt.
    } else {
      const char *prompt = is_json
          ? "Return exactly one valid JSON value and nothing else."
          : "Return the answer as a base-10 integer and nothing else.";
      const char *cast_type = is_json ? "JSON" : "SIGNED";
      // Locate the matching ')' of the AI_COMPLETE call so the config argument can
      // be injected before it.
      int64_t open_pos = -1;
      int64_t close_pos = -1;
      int32_t depth = 0;
      for (int64_t i = 0; i < len && close_pos < 0; ++i) {
        if ('(' == p[i]) {
          if (0 == depth) {
            open_pos = i;
          }
          ++depth;
        } else if (')' == p[i]) {
          if (0 == --depth) {
            close_pos = i;
          }
        }
      }
      if (open_pos < 0 || close_pos < 0) {
        // Fall back to the raw expression; validation already passed.
      } else {
        ObSqlString text;
        if (OB_FAIL(text.append("CAST("))) {
        } else if (OB_FAIL(text.append(expr.ptr(), static_cast<int32_t>(close_pos)))) {
        } else if (OB_FAIL(text.append(", '{\"system_prompt\":\""))) {
        } else if (OB_FAIL(text.append(prompt))) {
        } else if (OB_FAIL(text.append("\"}'"))) {
        } else if (OB_FAIL(text.append(expr.ptr() + close_pos, static_cast<int32_t>(len - close_pos)))) {
        } else if (OB_FAIL(text.append_fmt(" AS %s)", cast_type))) {
        } else {
          char *buf = static_cast<char *>(allocator.alloc(text.length()));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, text.ptr(), text.length());
            normalized.assign_ptr(buf, static_cast<int32_t>(text.length()));
          }
        }
      }
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
