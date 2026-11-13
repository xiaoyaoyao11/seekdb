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

#ifndef OB_AI_COLUMN_DML_UTIL_H_
#define OB_AI_COLUMN_DML_UTIL_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace sql
{

/**
 * @file ob_ai_column_dml_util.h
 * @brief Helpers used by the DML path to maintain the AI columns.
 *
 * An AI column is invalidated (its value is reset to NULL) when one of the
 * columns its expression depends on changes.  When a row is inserted, every
 * AI column of the table is invalidated.
 */
class ObAiColumnDmlUtil
{
public:
  ObAiColumnDmlUtil() = delete;
  ~ObAiColumnDmlUtil() = delete;

  /**
   * Check whether the AI expression of @p ai_column depends on @p column_name.
   *
   * The AI expression is stored as plain SQL text, so the dependency set is
   * derived by scanning the text for the identifiers it references.
   */
  static bool depends_on(const common::ObString &ai_expr,
                         const common::ObString &column_name);

  /**
   * Collect the AI columns of @p table_schema whose expression depends on
   * one of @p changed_columns.
   */
  static int collect_affected_ai_columns(
      const share::schema::ObTableSchema &table_schema,
      const common::ObIArray<common::ObString> &changed_columns,
      common::ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns);

  /**
   * Collect every AI column of @p table_schema.
   */
  static int collect_all_ai_columns(
      const share::schema::ObTableSchema &table_schema,
      common::ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OB_AI_COLUMN_DML_UTIL_H_
