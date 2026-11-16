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

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_virtual_ai_column.h"

#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_ai_column_util.h"
#include "common/object/ob_object.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

ObAllVirtualAiColumn::ObAllVirtualAiColumn()
  : ObVirtualTableScannerIterator(),
    start_to_read_(false),
    table_ids_(),
    table_idx_(0)
{
}

ObAllVirtualAiColumn::~ObAllVirtualAiColumn()
{
  reset();
}

void ObAllVirtualAiColumn::reset()
{
  start_to_read_ = false;
  table_idx_ = 0;
  table_ids_.reset();
  ObVirtualTableScannerIterator::reset();
}

int ObAllVirtualAiColumn::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (!start_to_read_) {
    if (OB_FAIL(fill_scanner())) {
    } else {
      start_to_read_ = true;
    }
  }
  if (OB_SUCC(ret) && start_to_read_) {
    if (OB_FAIL(scanner_it_.get_next_row(cur_row_))) {
      if (OB_ITER_END != ret) {
        SERVER_LOG(WARN, "fail to get next row from ai column scanner", K(ret));
      }
    } else {
      row = &cur_row_;
    }
  }
  return ret;
}

int ObAllVirtualAiColumn::fill_scanner()
{
  int ret = OB_SUCCESS;
  ObObj *cells = NULL;
  if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(cells = cur_row_.cells_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(schema_guard_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(schema_guard_->get_table_ids_in_tenant(table_ids_))) {
    SERVER_LOG(WARN, "fail to get table ids in tenant", K(ret));
  } else {
    for (int64_t t = 0; OB_SUCC(ret) && t < table_ids_.count(); ++t) {
      const ObTableSchema *table_schema = NULL;
      if (OB_FAIL(schema_guard_->get_table_schema(table_ids_.at(t), table_schema))) {
        SERVER_LOG(WARN, "fail to get table schema", K(ret), K(table_ids_.at(t)));
      } else if (OB_ISNULL(table_schema) || !table_schema->is_user_table()
                 || !table_schema->has_ai_column()) {
        // not a user table, or it has no AI column at all
        continue;
      } else {
        for (ObTableSchema::const_column_iterator it = table_schema->column_begin();
             OB_SUCC(ret) && NULL != it && it != table_schema->column_end(); ++it) {
          const ObColumnSchemaV2 *column = *it;
          if (OB_ISNULL(column) || !column->is_ai_column()) {
            continue;
          }
          // The descriptor of the AI column is persisted with its hidden
          // result spec column, because the column schema itself can only
          // keep the descriptor of a column which is not a collection.
          int8_t sync_mode = column->get_ai_sync_mode();
          int8_t max_retries = column->get_ai_max_retries();
          ObString expr = column->get_ai_column_expr();
          const ObString spec_name = ObAiColumnUtil::get_result_spec_column_name(
              *allocator_, column->get_column_name_str());
          const ObColumnSchemaV2 *spec_column = spec_name.empty()
                                                ? NULL
                                                : table_schema->get_column_schema(spec_name);
          if (OB_ISNULL(spec_column)) {
            // the AI column was added by an ALTER TABLE, which does not create
            // the hidden columns yet
          } else {
            const ObString &descriptor = spec_column->get_comment_str();
            ObString meta_expr;
            int8_t meta_sync_mode = 0;
            int8_t meta_max_retries = 0;
            if (OB_SUCC(ObAiColumnUtil::parse_meta_descriptor(descriptor, meta_sync_mode,
                                                             meta_max_retries, meta_expr))) {
              sync_mode = meta_sync_mode;
              max_retries = meta_max_retries;
              if (!meta_expr.empty()) {
                expr = meta_expr;
              }
            }
          }
          ObString normalized_expr;
          if (!expr.empty()) {
            if (OB_FAIL(ObAiColumnUtil::build_normalized_expr(*allocator_, expr,
                                                               *column, normalized_expr))) {
              SERVER_LOG(WARN, "fail to build the normalized ai column expression", K(ret));
            }
          }
          int64_t cell_idx = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < output_column_ids_.count(); ++i) {
            uint64_t col_id = output_column_ids_.at(i);
            switch (col_id) {
              case AI_COLUMN_TABLE_ID: {
                cells[cell_idx].set_int(table_schema->get_table_id());
                break;
              }
              case AI_COLUMN_COLUMN_ID: {
                cells[cell_idx].set_int(column->get_column_id());
                break;
              }
              case AI_COLUMN_COLUMN_NAME: {
                const ObString &name = column->get_column_name_str();
                cells[cell_idx].set_varchar(name);
                cells[cell_idx].set_collation_type(CS_TYPE_UTF8MB4_BIN);
                break;
              }
              case AI_COLUMN_MAX_RETRIES: {
                cells[cell_idx].set_int(max_retries);
                break;
              }
              case AI_COLUMN_NORMALIZED_EXPR: {
                // the column is not nullable, so an empty expression is an
                // empty string and not a null value
                static const char *const EMPTY_EXPR = "";
                const char *expr_ptr = normalized_expr.empty() ? EMPTY_EXPR : normalized_expr.ptr();
                const int32_t expr_len = normalized_expr.length();
                cells[cell_idx].set_lob_value(ObLongTextType, expr_ptr, expr_len);
                cells[cell_idx].set_collation_type(CS_TYPE_UTF8MB4_BIN);
                break;
              }
              case AI_COLUMN_STATE_COLUMN_ID: {
                cells[cell_idx].set_int(
                    ObAiColumnUtil::get_state_column_id(*allocator_, *table_schema, *column));
                break;
              }
              case AI_COLUMN_REQUEST_COLUMN_ID: {
                cells[cell_idx].set_int(
                    ObAiColumnUtil::get_request_column_id(*allocator_, *table_schema, *column));
                break;
              }
              case AI_COLUMN_RESULT_SPEC_COLUMN_ID: {
                cells[cell_idx].set_int(
                    ObAiColumnUtil::get_result_spec_column_id(*allocator_, *table_schema, *column));
                break;
              }              default: {
                ret = OB_ERR_UNEXPECTED;
                SERVER_LOG(WARN, "unexpected column id", K(ret), K(col_id));
                break;
              }
            }
            if (OB_SUCC(ret)) {
              ++cell_idx;
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(scanner_.add_row(cur_row_))) {
            SERVER_LOG(WARN, "fail to add ai column row", K(ret));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      scanner_it_ = scanner_.begin();
    }
  }
  return ret;
}

} // end namespace observer
} // end namespace oceanbase
