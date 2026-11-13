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
 *
 * The asynchronous worker of the AI columns.
 *
 * The worker polls the task queue (oceanbase.__all_ai_column_task) for the
 * tasks which are eligible, groups them by their request id (the AI columns
 * of one row share a request id) and fills the AI columns of every group:
 *
 *   - the normalized AI expression of the AI column is sent to the model as
 *     the system message, the evaluated prompt expression is sent as the user
 *     message,
 *   - the answer of the model is converted to the type of the AI column by
 *     the AI expression evaluation layer,
 *   - all the AI columns of the group are written in one transaction, so
 *     that they become visible together,
 *   - a result whose source row has been changed in the meantime is dropped,
 *   - a failed model call is retried, the state of the task is committed in
 *     its own transaction, so that the retry is visible to the other
 *     sessions.
 */

#ifndef OB_AI_COLUMN_WORKER_H_
#define OB_AI_COLUMN_WORKER_H_

#include <condition_variable>
#include <mutex>
#include <thread>

#include "lib/ob_define.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObSchemaGetterGuard;
}
}
namespace observer
{

/// The state of a task of the AI column queue.
enum ObAiColumnTaskState
{
  AI_COLUMN_TASK_PENDING = 0,  ///< just registered, waiting for the worker
  AI_COLUMN_TASK_RUNNING = 1,  ///< the worker is filling the AI column
  AI_COLUMN_TASK_DONE = 2,     ///< the AI column has been filled
  AI_COLUMN_TASK_RETRY = 3,    ///< the model call failed, the task is retried
  AI_COLUMN_TASK_FAILED = 4,   ///< the retries are exhausted
};

/// One row of oceanbase.__all_ai_column_task.
struct ObAiColumnTask
{
  int64_t task_id_;
  int64_t table_id_;
  int64_t column_id_;
  common::ObString request_id_;
  int64_t retry_count_;
  common::ObString rowkey_predicate_;
  int64_t state_;
  int64_t eligible_ts_;
  bool processed_;

  ObAiColumnTask()
      : task_id_(0),
        table_id_(0),
        column_id_(0),
        request_id_(),
        retry_count_(0),
        rowkey_predicate_(),
        state_(AI_COLUMN_TASK_PENDING),
        eligible_ts_(0),
        processed_(false)
  {}

  TO_STRING_KV(K_(task_id), K_(table_id), K_(column_id), K_(request_id), K_(retry_count),
               K_(rowkey_predicate), K_(state), K_(eligible_ts));
};

/// The AI column which is filled by one task.
struct ObAiColumnTarget
{
  int64_t task_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  common::ObString column_name_;
  common::ObString expr_;
  common::ObString normalized_expr_;
  common::ObString rowkey_predicate_;
  common::ObObjType data_type_;

  ObAiColumnTarget()
      : task_id_(0),
        retry_count_(0),
        max_retries_(0),
        column_name_(),
        expr_(),
        normalized_expr_(),
        rowkey_predicate_(),
        data_type_(common::ObMaxType)
  {}

  TO_STRING_KV(K_(task_id), K_(column_name), K_(data_type), K_(max_retries));
};

class ObAiColumnWorker
{
public:
  ObAiColumnWorker();
  ~ObAiColumnWorker();

  int init();
  int start();
  void stop();
  void wait();
  void destroy();

private:
  void worker_loop_();
  int process_round_();
  int collect_tasks_(common::ObIAllocator &allocator, common::ObIArray<ObAiColumnTask> &tasks);
  int process_group_(common::ObIAllocator &allocator, common::ObIArray<ObAiColumnTask> &group);
  int resolve_group_(common::ObIAllocator &allocator,
                     share::schema::ObSchemaGetterGuard &schema_guard,
                     common::ObIArray<ObAiColumnTask> &group,
                     common::ObIArray<ObAiColumnTarget> &targets,
                     common::ObString &db_name,
                     common::ObString &table_name);
  int check_stale_(const common::ObIArray<ObAiColumnTask> &group, bool &is_stale);
  int publish_group_(const common::ObString &db_name,
                     const common::ObString &table_name,
                     const common::ObIArray<ObAiColumnTarget> &targets);
  int retry_group_(const common::ObIArray<ObAiColumnTask> &group);
  int discard_group_(const common::ObIArray<ObAiColumnTask> &group);

private:
  static constexpr int64_t POLL_INTERVAL_US = 100 * 1000;
  // the task is not retried before the delay has passed, so that the state
  // of the task (RETRY) is visible to the other sessions for a while
  static constexpr int64_t RETRY_DELAY_S = 1;
  static constexpr int64_t MAX_TASK_CNT_PER_ROUND = 64;
  static constexpr int64_t MAX_GROUP_CNT = 16;

  bool is_inited_;
  bool is_started_;
  bool is_stopped_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cond_;

  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);
};

} // end namespace observer
} // end namespace oceanbase

#endif // OB_AI_COLUMN_WORKER_H_
