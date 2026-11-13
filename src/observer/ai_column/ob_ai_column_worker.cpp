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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_column/ob_ai_column_worker.h"

#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/allocator/ob_arena_allocator.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/utility.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_ai_column_eval_ctx.h"
#include "share/schema/ob_ai_column_task_service.h"
#include "share/schema/ob_ai_column_util.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace observer
{

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

/**
 * Append a string literal to a SQL statement.
 *
 * The string is appended as a quoted literal whose special characters are
 * escaped, so that the value of the column is read back unchanged.
 */
static int sql_append_hex_escape_str(const ObString &value, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if ('\'' == c) {
        ret = sql.append("''");
      } else if ('\\' == c) {
        ret = sql.append("\\\\");
      } else {
        ret = sql.append(&c, 1);
      }
    }
    if (OB_SUCC(ret)) {
      ret = sql.append("'");
    }
  }
  return ret;
}

ObAiColumnWorker::ObAiColumnWorker()
    : is_inited_(false),
      is_started_(false),
      is_stopped_(false),
      thread_(),
      mutex_(),
      cond_()
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (is_started_) {
    ret = OB_INIT_TWICE;
  } else {
    is_started_ = true;
    is_stopped_ = false;
    thread_ = std::thread(&ObAiColumnWorker::worker_loop_, this);
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  {
    std::lock_guard<std::mutex> guard(mutex_);
    is_stopped_ = true;
  }
  cond_.notify_all();
}

void ObAiColumnWorker::wait()
{
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ObAiColumnWorker::destroy()
{
  stop();
  wait();
  is_started_ = false;
  is_inited_ = false;
}

void ObAiColumnWorker::worker_loop_()
{
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (is_stopped_) {
        break;
      }
      cond_.wait_for(lock, std::chrono::microseconds(POLL_INTERVAL_US));
      if (is_stopped_) {
        break;
      }
    }
    int ret = process_round_();
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to process the tasks of the ai column queue", K(ret));
    }
  }
}

// Read the tasks which are eligible.  A task is eligible when its state is
// PENDING (it has just been registered) or RETRY (the last model call
// failed) and when the time it may be picked up has passed.
int ObAiColumnWorker::collect_tasks_(ObIAllocator &allocator, ObIArray<ObAiColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  const int64_t now_s = ObTimeUtility::current_time() / 1000000;
  ObSqlString sql;
  SMART_VAR(ObMySQLProxy::MySQLResult, res)
  {
    ObMySQLResult *result = NULL;
    if (OB_FAIL(sql.assign_fmt("SELECT task_id, table_id, column_id, request_id, retry_count, "
                                "rowkey_predicate, state, eligible_ts FROM %s "
                                "WHERE state IN (%d, %d, %d) AND eligible_ts <= %ld "
                                "ORDER BY task_id LIMIT %ld",
                                OB_ALL_AI_COLUMN_TASK_TNAME,
                                static_cast<int32_t>(AI_COLUMN_TASK_PENDING),
                                static_cast<int32_t>(AI_COLUMN_TASK_RUNNING),
                                static_cast<int32_t>(AI_COLUMN_TASK_RETRY),
                                now_s,
                                MAX_TASK_CNT_PER_ROUND))) {
    } else if (OB_FAIL(GCTX.sql_proxy_.read(res, sql.ptr()))) {
      LOG_WARN("failed to read the tasks of the ai column queue", K(ret), K(sql));
    } else if (NULL == (result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int tmp_ret = OB_SUCCESS;
      while (OB_SUCC(ret) && OB_SUCC(tmp_ret = result->next())) {
        ObAiColumnTask task;
        int64_t task_id = 0;
        int64_t table_id = 0;
        int64_t column_id = 0;
        int64_t retry_count = 0;
        int64_t state = 0;
        int64_t eligible_ts = 0;
        ObString request_id;
        ObString rowkey_predicate;
        EXTRACT_INT_FIELD_MYSQL(*result, "task_id", task_id, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "table_id", table_id, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "column_id", column_id, int64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "request_id", request_id);
        EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", retry_count, int64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", rowkey_predicate);
        EXTRACT_INT_FIELD_MYSQL(*result, "state", state, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "eligible_ts", eligible_ts, int64_t);
        if (OB_SUCC(ret)) {
          task.task_id_ = task_id;
          task.table_id_ = table_id;
          task.column_id_ = column_id;
          task.retry_count_ = retry_count;
          task.state_ = state;
          task.eligible_ts_ = eligible_ts;
          // the strings of the result are only valid while the result is
          // alive, so they are copied into the allocator of the round
          if (OB_FAIL(ob_write_string(allocator, request_id, task.request_id_))) {
          } else if (OB_FAIL(ob_write_string(allocator, rowkey_predicate, task.rowkey_predicate_))) {
          } else if (OB_FAIL(tasks.push_back(task))) {
          }
        }
      }
      if (OB_SUCC(ret) && OB_ITER_END != tmp_ret) {
        ret = tmp_ret;
      }
    }
  }
  return ret;
}

