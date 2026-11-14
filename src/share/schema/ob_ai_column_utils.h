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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTILS_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTILS_H_

#include "lib/ob_define.h"
#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
}
namespace obcall
{
struct ObAlterTableArg;
}
namespace share
{

namespace schema
{

// Observable AI column metadata/task tables live in the sys database as ordinary
// user tables so that they can be queried with plain SQL.
extern const char *const OB_AI_COLUMN_META_TABLE;
extern const char *const OB_AI_COLUMN_TASK_TABLE;

// Task state machine.
static const int64_t OB_AI_TASK_PENDING = 0;
static const int64_t OB_AI_TASK_RUNNING = 1;
static const int64_t OB_AI_TASK_READY = 2;
static const int64_t OB_AI_TASK_RETRYING = 3;
static const int64_t OB_AI_TASK_FAILED = 4;

// Create the two observable tables if they do not exist yet.
int ensure_ai_column_tables(common::ObMySQLProxy &proxy);

// Append the three hidden helper columns (state/request/result-spec) for every AI
// column of the table schema. Used on the CREATE TABLE path before the DDL is sent
// to the root server.
int append_ai_hidden_columns(ObTableSchema &table_schema);

// Same as above for ALTER TABLE ... ADD COLUMN: append hidden helper columns to
// the alter argument. The final column ids are assigned by the root server.
int append_ai_hidden_columns(obcall::ObAlterTableArg &arg);

// Find the hidden helper column id by name; returns OB_ENTRY_NOT_EXIST when absent.
int get_ai_hidden_column_id(const ObTableSchema &table_schema,
                            const ObString &column_name,
                            const char *prefix,
                            uint64_t &column_id);

// Insert/update the metadata rows of all AI columns of the table.
int sync_ai_column_metadata(common::ObMySQLProxy &proxy,
                            const ObTableSchema &table_schema,
                            const uint64_t table_id,
                            const ObTableSchema *source_schema = NULL);

// A process-wide monotonically increasing request id, used as the version fence
// and as the task group id.
int64_t next_ai_request_id();

// Server-issued SQL that maintains AI columns (background worker / RETRY command)
// is tagged with this marker. It travels with the statement text, so it also works
// for sql_proxy sessions that are not marked as inner sessions.
extern const char *const OB_AI_COLUMN_INTERNAL_MARKER;

bool is_ai_column_internal_sql(const ObString &sql);

// Escape a string literal and append it with surrounding single quotes.
int append_sql_literal(common::ObSqlString &sql, const ObString &value);

// DELETE FROM __all_ai_column_task for one row and all its AI columns.
int build_ai_task_delete_sql(common::ObSqlString &sql,
                             const uint64_t table_id,
                             const ObString &rowkey_predicate);

// INSERT ... ON DUPLICATE KEY UPDATE for a group of AI columns of one row.
int build_ai_task_upsert_sql(common::ObSqlString &sql,
                             const ObTableSchema &table_schema,
                             const ObString &database_name,
                             const uint64_t table_id,
                             const ObString &rowkey_predicate,
                             const int64_t request_id,
                             const ObIArray<const ObColumnSchemaV2 *> &ai_columns);

// UPDATE <db>.<table> SET ai_col = NULL, hidden_request = request_id
// WHERE rowkey_predicate, used to invalidate a row before regeneration.
int build_ai_invalidate_sql(common::ObSqlString &sql,
                            const ObTableSchema &table_schema,
                            const ObString &database_name,
                            const ObString &rowkey_predicate,
                            const int64_t request_id,
                            const ObIArray<const ObColumnSchemaV2 *> &ai_columns);

// SELECT 1 FROM db.table WHERE ai_col IS NULL LIMIT 1
int build_ai_strict_check_sql(common::ObSqlString &sql,
                              const ObString &database_name,
                              const ObString &table_name,
                              const ObString &column_name);

// Build the normalized AI expression that is stored in metadata and evaluated by
// the worker: type contract prompts are injected through the AI_COMPLETE config.
int build_ai_normalized_expr(ObIAllocator &allocator,
                             const ObColumnSchemaV2 &column,
                             const ObString &expr,
                             ObString &normalized);

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTILS_H_
