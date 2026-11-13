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

#include "share/schema/ob_ai_column_util.h"
#include "common/object/ob_obj_type.h"
#include "share/object/ob_obj_cast.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

using namespace oceanbase::common;

static const char *const AI_JSON_INSTRUCTION = "Return exactly one valid JSON value. ";
static const char *const AI_INTEGER_INSTRUCTION = "Return exactly one base-10 integer. ";

const char *ObAiColumnUtil::get_json_instruction()
{
  return AI_JSON_INSTRUCTION;
}

const char *ObAiColumnUtil::get_integer_instruction()
{
  return AI_INTEGER_INSTRUCTION;
}

uint64_t ObAiColumnUtil::get_state_column_id(ObIAllocator &allocator,
                                             const ObTableSchema &table_schema,
                                             const ObColumnSchemaV2 &ai_column)
{
  ObString name = get_state_column_name(allocator, ai_column.get_column_name_str());
  const ObColumnSchemaV2 *column = name.empty() ? nullptr
                                                 : table_schema.get_column_schema(name);
  return OB_ISNULL(column) ? OB_INVALID_ID : column->get_column_id();
}

uint64_t ObAiColumnUtil::get_request_column_id(ObIAllocator &allocator,
                                               const ObTableSchema &table_schema,
                                               const ObColumnSchemaV2 &ai_column)
{
  ObString name = get_request_column_name(allocator, ai_column.get_column_name_str());
  const ObColumnSchemaV2 *column = name.empty() ? nullptr
                                                 : table_schema.get_column_schema(name);
  return OB_ISNULL(column) ? OB_INVALID_ID : column->get_column_id();
}

uint64_t ObAiColumnUtil::get_result_spec_column_id(ObIAllocator &allocator,
                                                    const ObTableSchema &table_schema,
                                                    const ObColumnSchemaV2 &ai_column)
{
  ObString name = get_result_spec_column_name(allocator, ai_column.get_column_name_str());
  const ObColumnSchemaV2 *column = name.empty() ? nullptr
                                                 : table_schema.get_column_schema(name);
  return OB_ISNULL(column) ? OB_INVALID_ID : column->get_column_id();
}

static int build_hidden_column_name(ObIAllocator &allocator,
                                    const ObString &ai_column_name,
                                    const char *prefix,
                                    const char *suffix,
                                    ObString &result)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
  const int64_t suffix_len = static_cast<int64_t>(strlen(suffix));
  const int64_t total = prefix_len + ai_column_name.length() + suffix_len;
  char *buf = NULL;
  if (total <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(total)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, prefix, prefix_len);
    if (ai_column_name.length() > 0) {
      MEMCPY(buf + prefix_len, ai_column_name.ptr(), ai_column_name.length());
    }
    MEMCPY(buf + prefix_len + ai_column_name.length(), suffix, suffix_len);
    result.assign(total, buf);
  }
  return ret;
}

ObString ObAiColumnUtil::get_state_column_name(ObIAllocator &allocator,
                                                const ObString &ai_column_name)
{
  ObString result;
  int ret = build_hidden_column_name(allocator, ai_column_name,
                                     "__ob_ai_state_", "_s", result);
  if (OB_FAIL(ret)) {
    result = ObString();
  }
  return result;
}

ObString ObAiColumnUtil::get_request_column_name(ObIAllocator &allocator,
                                                  const ObString &ai_column_name)
{
  ObString result;
  int ret = build_hidden_column_name(allocator, ai_column_name,
                                     "__ob_ai_request_", "_r", result);
  if (OB_FAIL(ret)) {
    result = ObString();
  }
  return result;
}

ObString ObAiColumnUtil::get_result_spec_column_name(ObIAllocator &allocator,
                                                      const ObString &ai_column_name)
{
  ObString result;
  int ret = build_hidden_column_name(allocator, ai_column_name,
                                     "__ob_ai_spec_", "_p", result);
  if (OB_FAIL(ret)) {
    result = ObString();
  }
  return result;
}

