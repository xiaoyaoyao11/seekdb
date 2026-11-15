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

#include "observer/ai_column/ob_ai_column_worker.h"

#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/allocator/page_arena.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/time/ob_time_utility.h"
#include "share/ob_server_status.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_ai_column_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace observer
{

namespace
{

const int64_t AI_WORKER_IDLE_INTERVAL_US = 100 * 1000;
const int64_t AI_WORKER_STARTUP_GRACE_US = 3 * 1000 * 1000;
const int64_t AI_WORKER_BATCH_SIZE = 64;
const int64_t AI_WORKER_RETRY_DELAY_US = 300 * 1000;

int write_sql(ObMySQLProxy &proxy, const ObString &sql, int64_t &affected_rows)
{
  return proxy.write(sql, affected_rows);
}

} // namespace

ObAIColumnWorker::ObAIColumnWorker()
{
}

ObAIColumnWorker::~ObAIColumnWorker()
{
}

int ObAIColumnWorker::init(ObMySQLProxy *sql_proxy)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(sql_proxy)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    sql_proxy_ = sql_proxy;
    inited_ = true;
  }
  return ret;
}

int ObAIColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(share::ObThreadPool::start())) {
    LOG_WARN("failed to start ai column worker", K(ret));
  }
  return ret;
}

void ObAIColumnWorker::stop()
{
  share::ObThreadPool::stop();
}

void ObAIColumnWorker::wait()
{
  share::ObThreadPool::wait();
}

void ObAIColumnWorker::destroy()
{
  stop();
  wait();
  inited_ = false;
}

void ObAIColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  LOG_INFO("ai column worker started");
  // The worker is created while the server is still starting, before the SQL
  // layer and the root service can serve statements. Issuing internal SQL during
  // that window is pointless (it can only fail) and, on a slow boot, it can
  // interfere with the boot itself, so wait until the server is serving.
  while (!has_set_stop() && share::SS_SERVING != GCTX.status_) {
    ob_usleep(AI_WORKER_IDLE_INTERVAL_US);
  }
  if (has_set_stop()) {
    LOG_INFO("ai column worker stopped before serving");
    return;
  }
  // Give the very first statements of the instance a quiet window: the tables are
  // also created by the DDL path, so the worker does not have to race with the
  // first user DDL that follows the transition to serving.
  for (int64_t i = 0; !has_set_stop() && i < AI_WORKER_STARTUP_GRACE_US / AI_WORKER_IDLE_INTERVAL_US; ++i) {
    ob_usleep(AI_WORKER_IDLE_INTERVAL_US);
  }
  if (has_set_stop()) {
    LOG_INFO("ai column worker stopped before startup grace elapsed");
    return;
  }
  if (OB_FAIL(share::schema::ensure_ai_column_tables(*sql_proxy_))) {
    LOG_WARN("failed to ensure ai column tables, will retry later", K(ret));
  } else {
    tables_ready_ = true;
    int tmp_ret = reset_running_tasks_();
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to reset running ai column tasks", K(tmp_ret));
    }
  }
  // Table creation is retried by do_work_(), a failure here must not stop the worker.
  ret = OB_SUCCESS;
  while (!has_set_stop() && OB_SUCC(ret)) {
    if (share::SS_SERVING == GCTX.status_) {
      ret = do_work_();
      if (OB_SUCCESS != ret) {
        LOG_WARN("ai column worker round failed", K(ret));
        ret = OB_SUCCESS;
      }
    }
    ob_usleep(AI_WORKER_IDLE_INTERVAL_US);
  }
  LOG_INFO("ai column worker stopped");
}

int ObAIColumnWorker::alloc_read_result(common::ObIAllocator &alloc,
                                        common::ObISQLClient::ReadResult *&read_result)
{
  int ret = OB_SUCCESS;
  read_result = NULL;
  void *mem = alloc.alloc(sizeof(common::ObISQLClient::ReadResult));
  if (OB_ISNULL(mem)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate ai column read result", K(ret));
  } else {
    read_result = new (mem) common::ObISQLClient::ReadResult();
  }
  return ret;
}

int ObAIColumnWorker::reset_running_tasks_()
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  const char *sql = "update oceanbase.__all_ai_column_task set state = 0 where state = 1";
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(write_sql(*sql_proxy_, ObString::make_string(sql), affected_rows))) {
    LOG_WARN("failed to reset running ai column tasks", K(ret));
  }
  return ret;
}

