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

#define USING_LOG_PREFIX SQL

#include "sql/resolver/dml/ob_ai_column_dml_util.h"
#include "share/schema/ob_ai_column_util.h"
#include "share/schema/ob_table_schema.h"

#include <cctype>

namespace oceanbase
{
namespace sql
{

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

bool ObAiColumnDmlUtil::depends_on(const ObString &ai_expr,
                                    const ObString &column_name)
{
  if (ai_expr.empty() || column_name.empty()) {
    return false;
  }
  // The expression text is scanned for the column name.  A whole-word match
  // is required, so that a column named "id" does not match "id_card".
  const char *p = ai_expr.ptr();
  const int64_t len = ai_expr.length();
  const int64_t name_len = column_name.length();
  int64_t i = 0;
  while (i < len) {
    if (p[i] == column_name.ptr()[0]) {
      // the match must start at a word boundary
      const bool left_ok = (0 == i) || (!isalnum(p[i - 1]) && p[i - 1] != '_');
      int64_t j = i + 1;
      while (j < len && p[j] == column_name.ptr()[j - i]) {
        ++j;
      }
      const bool matched = (j - i == name_len);
      const bool right_ok = (j >= len) || (!isalnum(p[j]) && p[j] != '_');
      if (matched && left_ok && right_ok) {
        return true;
      }
      i = j;
    } else {
      ++i;
    }
  }
  return false;
}

int ObAiColumnDmlUtil::collect_affected_ai_columns(
    const ObTableSchema &table_schema,
    const ObIArray<ObString> &changed_columns,
    ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && NULL != it && it != table_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    if (OB_ISNULL(column) || !column->is_ai_column()) {
      continue;
    }
    const ObString &expr = column->get_ai_column_expr();
    if (expr.empty()) {
      continue;
    }
    bool affected = false;
    for (int64_t i = 0; !affected && i < changed_columns.count(); ++i) {
      if (depends_on(expr, changed_columns.at(i))) {
        affected = true;
      }
    }
    if (affected && OB_FAIL(ai_columns.push_back(column))) {
    }
  }
  return ret;
}

int ObAiColumnDmlUtil::collect_all_ai_columns(
    const ObTableSchema &table_schema,
    ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && NULL != it && it != table_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    if (OB_ISNULL(column) || !column->is_ai_column()) {
      continue;
    }
    if (OB_FAIL(ai_columns.push_back(column))) {
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
