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

#ifndef _OB_LOG_FILE_TABLE_H
#define _OB_LOG_FILE_TABLE_H
#include "sql/optimizer/ob_logical_operator.h"

namespace oceanbase
{
namespace sql
{

class FileTablePath;
struct ObFileTableDef;

/**
 * The logical operator that reads a local file as if it were a table.
 *
 * The operator has no child: the rows are produced by the file reader of the
 * physical operator.  The schema of the table is not described by the data
 * dictionary but inferred from the content of the file, so the columns are
 * created while the statement is resolved.
 */
class ObLogFileTable : public ObLogicalOperator
{
public:
  ObLogFileTable(ObLogPlan &plan)
      : ObLogicalOperator(plan),
        table_id_(common::OB_INVALID_ID),
        table_name_(),
        file_table_def_(NULL),
        file_table_path_(NULL)
  {}
  virtual ~ObLogFileTable() {}
  virtual int est_cost() override;
  virtual int do_re_est_cost(EstimateCostInfo &param, double &card, double &op_cost, double &cost) override;
  virtual int compute_equal_set() override;
  virtual int compute_table_set() override;
  virtual int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) override;
  virtual int is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed) override;
  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  virtual int get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item) override;

  const common::ObIArray<ObColumnRefRawExpr *> &get_column_exprs() const { return column_exprs_; }
  common::ObIArray<ObColumnRefRawExpr *> &get_column_exprs() { return column_exprs_; }
  inline const ObFileTableDef *get_file_table_def() const { return file_table_def_; }
  inline void set_file_table_def(ObFileTableDef *file_table_def) { file_table_def_ = file_table_def; }
  inline common::ObString &get_table_name() { return table_name_; }
  inline const common::ObString &get_table_name() const { return table_name_; }
  inline void set_table_name(const common::ObString &table_name) { table_name_ = table_name; }
  inline uint64_t get_table_id() const { return table_id_; }
  inline void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  inline FileTablePath *get_file_table_path() { return file_table_path_; }
  inline void set_file_table_path(FileTablePath *file_table_path) { file_table_path_ = file_table_path; }
private:
  common::ObSEArray<ObColumnRefRawExpr*, 4, common::ModulePageAllocator, true> column_exprs_;
  ObFileTableDef *file_table_def_;
  common::ObString table_name_;
  uint64_t table_id_;
  FileTablePath *file_table_path_;
  DISALLOW_COPY_AND_ASSIGN(ObLogFileTable);
};
}
}
#endif
