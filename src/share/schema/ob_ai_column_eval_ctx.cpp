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

#include "share/schema/ob_ai_column_eval_ctx.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

thread_local const char *ObAiColumnEvalCtx::system_prompt_ptr_ = NULL;
thread_local int64_t ObAiColumnEvalCtx::system_prompt_len_ = 0;
thread_local common::ObObjType ObAiColumnEvalCtx::target_type_ = common::ObMaxType;

void ObAiColumnEvalCtx::set_system_prompt(const common::ObString &system_prompt)
{
  system_prompt_ptr_ = system_prompt.ptr();
  system_prompt_len_ = system_prompt.length();
}

void ObAiColumnEvalCtx::set_target_type(const common::ObObjType target_type)
{
  target_type_ = target_type;
}

void ObAiColumnEvalCtx::clear()
{
  system_prompt_ptr_ = NULL;
  system_prompt_len_ = 0;
  target_type_ = common::ObMaxType;
}

common::ObString ObAiColumnEvalCtx::get_system_prompt()
{
  if (NULL == system_prompt_ptr_ || system_prompt_len_ <= 0) {
    return common::ObString();
  }
  return common::ObString(system_prompt_len_, system_prompt_ptr_);
}

common::ObObjType ObAiColumnEvalCtx::get_target_type()
{
  return target_type_;
}

bool ObAiColumnEvalCtx::is_set()
{
  return NULL != system_prompt_ptr_ && system_prompt_len_ > 0;
}

} // end namespace schema
} // end namespace share
} // end namespace oceanbase