int ObAIColumnWorker::do_work_()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<AITask, 16> tasks;
  bool has_more = false;
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (!tables_ready_) {
    if (OB_FAIL(share::schema::ensure_ai_column_tables(*sql_proxy_))) {
    } else {
      tables_ready_ = true;
      reset_running_tasks_();
    }
  } else if (OB_FAIL(load_tasks_(alloc, tasks, has_more))) {
  } else {
    int64_t begin = 0;
    while (OB_SUCC(ret) && begin < tasks.count()) {
      int64_t end = begin + 1;
      const AITask &first = tasks.at(begin);
      while (end < tasks.count()
             && tasks.at(end).request_id_ == first.request_id_
             && tasks.at(end).table_id_ == first.table_id_
             && 0 == tasks.at(end).rowkey_predicate_.compare(first.rowkey_predicate_)) {
        ++end;
      }
      if (OB_FAIL(process_group_(alloc, tasks, begin, end))) {
      } else {
        begin = end;
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::load_tasks_(ObIAllocator &alloc,
                                  ObIArray<AITask> &tasks,
                                  bool &has_more)
{
  int ret = OB_SUCCESS;
  has_more = false;
  ObSqlString sql;
  ObISQLClient::ReadResult *read_result = NULL;
  sqlclient::ObMySQLResult *result = NULL;
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(alloc_read_result(alloc, read_result))) {
  } else if (OB_FAIL(sql.append_fmt(
                 "select table_id, column_id, database_name, table_name, column_name,"
                 " rowkey_predicate, request_id, state, retry_count, max_retries"
                 " from oceanbase.__all_ai_column_task"
                 " where state in (0, 3) and eligible_ts <= now(6)"
                 " order by request_id, table_id, rowkey_predicate limit %ld",
                 AI_WORKER_BATCH_SIZE))) {
  } else if (OB_FAIL(sql_proxy_->read(*read_result, sql.ptr(), NULL))) {
    LOG_WARN("failed to read ai column tasks", K(ret));
  } else if (OB_ISNULL(result = read_result->get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(result->next())) {
      AITask task;
      ObString db_name;
      ObString table_name;
      ObString column_name;
      ObString predicate;
      if (OB_FAIL(result->get_int("table_id", task.table_id_))) {
      } else if (OB_FAIL(result->get_int("column_id", task.column_id_))) {
      } else if (OB_FAIL(result->get_varchar("database_name", db_name))) {
      } else if (OB_FAIL(result->get_varchar("table_name", table_name))) {
      } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
      } else if (OB_FAIL(result->get_varchar("rowkey_predicate", predicate))) {
      } else if (OB_FAIL(result->get_int("request_id", task.request_id_))) {
      } else if (OB_FAIL(result->get_int("state", task.state_))) {
      } else if (OB_FAIL(result->get_int("retry_count", task.retry_count_))) {
      } else if (OB_FAIL(result->get_int("max_retries", task.max_retries_))) {
      } else if (OB_FAIL(ob_write_string(alloc, db_name, task.database_name_))) {
      } else if (OB_FAIL(ob_write_string(alloc, table_name, task.table_name_))) {
      } else if (OB_FAIL(ob_write_string(alloc, column_name, task.column_name_))) {
      } else if (OB_FAIL(ob_write_string(alloc, predicate, task.rowkey_predicate_))) {
      } else if (OB_FAIL(tasks.push_back(task))) {
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      has_more = tasks.count() >= AI_WORKER_BATCH_SIZE;
    }
  }
  return ret;
}

int ObAIColumnWorker::update_task_state_(const AITask &task, const int64_t state)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("update oceanbase.__all_ai_column_task set state = %ld"
                             " where table_id = %ld and column_id = %ld"
                             " and request_id = %ld and rowkey_predicate = ",
                             state, task.table_id_, task.column_id_,
                             task.request_id_))) {
  } else if (OB_FAIL(share::schema::append_sql_literal(sql, task.rowkey_predicate_))) {
  } else if (OB_FAIL(write_sql(*sql_proxy_, sql.string(), affected_rows))) {
    LOG_WARN("failed to update ai column task state", K(ret));
  }
  return ret;
}

int ObAIColumnWorker::delete_group_tasks_(const AITask &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task"
                             " where table_id = %ld and request_id = %ld"
                             " and rowkey_predicate = ",
                             task.table_id_, task.request_id_))) {
  } else if (OB_FAIL(share::schema::append_sql_literal(sql, task.rowkey_predicate_))) {
  } else if (OB_FAIL(write_sql(*sql_proxy_, sql.string(), affected_rows))) {
    LOG_WARN("failed to delete ai column tasks", K(ret));
  }
  return ret;
}

int ObAIColumnWorker::delete_task_(const AITask &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task"
                             " where table_id = %ld and column_id = %ld"
                             " and request_id = %ld and rowkey_predicate = ",
                             task.table_id_, task.column_id_,
                             task.request_id_))) {
  } else if (OB_FAIL(share::schema::append_sql_literal(sql, task.rowkey_predicate_))) {
  } else if (OB_FAIL(write_sql(*sql_proxy_, sql.string(), affected_rows))) {
    LOG_WARN("failed to delete finished ai column task", K(ret));
  }
  return ret;
}

