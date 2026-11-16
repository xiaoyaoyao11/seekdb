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

#ifndef OB_ALL_VIRTUAL_AI_COLUMN_H_
#define OB_ALL_VIRTUAL_AI_COLUMN_H_

#include "observer/virtual_table/ob_virtual_table_scanner_iterator.h"
#include "lib/container/ob_se_array.h"
#include "lib/ob_define.h"

namespace oceanbase
{
namespace observer
{

/**
 * @file ob_all_virtual_ai_column.h
 * @brief The oceanbase.__all_ai_column virtual table.
 *
 * It exposes one row per AI column of every user table in the tenant:
 *
 *   table_id              the id of the table which owns the AI column
 *   column_id             the id of the AI column itself (a user visible column)
 *   column_name           the name of the AI column
 *   max_retries           the retry budget declared for the AI column
 *   normalized_expr       the normalized AI expression (typed system prompt included)
 *   state_column_id       the id of the hidden column holding the column state
 *   request_column_id     the id of the hidden column holding the request id
 *   result_spec_column_id the id of the hidden column holding the result spec
 */
class ObAllVirtualAiColumn : public common::ObVirtualTableScannerIterator
{
public:
  ObAllVirtualAiColumn();
  virtual ~ObAllVirtualAiColumn();

  virtual int inner_get_next_row(common::ObNewRow *&row) override;
  virtual void reset() override;

  enum COLUMN_ID_LIST
  {
    AI_COLUMN_TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    AI_COLUMN_COLUMN_ID,
    AI_COLUMN_COLUMN_NAME,
    AI_COLUMN_MAX_RETRIES,
    AI_COLUMN_NORMALIZED_EXPR,
    AI_COLUMN_STATE_COLUMN_ID,
    AI_COLUMN_REQUEST_COLUMN_ID,
    AI_COLUMN_RESULT_SPEC_COLUMN_ID
  };

private:
  int fill_scanner();

  bool start_to_read_;
  common::ObSEArray<uint64_t, 16> table_ids_;
  int64_t table_idx_;

  DISALLOW_COPY_AND_ASSIGN(ObAllVirtualAiColumn);
};

} // end namespace observer
} // end namespace oceanbase

#endif // OB_ALL_VIRTUAL_AI_COLUMN_H_
