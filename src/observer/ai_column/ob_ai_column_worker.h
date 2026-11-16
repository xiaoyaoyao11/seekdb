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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_

#include "share/ob_thread_pool.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
}
namespace observer
{

// Background consumer of the observable AI column task table. It evaluates the
// normalized AI expressions and publishes the results in one transaction per task
// group, using the hidden request id as a version fence.
class ObAIColumnWorker : public share::ObThreadPool
{
public:
  ObAIColumnWorker();
  virtual ~ObAIColumnWorker();
  int init(common::ObMySQLProxy *sql_proxy);
  int start();
  void stop();
  void wait();
  void destroy();
protected:
  void run1() override;
private:
  struct AITask
  {
    int64_t table_id_;
    int64_t column_id_;
    common::ObString database_name_;
    common::ObString table_name_;
    common::ObString column_name_;
    common::ObString rowkey_predicate_;
    int64_t request_id_;
    int64_t state_;
    int64_t retry_count_;
    int64_t max_retries_;
  };
  int do_work_();
  int reset_running_tasks_();
  int load_tasks_(common::ObIAllocator &alloc,
                  common::ObIArray<AITask> &tasks,
                  bool &has_more);
  int process_group_(const common::ObIArray<AITask> &tasks,
                     const int64_t begin,
                     const int64_t end);
  int update_task_state_(const AITask &task, const int64_t state);
  int delete_group_tasks_(const AITask &task);
  int retry_group_tasks_(const common::ObIArray<AITask> &tasks,
                         const int64_t begin,
                         const int64_t end);
private:
  common::ObMySQLProxy *sql_proxy_ = nullptr;
  bool inited_ = false;
  bool tables_ready_ = false;
  bool is_processing_ = false;
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