// Resolve the AI columns of the group: the table of the tasks, the name of
// the AI column, the AI expression and the normalized AI expression (which
// is sent to the model as the system message).
int ObAiColumnWorker::resolve_group_(ObIAllocator &allocator,
                                      ObSchemaGetterGuard &schema_guard,
                                      ObIArray<ObAiColumnTask> &group,
                                      ObIArray<ObAiColumnTarget> &targets,
                                      ObString &db_name,
                                      ObString &table_name)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *db_schema = NULL;
  if (group.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(schema_guard.get_table_schema(group.at(0).table_id_, table_schema))) {
    LOG_WARN("failed to get the schema of the table of the ai column", K(ret),
             K(group.at(0).table_id_));
  } else if (OB_ISNULL(table_schema) || !table_schema->is_user_table()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table of the ai column does not exist", K(ret), K(group.at(0).table_id_));
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
    LOG_WARN("failed to get the schema of the database of the ai column", K(ret));
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    db_name = db_schema->get_database_name_str();
    table_name = table_schema->get_table_name_str();
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      const ObAiColumnTask &task = group.at(i);
      const ObColumnSchemaV2 *column = table_schema->get_column_schema(task.column_id_);
      if (OB_ISNULL(column) || !column->is_ai_column()
          || column->get_ai_column_expr().empty()) {
        // the column is not an AI column anymore
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the column of the ai column task is not an ai column", K(ret),
                 K(task.table_id_), K(task.column_id_));
      } else {
        ObAiColumnTarget target;
        target.task_id_ = task.task_id_;
        target.retry_count_ = task.retry_count_;
        target.max_retries_ = column->get_ai_max_retries();
        target.column_name_ = column->get_column_name_str();
        target.expr_ = column->get_ai_column_expr();
        target.normalized_expr_ = column->get_ai_normalized_expr();
        target.rowkey_predicate_ = task.rowkey_predicate_;
        target.data_type_ = column->get_data_type();
        if (target.normalized_expr_.empty()) {
          // the normalized expression is built from the AI expression and the
          // type of the AI column
          if (OB_FAIL(ObAiColumnUtil::build_normalized_expr(allocator, target.expr_, *column,
                                                              target.normalized_expr_))) {
          } else if (OB_FAIL(targets.push_back(target))) {
          }
        } else if (OB_FAIL(targets.push_back(target))) {
        }
      }
    }
  }
  return ret;
}

// The stale guard: the source row of the task must not have been changed
// since the task was registered.  A changed source row invalidates the AI
// column and registers a new task, so the existence of another task for the
// same row and the same AI column means that the result of this task is
// outdated and must not be published.
int ObAiColumnWorker::check_stale_(const ObIArray<ObAiColumnTask> &group, bool &is_stale)
{
  int ret = OB_SUCCESS;
  is_stale = false;
  ObSqlString sql;
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const ObAiColumnTask &task = group.at(i);
    int64_t count = 0;
    SMART_VAR(ObMySQLProxy::MySQLResult, res)
    {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(sql.assign_fmt("SELECT count(*) FROM %s WHERE table_id = %ld AND column_id = %ld "
                                  "AND rowkey_predicate = ",
                                  OB_ALL_AI_COLUMN_TASK_TNAME, task.table_id_, task.column_id_))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(task.rowkey_predicate_, sql))) {
      } else if (OB_FAIL(sql.append(" AND request_id != "))) {
      } else if (OB_FAIL(sql_append_hex_escape_str(task.request_id_, sql))) {
      } else if (OB_FAIL(GCTX.sql_proxy_.read(res, sql.ptr()))) {
        LOG_WARN("failed to check the stale tasks of the ai column queue", K(ret), K(sql));
      } else if (NULL == (result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END != ret) {
          LOG_WARN("failed to read the stale tasks of the ai column queue", K(ret), K(sql));
        } else {
          ret = OB_SUCCESS;
        }
      } else if (OB_FAIL(result->get_int(0, count))) {
        LOG_WARN("failed to read the stale tasks of the ai column queue", K(ret), K(sql));
      } else if (count > 0) {
        is_stale = true;
      }
    }
    if (is_stale) {
      break;
    }
  }
  return ret;
}

