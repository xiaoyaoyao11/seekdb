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

#define USING_LOG_PREFIX SQL_OPT

#include "sql/optimizer/ob_log_file_table.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/optimizer/ob_optimizer_util.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

int ObLogFileTable::est_cost()
{
  int ret = OB_SUCCESS;
  double card = 0.0;
  double op_cost = 0.0;
  double cost = 0.0;
  EstimateCostInfo param;
  param.need_parallel_ = get_parallel();
  if (OB_FAIL(do_re_est_cost(param, card, op_cost, cost))) {
  } else {
    set_card(card);
    set_op_cost(op_cost);
    set_cost(cost);
  }
  return ret;
}

int ObLogFileTable::do_re_est_cost(EstimateCostInfo &param, double &card, double &op_cost, double &cost)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan()) || OB_ISNULL(file_table_def_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    card = static_cast<double>(file_table_def_->row_cnt_);
    if (param.need_row_count_ >= 0 && param.need_row_count_ < card) {
      card = param.need_row_count_;
    }
    // every row of a file is read and parsed, the cost is linear in the number
    // of the rows of the file
    op_cost = card;
    cost = op_cost;
  }
  return ret;
}

int ObLogFileTable::compute_equal_set()
{
  int ret = OB_SUCCESS;
  set_output_equal_sets(&empty_expr_sets_);
  return ret;
}

int ObLogFileTable::compute_table_set()
{
  int ret = OB_SUCCESS;
  set_table_set(&empty_table_set_);
  return ret;
}

int ObLogFileTable::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
  } else if (OB_FAIL(append(all_exprs, column_exprs_))) {
  } else { /*do nothing*/ }
  return ret;
}

int ObLogFileTable::is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed)
{
  is_fixed = ObOptimizerUtil::find_item(column_exprs_, expr);
  return OB_SUCCESS;
}

int ObLogFileTable::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_exprs_.count(); ++i) {
    ObColumnRefRawExpr *column = column_exprs_.at(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(mark_expr_produced(column, branch_id_, id_, ctx))) {
    } else if (!is_plan_root() && OB_FAIL(output_exprs_.push_back(column))) {
    } else { /*do nothing*/ }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObLogicalOperator::allocate_expr_post(ctx))) {
    } else { /*do nothing*/ }
  }
  return ret;
}

int ObLogFileTable::get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::get_plan_item_info(plan_text, plan_item))) {
  } else {
    const ObString &name = get_table_name();
    BUF_PRINT_OB_STR(name.ptr(),
                     name.length(),
                     plan_item.object_alias_,
                     plan_item.object_alias_len_);
    BUF_PRINT_STR("FILE TABLE", plan_item.object_type_, plan_item.object_type_len_);
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
