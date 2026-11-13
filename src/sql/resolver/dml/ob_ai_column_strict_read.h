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

#ifndef OB_AI_COLUMN_STRICT_READ_H_
#define OB_AI_COLUMN_STRICT_READ_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include <utility>

namespace oceanbase
{
namespace sql
{

class ObDMLStmt;
class ObSelectStmt;

/**
 * @file ob_ai_column_strict_read.h
 * @brief The strict read check of the AI columns.
 *
 * When the session variable ai_column_strict_read is on, a statement which
 * reads an AI column which still has unfinished tasks fails immediately with
 * the error 11120 instead of returning NULL for the AI column.
 *
 * The check is column wide: as long as one row of the AI column is not
 * finished, the whole column counts as unfinished.  Only the AI columns which
 * the statement really references are checked.
 */
class ObAiColumnStrictRead
{
public:
  ObAiColumnStrictRead() = delete;
  ~ObAiColumnStrictRead() = delete;

  /**
   * Check the AI columns which @p stmt references.
   *
   * @param stmt the resolved statement
   * @param is_strict whether the session variable ai_column_strict_read is on
   * @return OB_SUCCESS when the statement may run, OB_AI_COLUMN_HAS_UNFINISHED_TASKS
   *         when one of the referenced AI columns has unfinished tasks
   */
  static int check(ObDMLStmt &stmt, const bool is_strict);

  /**
   * Collect the AI columns which a statement references.
   *
   * @param stmt       the resolved statement
   * @param ai_columns the collected (table_id, column_id) pairs
   */
  static int collect_ai_columns_of_stmt(
      ObDMLStmt &stmt,
      common::ObIArray<std::pair<uint64_t, uint64_t> > &ai_columns);

  /**
   * Collect the AI columns which a select statement references.
   */
  static int collect_from_select(
      const ObSelectStmt &stmt,
      common::ObIArray<std::pair<uint64_t, uint64_t> > &ai_columns);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OB_AI_COLUMN_STRICT_READ_H_