// Fill the AI columns of the group.  All the AI columns of the group are
// written in one transaction, so that they become visible together.
//
// The task rows are removed before the AI columns are filled: the task table
// is locked before the table which owns the AI column, so that the worker
// and the DML which invalidates the AI column do not deadlock.
int ObAiColumnWorker::publish_group_(const ObString &db_name,
                                     const ObString &table_name,
                                     const ObIArray<ObAiColumnTarget> &targets)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  int64_t affected_rows = 0;
  if (targets.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(&GCTX.sql_proxy_))) {
    LOG_WARN("failed to start the transaction of the ai column worker", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < targets.count(); ++i) {
      if (OB_FAIL(ObAiColumnTaskService::delete_task(trans, targets.at(i).task_id_))) {
        LOG_WARN("failed to delete the task of the ai column", K(ret),
                 K(targets.at(i).task_id_));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < targets.count(); ++i) {
      const ObAiColumnTarget &target = targets.at(i);
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt("UPDATE `%.*s`.`%.*s` SET `%.*s` = (%.*s) WHERE %.*s",
                                  db_name.length(), db_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  target.column_name_.length(), target.column_name_.ptr(),
                                  target.expr_.length(), target.expr_.ptr(),
                                  target.rowkey_predicate_.length(),
                                  target.rowkey_predicate_.ptr()))) {
        LOG_WARN("failed to build the statement of the ai column", K(ret));
      } else {
        // the AI expression evaluation layer sends the normalized AI
        // expression as the system message of the model request and converts
        // the answer of the model to the type of the AI column
        ObAiColumnEvalCtx::set_system_prompt(target.normalized_expr_);
        ObAiColumnEvalCtx::set_target_type(target.data_type_);
        ret = trans.write(sql.ptr(), affected_rows);
        ObAiColumnEvalCtx::clear();
        if (OB_FAIL(ret)) {
          LOG_WARN("failed to fill the ai column", K(ret), K(sql));
        }
      }
    }
    int end_ret = trans.end(OB_SUCC(ret));
    if (OB_FAIL(end_ret)) {
      LOG_WARN("failed to commit the ai column", K(end_ret), K(ret));
      ret = end_ret;
    }
  }
  return ret;
}

// The model call failed.  The task is marked as RETRY in its own transaction,
// so that the state of the task is visible to the other sessions, and it is
// picked up again later.  When the retries are exhausted, the task is
// removed and the AI column keeps its NULL value.
int ObAiColumnWorker::retry_group_(const ObIArray<ObAiColumnTask> &group)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  int64_t affected_rows = 0;
  const int64_t eligible_ts = ObTimeUtility::current_time() / 1000000 + RETRY_DELAY_S;
  if (group.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(&GCTX.sql_proxy_))) {
    LOG_WARN("failed to start the transaction of the ai column worker", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      const ObAiColumnTask &task = group.at(i);
      ObDMLSqlSplicer sql;
      ObSqlString buffer;
      if (OB_FAIL(sql.add_pk_column("task_id", task.task_id_))) {
      } else if (OB_FAIL(sql.add_column("state", static_cast<int64_t>(AI_COLUMN_TASK_RETRY)))) {
      } else if (OB_FAIL(sql.add_column("retry_count", task.retry_count_ + 1))) {
      } else if (OB_FAIL(sql.add_column("eligible_ts", eligible_ts))) {
      } else if (OB_FAIL(sql.splice_update_sql(OB_ALL_AI_COLUMN_TASK_TNAME, buffer))) {
      } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
        LOG_WARN("failed to mark the task of the ai column as retry", K(ret), K(buffer));
      }
    }
    int end_ret = trans.end(OB_SUCC(ret));
    if (OB_FAIL(end_ret)) {
      LOG_WARN("failed to commit the retry of the ai column", K(end_ret), K(ret));
      ret = end_ret;
    }
  }
  return ret;
}

