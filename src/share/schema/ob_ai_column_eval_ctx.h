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

#ifndef OB_AI_COLUMN_EVAL_CTX_H_
#define OB_AI_COLUMN_EVAL_CTX_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

/**
 * @file ob_ai_column_eval_ctx.h
 * @brief The evaluation context of an AI column which is filled by the
 *        asynchronous worker.
 *
 * The worker knows which AI column it is filling, therefore it knows
 *
 *   - the system prompt of the model request: the normalized AI expression
 *     (the AI expression with the type specific instruction prepended), and
 *   - the type of the column which receives the answer of the model.
 *
 * The AI expression evaluation layer (AI_COMPLETE) reads both from this
 * context: the normalized expression is sent as the system message of the
 * model request, and the answer of the model is converted to the type of the
 * target column before it is handed over to the column conversion which the
 * SQL engine inserts for every assignment.
 *
 * The context is thread local: the worker fills the context, runs the
 * statement which evaluates the AI expression and clears the context
 * afterwards.  The inner SQL connection executes the statement in the
 * calling thread, so the context is visible to the expression evaluation.
 */
class ObAiColumnEvalCtx
{
public:
  ObAiColumnEvalCtx() = delete;
  ~ObAiColumnEvalCtx() = delete;

  /**
   * Set the system prompt of the model request.
   *
   * The caller keeps the memory of the prompt alive until clear() is called.
   */
  static void set_system_prompt(const common::ObString &system_prompt);

  /**
   * Set the type of the column which receives the answer of the model.
   */
  static void set_target_type(const common::ObObjType target_type);

  /**
   * Remove the context of the current thread.
   */
  static void clear();

  /**
   * The system prompt of the model request, an empty string if the context is
   * not set.
   */
  static common::ObString get_system_prompt();

  /**
   * The type of the target column, ObMaxType if the context is not set.
   */
  static common::ObObjType get_target_type();

  /**
   * Whether the context of the current thread is set.
   */
  static bool is_set();

private:
  static thread_local const char *system_prompt_ptr_;
  static thread_local int64_t system_prompt_len_;
  static thread_local common::ObObjType target_type_;
};

} // end namespace schema
} // end namespace share
} // end namespace oceanbase

#endif // OB_AI_COLUMN_EVAL_CTX_H_