int ObAIColumnWorker::retry_group_tasks_(const ObIArray<AITask> &tasks,
                                         const ObIArray<int64_t> &indexes)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < indexes.count(); ++i) {
    const AITask &task = tasks.at(indexes.at(i));
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (task.retry_count_ < task.max_retries_) {
      if (OB_FAIL(sql.append_fmt("update oceanbase.__all_ai_column_task"
                                 " set state = %ld, retry_count = retry_count + 1,"
                                 " eligible_ts = now(6) + interval %ld microsecond"
                                 " where table_id = %ld and column_id = %ld"
                                 " and request_id = %ld and rowkey_predicate = ",
                                 share::schema::OB_AI_TASK_RETRYING,
                                 AI_WORKER_RETRY_DELAY_US, task.table_id_,
                                 task.column_id_, task.request_id_))) {
      } else if (OB_FAIL(share::schema::append_sql_literal(sql,
                                                           task.rowkey_predicate_))) {
      } else if (OB_FAIL(write_sql(*sql_proxy_, sql.string(), affected_rows))) {
        LOG_WARN("failed to mark ai column task retrying", K(ret));
      }
    } else {
      // The retry budget is exhausted: the column reaches its terminal state.
      // Record FAILED on the row itself before the task row is removed, so the
      // failure stays observable and a later RETRY can select it.
      int tmp_ret = mark_row_failed_(task);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to mark ai column row as failed", K(tmp_ret));
      }
      if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task"
                                 " where table_id = %ld and column_id = %ld"
                                 " and request_id = %ld and rowkey_predicate = ",
                                 task.table_id_, task.column_id_,
                                 task.request_id_))) {
      } else if (OB_FAIL(share::schema::append_sql_literal(sql,
                                                           task.rowkey_predicate_))) {
      } else if (OB_FAIL(write_sql(*sql_proxy_, sql.string(), affected_rows))) {
        LOG_WARN("failed to drop failed ai column task", K(ret));
      }
    }
  }
  return ret;
}

int ObAIColumnWorker::mark_row_failed_(const AITask &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  // The request id fence keeps a stale failure from overwriting the state of a
  // row that has already been regenerated by a newer request.
  if (OB_FAIL(sql.append(share::schema::OB_AI_COLUMN_INTERNAL_MARKER))) {
  } else if (OB_FAIL(sql.append_fmt("update `%.*s`.`%.*s` set `__ai_state_%.*s` = %ld"
                                     " where `__ai_request_%.*s` = %ld and ",
                                     task.database_name_.length(),
                                     task.database_name_.ptr(),
                                     task.table_name_.length(),
                                     task.table_name_.ptr(),
                                     task.column_name_.length(),
                                     task.column_name_.ptr(),
                                     static_cast<int64_t>(share::schema::OB_AI_TASK_FAILED),
                                     task.column_name_.length(),
                                     task.column_name_.ptr(),
                                     task.request_id_))) {
  } else if (OB_FAIL(sql.append(task.rowkey_predicate_))) {
  } else if (OB_FAIL(write_sql(*sql_proxy_, sql.string(), affected_rows))) {
    LOG_WARN("failed to mark ai column row as failed", K(ret));
  }
  return ret;
}