// The task is not needed anymore: it is removed from the queue.
int ObAiColumnWorker::discard_group_(const ObIArray<ObAiColumnTask> &group)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  int64_t affected_rows = 0;
  if (group.count() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(&GCTX.sql_proxy_))) {
    LOG_WARN("failed to start the transaction of the ai column worker", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
      if (OB_FAIL(ObAiColumnTaskService::delete_task(trans, group.at(i).task_id_))) {
        LOG_WARN("failed to delete the task of the ai column", K(ret),
                 K(group.at(i).task_id_));
      }
    }
    int end_ret = trans.end(OB_SUCC(ret));
    if (OB_FAIL(end_ret)) {
      LOG_WARN("failed to commit the deletion of the ai column task", K(end_ret), K(ret));
      ret = end_ret;
    }
  }
  return ret;
}

// Fill the AI columns of one request group.
int ObAiColumnWorker::process_group_(ObIAllocator &allocator, ObIArray<ObAiColumnTask> &group)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAiColumnTarget, MAX_GROUP_CNT> targets;
  ObString db_name;
  ObString table_name;
  // the schema guard is kept alive while the AI columns are filled, the
  // names of the table and of the AI columns belong to the schema
  ObSchemaGetterGuard schema_guard;
  if (OB_FAIL(resolve_group_(allocator, schema_guard, group, targets, db_name, table_name))) {
    // the AI column does not exist anymore, the task is dropped
    LOG_WARN("failed to resolve the ai column of the task", K(ret));
    int tmp_ret = discard_group_(group);
    if (OB_FAIL(tmp_ret)) {
      LOG_WARN("failed to drop the task of the ai column", K(tmp_ret));
    }
  } else {
    bool is_stale = false;
    if (OB_FAIL(check_stale_(group, is_stale))) {
      LOG_WARN("failed to check the stale tasks of the ai column", K(ret));
    } else if (is_stale) {
      // the source row has been changed in the meantime, the result of this
      // task is dropped, the new task of the row fills the AI column
      LOG_INFO("the result of the ai column task is dropped, the source row has been changed",
               K(group.at(0).request_id_));
      int tmp_ret = discard_group_(group);
      if (OB_FAIL(tmp_ret)) {
        LOG_WARN("failed to drop the stale task of the ai column", K(tmp_ret));
      }
    } else if (OB_FAIL(ret = publish_group_(db_name, table_name, targets))) {
      // the model call failed, the task is retried or dropped
      bool can_retry = false;
      for (int64_t i = 0; i < targets.count(); ++i) {
        if (targets.at(i).retry_count_ < targets.at(i).max_retries_) {
          can_retry = true;
          break;
        }
      }
      int tmp_ret = OB_SUCCESS;
      if (can_retry) {
        tmp_ret = retry_group_(group);
      } else {
        LOG_INFO("the retries of the ai column task are exhausted", K(ret),
                 K(group.at(0).request_id_));
        tmp_ret = discard_group_(group);
      }
      if (OB_FAIL(tmp_ret)) {
        LOG_WARN("failed to handle the failed task of the ai column", K(tmp_ret));
      }
    }
  }
  return ret;
}

// One round of the worker: read the eligible tasks and fill the AI columns
// of every request group.
int ObAiColumnWorker::process_round_()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnWorker", OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<ObAiColumnTask, MAX_TASK_CNT_PER_ROUND> tasks;
  if (OB_FAIL(collect_tasks_(allocator, tasks))) {
    LOG_WARN("failed to collect the tasks of the ai column queue", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      if (tasks.at(i).processed_) {
        continue;
      }
      // the AI columns of one row share their request id, they are filled
      // together
      ObSEArray<ObAiColumnTask, MAX_GROUP_CNT> group;
      for (int64_t j = i; OB_SUCC(ret) && j < tasks.count(); ++j) {
        if (!tasks.at(j).processed_
            && tasks.at(j).request_id_.length() == tasks.at(i).request_id_.length()
            && 0 == tasks.at(j).request_id_.compare(tasks.at(i).request_id_)) {
          tasks.at(j).processed_ = true;
          if (OB_FAIL(group.push_back(tasks.at(j)))) {
            break;
          }
        }
      }
      if (OB_SUCC(ret)) {
        int tmp_ret = process_group_(allocator, group);
        if (OB_FAIL(tmp_ret)) {
          LOG_WARN("failed to process the group of the ai column tasks", K(tmp_ret));
        }
      }
    }
  }
  return ret;
}

} // end namespace observer
} // end namespace oceanbase
