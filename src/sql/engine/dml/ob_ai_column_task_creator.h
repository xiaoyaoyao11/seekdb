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

#ifndef OB_AI_COLUMN_TASK_CREATOR_H_
#define OB_AI_COLUMN_TASK_CREATOR_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace sql
{

class ObDMLModifyRowNode;
class ObTableModifyOp;

/**
 * @file ob_ai_column_task_creator.h
 * @brief Create the AI column tasks of the rows which a DML statement touched.
 *
 * The tasks are written through the SQL client of the user's session, so they
 * take part in the transaction of the DML statement: they are visible to that
 * transaction and they disappear when it rolls back.
 */
class ObAiColumnTaskCreator
{
public:
  ObAiColumnTaskCreator() = delete;
  ~ObAiColumnTaskCreator() = delete;

  /**
   * Create the AI column tasks of one modified row.
   *
   * @param modify_row the row which the DML statement touched
   * @param op         the DML operator, it provides the inner SQL connection
   *                   which shares the user's transaction
   */
  static int create_tasks(ObDMLModifyRowNode &modify_row, ObTableModifyOp &op);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OB_AI_COLUMN_TASK_CREATOR_H_
