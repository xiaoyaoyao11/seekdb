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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/dml/ob_ai_column_task_creator.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/resolver/dml/ob_ai_column_dml_util.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "share/schema/ob_ai_column_task_service.h"
#include "share/schema/ob_ai_column_util.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "common/object/ob_obj_type.h"
#include "lib/utility/utility.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace sql
{

/**
 * @brief Adapts the inner SQL connection of the session to the ObISQLClient
 *        interface.
 *
 * The rows of the AI column task queue have to join the transaction of the
 * session which invalidated the AI column, so they are written through the
 * inner connection of that session instead of the tenant proxy.
 */
class ObAiColumnInnerSqlClient : public common::ObISQLClient
{
public:
  explicit ObAiColumnInnerSqlClient(ObIInnerSQLConnection *conn) : conn_(conn) {}
  virtual ~ObAiColumnInnerSqlClient() {}

  virtual int escape(const char *from, const int64_t from_size, char *to,
                     const int64_t to_size, int64_t &out_size) override
  {
    UNUSED(from);
    UNUSED(from_size);
    UNUSED(to);
    UNUSED(to_size);
    UNUSED(out_size);
    return OB_NOT_SUPPORTED;
  }

  virtual int read(ReadResult &res, const char *sql, const int32_t group_id) override
  {
    UNUSED(res);
    UNUSED(sql);
    UNUSED(group_id);
    return OB_NOT_SUPPORTED;
  }

  virtual int write(const char *sql, const int32_t group_id,
                    int64_t &affected_rows) override
  {
    UNUSED(group_id);
    if (OB_ISNULL(conn_)) {
      return OB_NOT_INIT;
    }
    return conn_->execute_write(ObString(sql), affected_rows);
  }

  virtual common::sqlclient::ObISQLConnection *get_connection() override { return conn_; }

private:
  ObIInnerSQLConnection *conn_;
};

/**
 * Append the literal of one primary key value to @p condition.
 *
 * The format is the one which the worker and the tests expect:
 *   `id` = 2
 */
static int append_pk_literal(ObSqlString &condition,
                             const ObColumnSchemaV2 &column,
                             const ObDatum &datum)
{
  int ret = OB_SUCCESS;
  if (datum.is_null()) {
    ret = condition.append("NULL");
  } else if (ob_is_integer_type(column.get_data_type())) {
    ret = condition.append_fmt("%ld", datum.get_int());
  } else if (ob_is_string_type(column.get_data_type())) {
    const ObString &value = datum.get_string();
    const char *data = value.ptr();
    const int64_t len = value.length();
    if (OB_FAIL(condition.append("'"))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
        if ('\'' == data[i]) {
          ret = condition.append("''");
        } else {
          ret = condition.append(data + i, 1);
        }
      }
      if (OB_SUCC(ret)) {
        ret = condition.append("'");
      }
    }
  } else {
    // an unsupported primary key type: the predicate cannot be built, so the
    // task is skipped rather than written with a wrong predicate.
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

/**
 * Build the primary key predicate of one row, e.g. "`id` = 2".
 *
 * The column ids of the row are carried by dml_ctdef.column_ids_, which is
 * parallel to the row expressions.
 */
static int build_rowkey_predicate(ObIAllocator &allocator,
                                  const ObTableSchema &table_schema,
                                  const ObDMLBaseCtDef &dml_ctdef,
                                  const ObExprPtrIArray &row,
                                  ObEvalCtx &eval_ctx,
                                  ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString condition;
  bool has_pk = false;
  const int64_t cnt = MIN(row.count(), dml_ctdef.column_ids_.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
    ObExpr *expr = row.at(i);
    if (OB_ISNULL(expr)) {
      continue;
    }
    const uint64_t column_id = dml_ctdef.column_ids_.at(i);
    const ObColumnSchemaV2 *column = table_schema.get_column_schema(column_id);
    if (OB_ISNULL(column) || !column->is_rowkey_column()) {
      continue;
    }
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
    } else if (OB_ISNULL(datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!has_pk && OB_FAIL(condition.append_fmt("`%.*s` = ",
                                                       column->get_column_name_str().length(),
                                                       column->get_column_name_str().ptr()))) {
    } else if (FALSE_IT(has_pk = true)) {
    } else if (OB_FAIL(append_pk_literal(condition, *column, *datum))) {
      if (OB_NOT_SUPPORTED == ret) {
        // an unsupported primary key type: skip this row
        ret = OB_SUCCESS;
        condition.reset();
        has_pk = false;
        break;
      }
    }
  }
  if (OB_SUCC(ret) && has_pk) {
    char *buf = nullptr;
    const int64_t len = condition.length();
    if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, condition.ptr(), len);
      predicate.assign(buf, len);
    }
  } else if (OB_SUCC(ret)) {
    // the row has no primary key column, the task cannot address the row
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

/**
 * Build the id of the request group which one row belongs to.
 *
 * Every AI column of one row shares the same request id, so that the worker
 * publishes the results of the row atomically.
 *
 * The request id carries the moment of the change, so that a later change of
 * the same row starts a new request group: the worker can then tell that the
 * result of the older group is outdated.
 */
static int build_request_id(ObIAllocator &allocator,
                            const uint64_t table_id,
                            const int64_t change_ts,
                            const ObString &rowkey_predicate,
                            ObString &request_id)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  const int64_t len = 256;
  if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    const int64_t written = snprintf(buf, len, "%lu:%ld:%.*s",
                                      static_cast<unsigned long>(table_id),
                                      change_ts,
                                      rowkey_predicate.length(),
                                      rowkey_predicate.ptr());
    if (written <= 0 || written >= len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      request_id.assign(buf, written);
    }
  }
  return ret;
}

