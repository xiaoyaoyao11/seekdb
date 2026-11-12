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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

// The max number of inputs carried by one embedding http request.
static const int64_t AI_EMBED_MAX_INPUTS_PER_REQUEST = 64;

ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::build_dimension_config(common::ObIAllocator &allocator,
                                          const int64_t dimension,
                                          common::ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *dimension_json = nullptr;
  if (dimension <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dimension_json))) {
  } else if (OB_FAIL(config_obj->add("dimensions", dimension_json))) {
  } else {
    config = config_obj;
  }
  return ret;
}

int ObExprAIEmbed::call_embedding(common::ObIAllocator &allocator,
                                  const common::ObString &model_id,
                                  common::ObArray<common::ObString> &contents,
                                  const int64_t dimension,
                                  common::ObArray<common::ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint_info;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  if (contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai_embed, the input is empty", K(ret));
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
    LOG_WARN("ai_embed, failed to get ai function info", K(ret), K(model_id));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, endpoint_info))) {
    LOG_WARN("ai_embed, failed to resolve the ai model endpoint", K(ret), K(model_id));
  } else if (OB_FAIL(ObAIFuncUtils::check_info_type_dense_embedding(info))) {
    LOG_WARN("ai_embed, the ai model is not a dense embedding model", K(ret), K(model_id));
  } else if (dimension > 0 && OB_FAIL(build_dimension_config(allocator, dimension, config))) {
    LOG_WARN("ai_embed, failed to build the dimension config", K(ret), K(dimension));
  } else {
    ObAIFuncModel model(allocator, *info, endpoint_info);
    if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
      LOG_WARN("ai_embed, failed to call the dense embedding model", K(ret), K(contents.count()));
    }
  }
  return ret;
}

