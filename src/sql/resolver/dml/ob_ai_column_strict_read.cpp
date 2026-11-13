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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_ai_column_strict_read.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/resolver/dml/ob_delete_stmt.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/ob_server_struct.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;

namespace sql
{

/**
 * Collect the AI columns which the expression tree of @p expr references.
 */
static int collect_ai_columns(ObRawExpr *expr,
                              ObIArray<std::pair<uint64_t, uint64_t> > &ai_columns)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr)) {
    // nothing to do
  } else if (T_COLUMN_REF == expr->get_expr_type()) {
    ObColumnRefRawExpr *column_expr = static_cast<ObColumnRefRawExpr *>(expr);
    if (column_expr->is_ai_column()) {
      const uint64_t table_id = column_expr->get_table_id();
      const uint64_t column_id = column_expr->get_column_id();
      bool found = false;
      for (int64_t i = 0; !found && i < ai_columns.count(); ++i) {
        if (ai_columns.at(i).first == table_id && ai_columns.at(i).second == column_id) {
          found = true;
        }
      }
      if (!found && OB_FAIL(ai_columns.push_back(std::pair<uint64_t, uint64_t>(table_id, column_id)))) {
      }
    }
  } else if (expr->is_query_ref_expr()) {
    // a subquery: the AI columns of the sub statement count as well
    ObQueryRefRawExpr *query_ref = static_cast<ObQueryRefRawExpr *>(expr);
    const ObSelectStmt *sub_stmt = query_ref->get_ref_stmt();
    if (OB_NOT_NULL(sub_stmt)) {
      if (OB_FAIL(ObAiColumnStrictRead::collect_from_select(*sub_stmt, ai_columns))) {
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
      if (OB_FAIL(collect_ai_columns(expr->get_param_expr(i), ai_columns))) {
      }
    }
  }
  return ret;
}

int ObAiColumnStrictRead::collect_from_select(
    const ObSelectStmt &stmt,
    ObIArray<std::pair<uint64_t, uint64_t> > &ai_columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_select_item_size(); ++i) {
    if (OB_FAIL(collect_ai_columns(stmt.get_select_item(i).expr_, ai_columns))) {
    }
  }
  const ObIArray<ObRawExpr*> &conditions = stmt.get_condition_exprs();
  for (int64_t i = 0; OB_SUCC(ret) && i < conditions.count(); ++i) {
    if (OB_FAIL(collect_ai_columns(conditions.at(i), ai_columns))) {
    }
  }
  const ObIArray<ObRawExpr*> &group_exprs = stmt.get_group_exprs();
  for (int64_t i = 0; OB_SUCC(ret) && i < group_exprs.count(); ++i) {
    if (OB_FAIL(collect_ai_columns(group_exprs.at(i), ai_columns))) {
    }
  }
  const ObIArray<ObRawExpr*> &having_exprs = stmt.get_having_exprs();
  for (int64_t i = 0; OB_SUCC(ret) && i < having_exprs.count(); ++i) {
    if (OB_FAIL(collect_ai_columns(having_exprs.at(i), ai_columns))) {
    }
  }
  const ObIArray<OrderItem> &order_items = stmt.get_order_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < order_items.count(); ++i) {
    if (OB_FAIL(collect_ai_columns(order_items.at(i).expr_, ai_columns))) {
    }
  }
  return ret;
}

/**
 * Whether the AI column @p column_id of the table @p table_id still has
 * unfinished tasks.
 *
 * The check is column wide: as long as one row of the AI column is not
 * finished, the whole column counts as unfinished.
 */
static bool has_unfinished_tasks(const uint64_t table_id,
                                 const uint64_t column_id)
{
  bool unfinished = false;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res)
  {
    ObMySQLResult *result = NULL;
    int64_t count = 0;
    if (OB_FAIL(sql.assign_fmt("SELECT count(*) FROM %s WHERE table_id = %ld AND column_id = %ld",
                                OB_ALL_AI_COLUMN_TASK_TNAME,
                                static_cast<long>(table_id),
                                static_cast<long>(column_id)))) {
      LOG_WARN("failed to build the statement of the ai column strict read", K(ret), K(sql));
    } else if (OB_FAIL(GCTX.sql_proxy_.read(res, sql.ptr()))) {
      LOG_WARN("failed to read the tasks of the ai column", K(ret), K(sql));
    } else if (NULL == (result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to read the tasks of the ai column", K(ret), K(sql));
      }
    } else if (OB_FAIL(result->get_int(0, count))) {
      LOG_WARN("failed to read the tasks of the ai column", K(ret), K(sql));
    } else {
      unfinished = (count > 0);
    }
  }
  return unfinished;
}

int ObAiColumnStrictRead::check(ObDMLStmt &stmt, const bool is_strict)
{
  int ret = OB_SUCCESS;
  ObSEArray<std::pair<uint64_t, uint64_t>, 4> ai_columns;
  if (!is_strict) {
    // the strict read is off: the AI column which is not finished is read
    // as NULL
  } else if (OB_FAIL(collect_ai_columns_of_stmt(stmt, ai_columns))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const uint64_t table_id = ai_columns.at(i).first;
      const uint64_t column_id = ai_columns.at(i).second;
      if (has_unfinished_tasks(table_id, column_id)) {
        ret = OB_AI_COLUMN_HAS_UNFINISHED_TASKS;
        LOG_USER_ERROR(OB_AI_COLUMN_HAS_UNFINISHED_TASKS,
                       static_cast<long>(table_id), static_cast<long>(column_id));
        break;
      }
    }
  }
  return ret;
}

int ObAiColumnStrictRead::collect_ai_columns_of_stmt(
    ObDMLStmt &stmt,
    ObIArray<std::pair<uint64_t, uint64_t> > &ai_columns)
{
  int ret = OB_SUCCESS;
  if (stmt.is_select_stmt()) {
    if (OB_FAIL(collect_from_select(static_cast<ObSelectStmt &>(stmt), ai_columns))) {
    }
  } else {
    // an UPDATE or a DELETE: the where conditions and the assignments of the
    // statement may reference an AI column
    const ObIArray<ObRawExpr*> &conditions = stmt.get_condition_exprs();
    for (int64_t i = 0; OB_SUCC(ret) && i < conditions.count(); ++i) {
      if (OB_FAIL(collect_ai_columns(conditions.at(i), ai_columns))) {
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