int ObAIColumnWorker::process_group_(common::ObIAllocator &alloc,
                                     const common::ObIArray<AITask> &tasks,
                                     const int64_t begin,
                                     const int64_t end)
{
  int ret = OB_SUCCESS;
  bool abort_group = false;
  ObSqlString update_sql;
  ObSqlString running_sql;
  int64_t affected_rows = 0;
  const AITask &first = tasks.at(begin);
  // Mark all tasks of the group as running so that SHOW AI COLUMNS can observe it.
  if (OB_FAIL(running_sql.append_fmt("update oceanbase.__all_ai_column_task set state = %ld"
                                     " where table_id = %ld and request_id = %ld"
                                     " and rowkey_predicate = ",
                                     share::schema::OB_AI_TASK_RUNNING,
                                     first.table_id_, first.request_id_))) {
  } else if (OB_FAIL(share::schema::append_sql_literal(running_sql,
                                                       first.rowkey_predicate_))) {
  } else if (OB_FAIL(write_sql(*sql_proxy_, running_sql.string(), affected_rows))) {
  } else if (OB_FAIL(update_sql.append(
                 share::schema::OB_AI_COLUMN_INTERNAL_MARKER))) {
  } else if (OB_FAIL(update_sql.append_fmt("update `%.*s`.`%.*s` set ",
                                           first.database_name_.length(),
                                           first.database_name_.ptr(),
                                           first.table_name_.length(),
                                           first.table_name_.ptr()))) {
  }
  // Every AI column of this row is requested on its own statement, so that one
  // failing model call does not take its siblings down with it.  The group is
  // published once every column reached a terminal state: the columns that
  // succeeded become READY, the ones that failed become FAILED.
  ObSEArray<int64_t, 8> succeeded;   // indexes into [begin, end)
  ObSEArray<int64_t, 8> failed;
  for (int64_t i = begin; OB_SUCC(ret) && !abort_group && i < end; ++i) {
    const AITask &task = tasks.at(i);
    ObSqlString meta_sql;
    ObISQLClient::ReadResult *read_result = NULL;
    sqlclient::ObMySQLResult *result = NULL;
    ObString normalized_expr;
    int column_ret = OB_SUCCESS;
    if (OB_FAIL(alloc_read_result(alloc, read_result))) {
      column_ret = ret;
    } else if (OB_FAIL(meta_sql.append_fmt(
            "select normalized_expr from oceanbase.__all_ai_column"
            " where table_id = %ld and column_id = %ld",
            task.table_id_, task.column_id_))) {
      column_ret = ret;
    } else if (OB_FAIL(sql_proxy_->read(*read_result, meta_sql.ptr(), NULL))) {
      column_ret = ret;
    } else if (OB_ISNULL(result = read_result->get_result())) {
      column_ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END == ret) {
        // The column is gone: drop the whole group, it cannot be published.
        ret = OB_SUCCESS;
        abort_group = true;
        int tmp_ret = update_task_state_(task, share::schema::OB_AI_TASK_FAILED);
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("failed to mark orphan ai column task", K(tmp_ret));
        }
        tmp_ret = delete_group_tasks_(first);
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("failed to delete orphan ai column tasks", K(tmp_ret));
        }
      } else {
        column_ret = ret;
      }
    } else if (OB_FAIL(result->get_varchar("normalized_expr", normalized_expr))) {
      column_ret = ret;
    } else {
      // Request this single column.  The model call is evaluated by the update
      // itself, so a failure of the call only affects this column.
      ObSqlString column_sql;
      int64_t column_affected = 0;
      if (OB_FAIL(column_sql.append(share::schema::OB_AI_COLUMN_INTERNAL_MARKER))) {
        column_ret = ret;
      } else if (OB_FAIL(column_sql.append_fmt(
              "update `%.*s`.`%.*s` set `%.*s` = (%.*s), `__ai_state_%.*s` = %d"
              " where %.*s and `__ai_request_%.*s` = %ld",
              first.database_name_.length(),
              first.database_name_.ptr(),
              first.table_name_.length(),
              first.table_name_.ptr(),
              task.column_name_.length(),
              task.column_name_.ptr(),
              normalized_expr.length(),
              normalized_expr.ptr(),
              task.column_name_.length(),
              task.column_name_.ptr(),
              static_cast<int>(share::schema::OB_AI_TASK_READY),
              first.rowkey_predicate_.length(),
              first.rowkey_predicate_.ptr(),
              task.column_name_.length(),
              task.column_name_.ptr(),
              task.request_id_))) {
        column_ret = ret;
      } else if (OB_FAIL(write_sql(*sql_proxy_, column_sql.string(), column_affected))) {
        // The model call failed for this column only.
        LOG_WARN("failed to generate ai column result", K(ret), K(column_sql));
        column_ret = ret;
        ret = OB_SUCCESS;
      } else if (0 == column_affected) {
        // The request id fence rejected the write: a newer request already
        // replaced this row, so this result is stale and must not be published.
        column_ret = OB_ERR_UNEXPECTED;
      } else {
        column_ret = OB_SUCCESS;
      }
    }
    if (OB_SUCC(ret) && !abort_group) {
      if (OB_SUCCESS == column_ret) {
        ret = succeeded.push_back(i);
      } else {
        ret = failed.push_back(i);
      }
    }
  }
  if (OB_FAIL(ret) || abort_group) {
  } else if (!failed.empty()) {
    // The columns that could not be generated keep their own outcome: they are
    // retried on their own budget, so a failing column does not hold back the
    // ones that succeeded.
    int tmp_ret = retry_group_tasks_(tasks, failed);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to handle ai column retry", K(tmp_ret));
    }
  }
  if (OB_SUCC(ret) && !abort_group) {
    // The columns that succeeded are done, their task rows are removed.  The
    // failed ones keep their task row while they are still retryable.
    for (int64_t i = 0; OB_SUCC(ret) && i < succeeded.count(); ++i) {
      int tmp_ret = delete_task_(tasks.at(succeeded.at(i)));
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to delete finished ai column task", K(tmp_ret));
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
