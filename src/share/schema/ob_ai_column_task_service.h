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

#ifndef OB_AI_COLUMN_TASK_SERVICE_H_
#define OB_AI_COLUMN_TASK_SERVICE_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "share/ob_dml_sql_splicer.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

/**
 * @file ob_ai_column_task_service.h
 * @brief Read and write access to the oceanbase.__all_ai_column_task queue.
 *
 * The task rows are written through the SQL client of the caller, so that
 * they take part in the transaction which invalidated the AI column: they are
 * visible to that transaction and they are removed when it rolls back.
 */
class ObAiColumnTaskService
{
public:
  ObAiColumnTaskService() = delete;
  ~ObAiColumnTaskService() = delete;

  /**
   * Insert one task row.
   *
   * @param sql_client   the client whose transaction the row joins
   * @param task_id      the unique id of the task
   * @param table_id     the id of the table which owns the AI column
   * @param column_id    the id of the AI column
   * @param request_id   the id of the request group the task belongs to
   * @param retry_count  how many times the task has been retried already
   * @param rowkey_predicate the primary key predicate of the affected row
   * @param state        the task state
   * @param eligible_ts  the timestamp from which the task may be picked up
   */
  static int insert_task(common::ObISQLClient &sql_client,
                          const int64_t task_id,
                          const int64_t table_id,
                          const int64_t column_id,
                          const common::ObString &request_id,
                          const int64_t retry_count,
                          const common::ObString &rowkey_predicate,
                          const int64_t state,
                          const int64_t eligible_ts);

  /**
   * Delete the task rows of one table (or of one table and column).
   */
  static int delete_tasks(common::ObISQLClient &sql_client,
                           const int64_t table_id,
                           const int64_t column_id);

  /**
   * Delete the task rows which address one row through its primary key
   * predicate.  It is used when a row is deleted, so that its pending tasks
   * do not survive the row.
   */
  static int delete_tasks_by_predicate(common::ObISQLClient &sql_client,
                                        const common::ObString &rowkey_predicate);

  /**
   * Delete one task row by its id.
   */
  static int delete_task(common::ObISQLClient &sql_client, const int64_t task_id);

  /**
   * Delete the task rows of one row of a table.
   *
   * The row is identified by the primary key predicate which is stored in the
   * task row, it is used when a row is deleted.
   */
  static int delete_tasks_by_rowkey(common::ObISQLClient &sql_client,
                                     const int64_t table_id,
                                     const common::ObString &rowkey_predicate);

  /**
   * Update the state of one task row.
   *
   * The worker uses it to mark a task as RETRY, the new state is committed in
   * its own transaction, so that it is visible to the other sessions.
   */
  static int update_task_state(common::ObISQLClient &sql_client,
                                const int64_t task_id,
                                const int64_t state,
                                const int64_t retry_count,
                                const int64_t eligible_ts);
};

} // end namespace schema
} // end namespace share
} // end namespace oceanbase

#endif // OB_AI_COLUMN_TASK_SERVICE_H_
