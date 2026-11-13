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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/basic/ob_file_table_reader.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "share/ob_errno.h"
#include "lib/container/ob_se_array.h"
#include "common/timezone/ob_time_convert.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{

int set_datum_from_text(ObDatum &datum, const ObExpr &expr, const ObString &text)
{
  int ret = OB_SUCCESS;
  const char *p = text.ptr();
  const int64_t len = text.length();
  char tmp[128];
  switch (expr.datum_meta_.type_) {
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObIntType:
    case ObUTinyIntType:
    case ObUSmallIntType:
    case ObUMediumIntType:
    case ObUInt32Type:
    case ObUInt64Type: {
      if (len >= static_cast<int64_t>(sizeof(tmp))) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(tmp, p, len);
        tmp[len] = '\0';
        if (0 == strncasecmp(tmp, "true", 4)) {
          datum.set_int(1);
        } else if (0 == strncasecmp(tmp, "false", 5)) {
          datum.set_int(0);
        } else {
          datum.set_int(strtoll(tmp, NULL, 10));
        }
      }
      break;
    }
    case ObFloatType:
    case ObUFloatType: {
      if (len >= static_cast<int64_t>(sizeof(tmp))) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(tmp, p, len);
        tmp[len] = '\0';
        datum.set_float(strtof(tmp, NULL));
      }
      break;
    }
    case ObDoubleType:
    case ObUDoubleType: {
      if (len >= static_cast<int64_t>(sizeof(tmp))) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(tmp, p, len);
        tmp[len] = '\0';
        datum.set_double(strtod(tmp, NULL));
      }
      break;
    }
    case ObDateType: {
      int32_t date_val = 0;
      if (OB_FAIL(ObTimeConverter::str_to_date(text, date_val))) {
        LOG_WARN("failed to convert text to date", K(ret), K(text));
      } else {
        datum.set_date(date_val);
      }
      break;
    }
    case ObDateTimeType: {
      ObTimeConvertCtx cvrt_ctx(NULL, false);
      int64_t datetime_val = 0;
      if (OB_FAIL(ObTimeConverter::str_to_datetime(text, cvrt_ctx, datetime_val))) {
        LOG_WARN("failed to convert text to datetime", K(ret), K(text));
      } else {
        datum.set_datetime(datetime_val);
      }
      break;
    }
    case ObMySQLDateType: {
      ObMySQLDate date_val;
      if (OB_FAIL(ObTimeConverter::str_to_mdate(text, date_val))) {
        LOG_WARN("failed to convert text to mysql date", K(ret), K(text));
      } else {
        datum.set_int(date_val.date_);
      }
      break;
    }
    case ObMySQLDateTimeType: {
      ObTimeConvertCtx cvrt_ctx(NULL, false);
      ObMySQLDateTime datetime_val;
      if (OB_FAIL(ObTimeConverter::str_to_mdatetime(text, cvrt_ctx, datetime_val))) {
        LOG_WARN("failed to convert text to mysql datetime", K(ret), K(text));
      } else {
        datum.set_int(datetime_val.datetime_);
      }
      break;
    }
    default:
      datum.set_string(text);
      break;
  }
  return ret;
}

} // anonymous namespace

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_path_, file_type_, has_header_, sample_size_, column_ids_, column_names_);

int ObFunctionTableOp::open_reader()
{
  int ret = OB_SUCCESS;
  ObIAllocator &alloc = ctx_.get_allocator();
  if (NULL == reader_) {
    void *mem = alloc.alloc(sizeof(ObFileTableReader));
    if (NULL == mem) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      reader_ = new (mem) ObFileTableReader();
    }
  }
  if (OB_SUCC(ret)) {
    ObFileTableDef def;
    def.path_ = MY_SPEC.file_path_;
    def.file_type_ = MY_SPEC.file_type_;
    def.has_header_ = MY_SPEC.has_header_;
    def.sample_size_ = MY_SPEC.sample_size_;
    if (MY_SPEC.column_names_.count() > 0) {
      ObFileTableColumn *cols = static_cast<ObFileTableColumn *>(
          alloc.alloc(sizeof(ObFileTableColumn) * MY_SPEC.column_names_.count()));
      if (NULL == cols) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t i = 0; i < MY_SPEC.column_names_.count(); ++i) {
          new (&cols[i]) ObFileTableColumn();
          cols[i].name_ = MY_SPEC.column_names_.at(i);
        }
        def.columns_ = cols;
        def.column_count_ = MY_SPEC.column_names_.count();
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(reader_->open(alloc, def))) {
      LOG_WARN("failed to open file table reader", K(ret), K(MY_SPEC.file_path_));
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FILE_TABLE_INVALID != MY_SPEC.file_type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    if (OB_FAIL(open_reader())) {
    }
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 16> fields;
  clear_evaluated_flag();
  if (OB_ISNULL(reader_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(reader_->get_next_row(fields))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("failed to get next row from file", K(ret), K(MY_SPEC.file_type_));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      const int64_t file_idx = i < MY_SPEC.column_ids_.count() ? MY_SPEC.column_ids_.at(i) : i;
      if (file_idx < 0 || file_idx >= fields.count() || NULL == fields.at(file_idx).ptr()) {
        datum.set_null();
      } else if (OB_FAIL(set_datum_from_text(datum, *expr, fields.at(file_idx)))) {
        LOG_WARN("failed to convert file value", K(ret), K(file_idx));
      }
      if (OB_SUCC(ret)) {
        expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}


int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
    if (FILE_TABLE_INVALID != MY_SPEC.file_type_ && NULL != reader_) {
      reader_->close();
      if (OB_FAIL(open_reader())) {
        LOG_WARN("failed to reopen file table reader", K(ret));
      }
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
  if (NULL != reader_) {
    reader_->close();
  }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (NULL != reader_) {
    reader_->close();
    reader_->~ObFileTableReader();
    reader_ = NULL;
  }
  ObOperator::destroy();
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