int ObAiColumnUtil::build_normalized_expr(ObIAllocator &allocator,
                                           const ObString &expr,
                                           const ObColumnSchemaV2 &column,
                                           ObString &normalized_expr)
{
  int ret = OB_SUCCESS;
  const char *instruction = NULL;
  ObObjType type = column.get_data_type();
  if (ob_is_json(type)) {
    instruction = AI_JSON_INSTRUCTION;
  } else if (ob_is_integer_type(type)) {
    instruction = AI_INTEGER_INSTRUCTION;
  } else {
    // a string column takes the model answer as it is
    instruction = "";
  }

  const int64_t instruction_len = static_cast<int64_t>(strlen(instruction));
  const int64_t total = instruction_len + expr.length();
  char *buf = NULL;
  if (total <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(total)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, instruction, instruction_len);
    if (expr.length() > 0) {
      MEMCPY(buf + instruction_len, expr.ptr(), expr.length());
    }
    normalized_expr.assign(total, buf);
  }
  return ret;
}

bool ObAiColumnUtil::is_function_name(const char *name, const int64_t len)
{
  // The AI expressions only use a small set of functions.  Anything which is
  // followed by '(' is a function call and not a column reference.
  UNUSED(name);
  UNUSED(len);
  return false;
}

int ObAiColumnUtil::collect_dependency_columns(ObIAllocator &allocator,
                                                 const ObString &expr,
                                                 ObIArray<ObString> &deps)
{
  int ret = OB_SUCCESS;
  const char *p = expr.ptr();
  const int64_t len = expr.length();
  int64_t i = 0;
  while (OB_SUCC(ret) && i < len) {
    const char c = p[i];
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_') {
      const int64_t begin = i;
      while (i < len) {
        const char ch = p[i];
        if (('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z')
            || ('0' <= ch && ch <= '9') || ch == '_' || ch == '$') {
          ++i;
        } else {
          break;
        }
      }
      const int64_t name_len = i - begin;
      // skip a quoted identifier
      if (begin > 0 && p[begin - 1] == '`') {
        continue;
      }
      // skip a function call
      int64_t j = i;
      while (j < len && (' ' == p[j] || '\t' == p[j] || '\n' == p[j] || '\r' == p[j])) {
        ++j;
      }
      if (j < len && '(' == p[j]) {
        continue;
      }
      // skip a keyword
      static const char *const keywords[] = {
        "AND", "AS", "ASCII", "BETWEEN", "CASE", "CAST", "COLLATE", "CONCAT",
        "DISTINCT", "ELSE", "END", "FALSE", "FROM", "IF", "IN", "INTERVAL", "IS",
        "LIKE", "NOT", "NULL", "OR", "ORDER", "SELECT", "SUBSTRING", "THEN",
        "TRUE", "WHEN", "WHERE"
      };
      bool is_keyword = false;
      for (int64_t k = 0; !is_keyword && k < static_cast<int64_t>(sizeof(keywords) / sizeof(keywords[0])); ++k) {
        if (static_cast<int64_t>(strlen(keywords[k])) == name_len
            && 0 == strncasecmp(p + begin, keywords[k], name_len)) {
          is_keyword = true;
        }
      }
      if (is_keyword) {
        continue;
      }
      // skip the duplicated names
      bool duplicated = false;
      for (int64_t k = 0; !duplicated && k < deps.count(); ++k) {
        if (deps.at(k).length() == name_len
            && 0 == strncasecmp(deps.at(k).ptr(), p + begin, name_len)) {
          duplicated = true;
        }
      }
      if (!duplicated) {
        if (OB_FAIL(deps.push_back(ObString(name_len, p + begin)))) {
        }
      }
    } else if (c == '\'' || c == '"' || c == '`') {
      // skip a string literal or a quoted identifier
      const char quote = c;
      ++i;
      while (i < len) {
        if (p[i] == '\\' && quote != '`' && i + 1 < len) {
          i += 2;
          continue;
        }
        if (p[i] == quote) {
          if (i + 1 < len && p[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
    } else {
      ++i;
    }
  }
  UNUSED(allocator);
  return ret;
}

} // end namespace schema
} // end namespace share
} // end namespace oceanbase
