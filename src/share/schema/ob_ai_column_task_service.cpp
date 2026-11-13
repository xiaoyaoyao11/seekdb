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

#define USING_LOG_PREFIX SHARE_SCHEMA

#include "share/schema/ob_ai_column_task_service.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "lib/object/ob_object.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

int ObAiColumnTaskService::insert_task(common::ObISQLClient &sql_client,
                                        const int64_t task_id,
                                        const int64_t table_id,
                                        const int64_t column_id,
                                        const common::ObString &request_id,
                                        const int64_t retry_count,
                                        const common::ObString &rowkey_predicate,
                                        const int64_t state,
                                        const int64_t eligible_ts)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  common::ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("task_id", task_id))) {
  } else if (OB_FAIL(sql.add_column("table_id", table_id))) {
  } else if (OB_FAIL(sql.add_column("column_id", column_id))) {
  } else if (OB_FAIL(sql.add_column("request_id", common::ObHexEscapeSqlStr(request_id)))) {
  } else if (OB_FAIL(sql.add_column("retry_count", retry_count))) {
  } else if (OB_FAIL(sql.add_column("rowkey_predicate", common::ObHexEscapeSqlStr(rowkey_predicate)))) {
  } else if (OB_FAIL(sql.add_column("state", state))) {
  } else if (OB_FAIL(sql.add_column("eligible_ts", eligible_ts))) {
  } else if (OB_FAIL(sql.splice_insert_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(sql_client.write(buffer.ptr(), affected_rows))) {
  } else if (1 != affected_rows) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected rows when inserting an ai column task",
             K(ret), K(affected_rows), K(task_id), K(table_id), K(column_id));
  }
  return ret;
}

int ObAiColumnTaskService::delete_task(common::ObISQLClient &sql_client,
                                        const int64_t task_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  common::ObSqlString buffer;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.add_pk_column("task_id", task_id))) {
  } else if (OB_FAIL(sql.splice_delete_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
  } else if (OB_FAIL(sql_client.write(buffer.ptr(), affected_rows))) {
  } else {
    // deleting a task which is already gone is not an error
    UNUSED(affected_rows);
  }
  return ret;
}

int ObAiColumnTaskService::delete_tasks(common::ObISQLClient &sql_client,
                                         const int64_t table_id,
                                         const int64_t column_id)
{
  int ret = OB_SUCCESS;
  common::ObSqlString buffer;
  int64_t affected_rows = 0;
  // the delete is driven by a condition, so the statement is built directly
  // instead of going through ObDMLSqlSplicer (which only deletes by primary key)
  if (OB_FAIL(buffer.append_fmt("DELETE FROM %s WHERE table_id = %ld",
                                 OB_ALL_AI_COLUMN_TASK_TNAME, table_id))) {
  } else if (column_id > 0
             && OB_FAIL(buffer.append_fmt(" AND column_id = %ld", column_id))) {
  } else if (OB_FAIL(sql_client.write(buffer.ptr(), affected_rows))) {
  } else {
    UNUSED(affected_rows);
  }
  return ret;
}

int ObAiColumnTaskService::delete_tasks_by_predicate(common::ObISQLClient &sql_client,
                                                      const common::ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  common::ObSqlString buffer;
  int64_t affected_rows = 0;
  if (rowkey_predicate.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buffer.append_fmt("DELETE FROM %s WHERE rowkey_predicate = '%.*s'",
                                       OB_ALL_AI_COLUMN_TASK_TNAME,
                                       rowkey_predicate.length(),
                                       rowkey_predicate.ptr()))) {
  } else if (OB_FAIL(sql_client.write(buffer.ptr(), affected_rows))) {
  } else {
    UNUSED(affected_rows);
  }
  return ret;
}

} // end namespace schema
} // end namespace share
} // end namespace oceanbase
