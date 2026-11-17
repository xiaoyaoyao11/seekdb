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

#include "sql/engine/basic/file_sql_plan.h"
#include "sql/engine/basic/file_sql_privilege.h"
#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/timezone/ob_time_convert.h"
#include "sql/pl/ob_pl_user_type.h"
#include <charconv>
#include <cmath>
#include <new>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_sql_descriptor_, file_column_ordinals_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (!MY_SPEC.file_sql_descriptor_.empty()) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_file_row;
    ret = open_file_reader();
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (!MY_SPEC.file_sql_descriptor_.empty()) {
    ret = file_reader_ == nullptr ? OB_NOT_INIT : file_reader_->rescan();
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  if (file_reader_ != nullptr) { delete file_reader_; file_reader_=nullptr; }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (file_reader_ != nullptr) { delete file_reader_; file_reader_=nullptr; }
  ObOperator::destroy();
}


int ObFunctionTableOp::open_file_reader()
{
  int ret=OB_SUCCESS;
  FileSqlPlan plan;
  ObString secure_root;
  ObObj enabled;
  auto *session=ctx_.get_my_session();
  CK (OB_NOT_NULL(session));
  OZ (session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled));
  if (OB_SUCC(ret) && (enabled.is_null() || enabled.get_int()==0)) ret=OB_NOT_SUPPORTED;
  CK (OB_NOT_NULL(ctx_.get_sql_ctx()));
  CK (OB_NOT_NULL(ctx_.get_sql_ctx()->schema_guard_));
  OZ (file_sql_check_file_privilege(*ctx_.get_sql_ctx()->schema_guard_, *session));
  OZ (session->get_secure_file_priv(secure_root));
  OZ (plan.decode(MY_SPEC.file_sql_descriptor_));
  CK (MY_SPEC.column_exprs_.count()==MY_SPEC.file_column_ordinals_.count());
  std::vector<int64_t> projection;
  for (int64_t i=0; OB_SUCC(ret) && i<MY_SPEC.file_column_ordinals_.count(); ++i) {
    const int64_t ordinal=MY_SPEC.file_column_ordinals_.at(i);
    if (ordinal<0 || ordinal>=static_cast<int64_t>(plan.schema.columns.size())) ret=OB_ERR_UNEXPECTED;
    else projection.push_back(ordinal);
  }
  if (OB_SUCC(ret)) {
    if (file_reader_!=nullptr) delete file_reader_;
    file_reader_=new (std::nothrow) FileSqlReader();
    if (file_reader_==nullptr) ret=OB_ALLOCATE_MEMORY_FAILED;
    else {
      const std::string root=secure_root.empty() ? std::string() : std::string(secure_root.ptr(),secure_root.length());
      if (OB_FAIL(file_reader_->open(plan.path,plan.options,root,plan.schema,projection))) {
        LOG_WARN("file SQL open failed", K(ret), "detail", file_reader_->error().c_str());
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, file_reader_->error().c_str());
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_file_row()
{
  int ret=OB_SUCCESS;
  clear_evaluated_flag();
  FileSqlRow row;
  CK (OB_NOT_NULL(file_reader_));
  OZ (ctx_.check_status());
  if (OB_SUCC(ret) && OB_FAIL(file_reader_->next(row))) {
    if (ret!=OB_ITER_END) {
      LOG_WARN("file SQL read failed", K(ret), "detail", file_reader_->error().c_str());
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, file_reader_->error().c_str());
    }
  }
  for (int64_t i=0; OB_SUCC(ret) && i<MY_SPEC.column_exprs_.count(); ++i) {
    ObExpr *expr=MY_SPEC.column_exprs_.at(i);
    const int64_t ordinal=MY_SPEC.file_column_ordinals_.at(i);
    if (ordinal<0 || ordinal>=static_cast<int64_t>(row.cells.size())) { ret=OB_ERR_UNEXPECTED; break; }
    const auto &cell=row.cells[ordinal];
    ObDatum &datum=expr->locate_datum_for_write(eval_ctx_);
    if (cell.is_null) datum.set_null();
    else {
      const std::string &text=cell.text;
      const ObString input(text.size(),text.data());
      switch (expr->datum_meta_.type_) {
        case ObIntType: {
          int64_t v=0; auto parsed=std::from_chars(text.data()+(!text.empty() && text[0]=='+' ? 1 : 0),text.data()+text.size(),v);
          if (parsed.ec!=std::errc() || parsed.ptr!=text.data()+text.size()) ret=OB_INVALID_ARGUMENT;
          else datum.set_int(v);
          break;
        }
        case ObTinyIntType:
          if (text=="true" || text=="1") datum.set_int(1);
          else if (text=="false" || text=="0") datum.set_int(0);
          else ret=OB_INVALID_ARGUMENT;
          break;
        case ObDoubleType: {
          double v=0; auto parsed=std::from_chars(text.data()+(!text.empty() && text[0]=='+' ? 1 : 0),text.data()+text.size(),v);
          if (parsed.ec!=std::errc() || parsed.ptr!=text.data()+text.size() || !std::isfinite(v)) ret=OB_INVALID_ARGUMENT;
          else datum.set_double(v);
          break;
        }
        case ObDateType: {
          int32_t date=0;
          OZ (ObTimeConverter::str_to_date(input,date));
          if (OB_SUCC(ret)) datum.set_date(date);
          break;
        }
        case ObDateTimeType: {
          int64_t datetime=0;
          ObTimeConvertCtx convert_ctx(ctx_.get_my_session()->get_timezone_info(),false);
          OZ (ObTimeConverter::str_to_datetime(input,convert_ctx,datetime));
          if (OB_SUCC(ret)) datum.set_datetime(datetime);
          break;
        }
        default: {
          char *dest=expr->get_str_res_mem(eval_ctx_,text.size());
          if (dest==nullptr && !text.empty()) ret=OB_ALLOCATE_MEMORY_FAILED;
          else { if (!text.empty()) MEMCPY(dest,text.data(),text.size()); datum.set_string(dest,text.size()); }
          break;
        }
      }
    }
    if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}

int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
