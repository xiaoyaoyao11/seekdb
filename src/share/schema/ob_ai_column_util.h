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

#ifndef OB_AI_COLUMN_UTIL_H_
#define OB_AI_COLUMN_UTIL_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

/**
 * @file ob_ai_column_util.h
 * @brief Helpers shared by the AI column metadata, the DDL resolver, the
 *        virtual tables and the asynchronous worker.
 *
 * Every AI column owns three hidden columns:
 *
 *   __ob_ai_state_<col>   the lifecycle state of the column (PENDING/READY/FAILED)
 *   __ob_ai_request_<col> the id of the request group which produced the value
 *   __ob_ai_spec_<col>    the result spec (target type) of the AI expression
 *
 * The three column ids are derived from the id of the AI column itself, so
 * that they can be recomputed from the schema without extra metadata.
 */
class ObAiColumnUtil
{
public:
  ObAiColumnUtil() = delete;
  ~ObAiColumnUtil() = delete;

  // ---- the column ids of the three hidden columns ----
  // The hidden columns are looked up by their name, so that their ids do not
  // depend on the order in which the columns of the table were declared.
  // The allocator builds the name of the hidden column, the caller owns it.
  static uint64_t get_state_column_id(common::ObIAllocator &allocator,
                                      const ObTableSchema &table_schema,
                                      const ObColumnSchemaV2 &ai_column);
  static uint64_t get_request_column_id(common::ObIAllocator &allocator,
                                        const ObTableSchema &table_schema,
                                        const ObColumnSchemaV2 &ai_column);
  static uint64_t get_result_spec_column_id(common::ObIAllocator &allocator,
                                             const ObTableSchema &table_schema,
                                             const ObColumnSchemaV2 &ai_column);

  // ---- the names of the three hidden columns ----
  static common::ObString get_state_column_name(common::ObIAllocator &allocator,
                                                 const common::ObString &ai_column_name);
  static common::ObString get_request_column_name(common::ObIAllocator &allocator,
                                                   const common::ObString &ai_column_name);
  static common::ObString get_result_spec_column_name(common::ObIAllocator &allocator,
                                                      const common::ObString &ai_column_name);

  // ---- the normalized expression ----
  /**
   * Build the normalized expression of an AI column.
   *
   * The normalized expression is the AI expression with a type specific
   * system instruction prepended, so that the model is asked for a value of
   * the column's type:
   *
   *   JSON    -> "Return exactly one valid JSON value. <expr>"
   *   BIGINT  -> "Return exactly one base-10 integer. <expr>"
   *   VARCHAR -> "<expr>"
   */
  static int build_normalized_expr(common::ObIAllocator &allocator,
                                    const common::ObString &expr,
                                    const ObColumnSchemaV2 &column,
                                    common::ObString &normalized_expr);

  // ---- the typed system instruction ----
  static const char *get_json_instruction();
  static const char *get_integer_instruction();

  // ---- the dependency columns of an AI expression ----
  /**
   * Collect the names of the columns referenced by an AI expression.
   *
   * The AI expression is stored as plain SQL text, so the dependency set is
   * derived by scanning the text for identifiers.  Only identifiers which are
   * not function names, keywords or string literals are reported.
   */
  static int collect_dependency_columns(common::ObIAllocator &allocator,
                                        const common::ObString &expr,
                                        common::ObIArray<common::ObString> &deps);

private:
  static bool is_function_name(const char *name, const int64_t len);
};

} // end namespace schema
} // end namespace share
} // end namespace oceanbase

#endif // OB_AI_COLUMN_UTIL_H_