/**
 * Collect the names of the columns which an UPDATE statement changed.
 *
 * The changed columns are the ones the assignments of the statement touch.
 * An update of a primary key column moves the row, so the flag which tells
 * that the row moved is raised as well: every AI column of the row has to be
 * generated again, because the row itself is a new one.
 */
static int collect_updated_columns(const ObUpdCtDef &upd_ctdef,
                                   const ObTableSchema &table_schema,
                                   ObIArray<ObString> &changed_columns,
                                   bool &rowkey_updated)
{
  int ret = OB_SUCCESS;
  const UIntFixedArray &updated_ids = upd_ctdef.dupd_ctdef_.updated_column_ids_;
  for (int64_t i = 0; OB_SUCC(ret) && i < updated_ids.count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema(updated_ids.at(i));
    if (OB_NOT_NULL(column)) {
      if (column->is_rowkey_column()) {
        rowkey_updated = true;
      }
      if (OB_FAIL(changed_columns.push_back(column->get_column_name_str()))) {
      }
    }
  }
  return ret;
}

int ObAiColumnTaskCreator::create_tasks(ObDMLModifyRowNode &modify_row,
                                         ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard *schema_guard = NULL;
  const ObTableSchema *table_schema = NULL;
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObString rowkey_predicate;
  ObString request_id;
  const ObDmlEventType t_insert = ObDmlEventType::DE_INSERTING;
  const ObDmlEventType t_update = ObDmlEventType::DE_UPDATING;
  const ObDmlEventType t_delete = ObDmlEventType::DE_DELETING;
  if (OB_ISNULL(modify_row.dml_ctdef_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(schema_guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard->get_table_schema(
                 modify_row.dml_ctdef_->das_base_ctdef_.table_id_, table_schema))) {
  } else if (OB_ISNULL(table_schema) || !table_schema->has_ai_column()) {
    // the table has no AI column
  } else if (OB_FAIL(ObAiColumnDmlUtil::collect_all_ai_columns(*table_schema, ai_columns))) {
  } else if (0 == ai_columns.count()) {
    // no AI column at all
  } else {
    const ObDmlEventType dml_event = modify_row.dml_event_;
    const ObExprPtrIArray &new_row = modify_row.dml_ctdef_->new_row_;
    const ObExprPtrIArray &old_row = modify_row.dml_ctdef_->old_row_;
    ObIAllocator &allocator = op.get_exec_ctx().get_allocator();
    ObIInnerSQLConnection *inner_conn = as_inner_sql_connection(op.get_inner_conn());
    if (OB_ISNULL(inner_conn)) {
      ret = OB_NOT_INIT;
    } else if (t_delete == dml_event) {
      // the row is gone: its pending tasks are dropped
      if (OB_FAIL(build_rowkey_predicate(allocator, *table_schema, *modify_row.dml_ctdef_,
                                         old_row, op.get_eval_ctx(), rowkey_predicate))) {
        if (OB_NOT_SUPPORTED == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        ObAiColumnInnerSqlClient inner_client(inner_conn);
        if (OB_FAIL(ObAiColumnTaskService::delete_tasks_by_predicate(
                       inner_client, table_schema->get_table_id(), rowkey_predicate))) {
          LOG_WARN("fail to delete the ai column tasks of a deleted row", K(ret));
        }
      }
    } else if (t_insert == dml_event || t_update == dml_event) {
      // the AI columns which the statement invalidated are the ones whose
      // dependency changed; on an INSERT every AI column is invalidated.
      ObSEArray<const ObColumnSchemaV2 *, 4> affected_columns;
      // the moment of the change: it identifies the generation of the value of
      // the row, so that a later change of the same row starts a new request
      // group and the worker can drop the result of the older one
      const int64_t change_ts = ObTimeUtility::current_time_us();
      if (t_insert == dml_event) {
        if (OB_FAIL(affected_columns.assign(ai_columns))) {
        }
      } else {
        ObSEArray<ObString, 8> changed_columns;
        bool rowkey_updated = false;
        const ObUpdCtDef *upd_ctdef = static_cast<const ObUpdCtDef *>(modify_row.dml_ctdef_);
        if (OB_FAIL(collect_updated_columns(*upd_ctdef, *table_schema, changed_columns,
                                            rowkey_updated))) {
        } else if (rowkey_updated) {
          // the row moved to another primary key: it is a new row, so every AI
          // column of it has to be generated again
          if (OB_FAIL(affected_columns.assign(ai_columns))) {
          }
        } else if (OB_FAIL(ObAiColumnDmlUtil::collect_affected_ai_columns(
                              *table_schema, changed_columns, affected_columns))) {
        }
      }
      if (OB_SUCC(ret) && 0 == affected_columns.count()) {
        // the statement did not touch any AI column
      } else if (OB_SUCC(ret)
                 && OB_FAIL(build_rowkey_predicate(allocator, *table_schema, *modify_row.dml_ctdef_,
                                                   new_row, op.get_eval_ctx(), rowkey_predicate))) {
        if (OB_NOT_SUPPORTED == ret) {
          ret = OB_SUCCESS;
        }
      } else if (OB_SUCC(ret)
                 && OB_FAIL(build_request_id(allocator,
                                            table_schema->get_table_id(),
                                            change_ts,
                                            rowkey_predicate, request_id))) {
      } else {
        ObAiColumnInnerSqlClient inner_client(inner_conn);
        for (int64_t i = 0; OB_SUCC(ret) && i < affected_columns.count(); ++i) {
          const ObColumnSchemaV2 *ai_column = affected_columns.at(i);
          const int64_t task_id = ObAiColumnTaskService::alloc_task_id(
              table_schema->get_table_id(), ai_column->get_column_id(), request_id);
          if (OB_FAIL(ObAiColumnTaskService::insert_task(
                         inner_client,
                         task_id,
                         table_schema->get_table_id(),
                         ai_column->get_column_id(),
                         request_id,
                         /*retry_count=*/0,
                         rowkey_predicate,
                         /*state=*/0,
                         /*eligible_ts=*/0))) {
            LOG_WARN("fail to create an ai column task", K(ret),
                     K(table_schema->get_table_id()), K(ai_column->get_column_id()));
          }
        }
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