int ObExprAIEmbed::read_content(ObEvalCtx &ctx,
                                common::ObIAllocator &allocator,
                                const ObExpr &expr,
                                const ObDatum &datum,
                                common::ObString &content)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_,
                                                        allocator,
                                                        datum,
                                                        expr.datum_meta_,
                                                        expr.obj_meta_.has_lob_header(),
                                                        content))) {
    LOG_WARN("ai_embed, failed to read the string data", K(ret));
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_model_id->get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null or empty");
    res.set_null();
  } else if (arg_content->is_null()) {
    // null input propagates to the result directly
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString content;
    int64_t dimension = 0;
    bool has_dimension = OB_NOT_NULL(arg_dim) && !arg_dim->is_null();
    if (has_dimension) {
      dimension = arg_dim->get_int();
    }
    if (OB_FAIL(ret)) {
    } else if (has_dimension && dimension <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
      res.set_null();
    } else if (OB_FAIL(read_content(ctx, temp_allocator, *expr.args_[CONTENT_IDX], *arg_content, content))) {
    } else if (content.empty()) {
      // an empty input has no meaningful embedding, propagate null to the result
      res.set_null();
    } else {
      ObArray<ObString> contents;
      ObArray<ObString> results;
      if (OB_FAIL(contents.push_back(content))) {
      } else if (OB_FAIL(call_embedding(temp_allocator, arg_model_id->get_string(), contents, dimension, results))) {
      } else if (results.count() != 1) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai_embed, unexpected result count", K(ret), K(results.count()));
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t size)
{
  INIT_SUCC(ret);
  ObDatumVector model_datums;
  ObDatumVector content_datums;
  ObDatumVector dim_datums;
  ObDatumVector res_datums = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  const bool has_dim = (3 == expr.arg_cnt_);
  if (has_dim) {
    ret = expr.eval_batch_param_value(ctx, skip, size, model_datums, content_datums, dim_datums);
  } else {
    ret = expr.eval_batch_param_value(ctx, skip, size, model_datums, content_datums);
  }
  if (OB_SUCC(ret)) {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    // Collect the rows which really need to request the model. The null rows are
    // skipped and their results are set to null, so that the result datum of each
    // row keeps aligned with the original row.
    ObArray<int64_t> row_indexes;
    ObArray<ObString> row_contents;
    ObArray<ObString> row_model_ids;
    int64_t dimension = 0;
    bool dimension_inited = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      eval_flags.set(i);
      ObDatum *model_datum = model_datums.at(i);
      ObDatum *content_datum = content_datums.at(i);
      if (OB_ISNULL(model_datum) || model_datum->is_null() || model_datum->get_string().empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null or empty");
      } else if (OB_ISNULL(content_datum) || content_datum->is_null()) {
        res_datums.at(i)->set_null();
        continue;
      } else if (has_dim && !dim_datums.at(i)->is_null()) {
        const int64_t row_dimension = dim_datums.at(i)->get_int();
        if (row_dimension <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        } else if (!dimension_inited) {
          dimension = row_dimension;
          dimension_inited = true;
        } else if (dimension != row_dimension) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter is not the same in one batch");
        }
      }
      ObString content;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(read_content(ctx, temp_allocator, *expr.args_[CONTENT_IDX], *content_datum, content))) {
      } else if (content.empty()) {
        res_datums.at(i)->set_null();
      } else if (OB_FAIL(row_indexes.push_back(i))
                 || OB_FAIL(row_contents.push_back(content))
                 || OB_FAIL(row_model_ids.push_back(model_datum->get_string()))) {
        LOG_WARN("ai_embed, failed to record the row input", K(ret), K(i));
      }
    }

    // Group the rows by the model id, and flush a group when the model changes
    // or the inputs of one request reach the limit.
    int64_t group_begin = 0;
    while (OB_SUCC(ret) && group_begin < row_contents.count()) {
      int64_t group_end = group_begin + 1;
      while (group_end < row_contents.count()
             && group_end - group_begin < AI_EMBED_MAX_INPUTS_PER_REQUEST
             && 0 == row_model_ids.at(group_begin).case_compare(row_model_ids.at(group_end))) {
        group_end++;
      }
      ObArray<ObString> contents;
      ObArray<ObString> results;
      for (int64_t i = group_begin; OB_SUCC(ret) && i < group_end; i++) {
        if (OB_FAIL(contents.push_back(row_contents.at(i)))) {
          LOG_WARN("ai_embed, failed to push back the content", K(ret), K(i));
        }
      }
      int64_t batch_ret = OB_SUCCESS;
      if (OB_FAIL(ret)) {
      } else if (OB_SUCCESS == (batch_ret = call_embedding(temp_allocator,
                                                           row_model_ids.at(group_begin),
                                                           contents,
                                                           dimension,
                                                           results))) {
      } else {
        // Fall back to request the inputs one by one when the service rejects
        // the batched request, so that a single bad input does not break the
        // whole batch.
        LOG_WARN("ai_embed, failed to call the embedding service in batch, try one by one",
                 K(batch_ret), K(contents.count()));
        results.reset();
        for (int64_t i = group_begin; OB_SUCC(ret) && i < group_end; i++) {
          ObArray<ObString> one_content;
          ObArray<ObString> one_result;
          if (OB_FAIL(one_content.push_back(row_contents.at(i)))) {
          } else if (OB_FAIL(call_embedding(temp_allocator,
                                            row_model_ids.at(i),
                                            one_content,
                                            dimension,
                                            one_result))) {
            LOG_WARN("ai_embed, failed to call the embedding service", K(ret), K(i));
          } else if (one_result.count() != 1) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(results.push_back(one_result.at(0)))) {
          }
        }
      }
      if (OB_SUCC(ret) && results.count() != group_end - group_begin) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai_embed, unexpected result count", K(ret), K(results.count()),
                 K(group_end - group_begin));
      }
      for (int64_t i = group_begin; OB_SUCC(ret) && i < group_end; i++) {
        const ObString &result = results.at(i - group_begin);
        const int64_t row_idx = row_indexes.at(i);
        char *res_buf = nullptr;
        if (result.empty()) {
          res_datums.at(row_idx)->set_null();
        } else if (OB_ISNULL(res_buf = expr.get_str_res_mem(ctx, result.length(), row_idx))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("ai_embed, failed to allocate the result buffer", K(ret), K(row_idx));
        } else {
          MEMCPY(res_buf, result.ptr(), result.length());
          res_datums.at(row_idx)->set_string(res_buf, result.length());
        }
      }
      group_begin = group_end;
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
  rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  return ret;
}

} // namespace sql
} // namespace oceanbase
