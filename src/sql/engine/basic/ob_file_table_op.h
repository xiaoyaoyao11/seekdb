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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_parquet_reader.h"

namespace oceanbase
{
namespace sql
{

/**
 * The physical operator that exposes a local file as a table.
 *
 * The operator is a leaf operator: it has no child and produces the rows of
 * the file by itself.  Only one record is kept in memory at any point in
 * time, so a file of arbitrary size can be scanned without materialising it.
 *
 * The spec carries everything the operator needs:
 *   - the absolute path of the file (or the glob pattern),
 *   - the kind of the reader,
 *   - one ObExpr per column, in the order of the inferred schema,
 *   - the OceanBase type of every column.
 */
class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      column_exprs_(alloc),
      path_(),
      func_type_(0),
      delimiter_(','),
      quote_char_('"'),
      has_header_(1),
      sample_size_(-1),
      col_types_(alloc)
  {}
  INHERIT_TO_STRING_KV("op_spec", ObOpSpec, K_(path), K_(func_type));
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  common::ObString path_;
  /* the reader to use, one of the ObFileTableFuncType values */
  int32_t func_type_;
  int32_t delimiter_;
  int32_t quote_char_;
  int32_t has_header_;
  int64_t sample_size_;
  /* the OceanBase type of every column, in the order of the schema */
  common::ObFixedArray<int32_t, common::ObIAllocator> col_types_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObFileTableSpec);
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      csv_scanner_(),
      jsonl_scanner_(),
      parquet_scanner_(),
      fields_(),
      nulls_(),
      glob_files_(),
      glob_idx_(0),
      is_glob_(false),
      is_opened_(false)
  {}
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int switch_iterator() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override { ObOperator::destroy(); }

private:
  int open_source();
  int close_source();
  int next_record(bool &has_row);
  int fill_row();
  int set_column_datum(int64_t col_idx, ObExpr *dst_expr);
  int set_glob_datum(ObExpr *dst_expr);
  int report_error(const char *what);

private:
  file_scan::CsvScanner csv_scanner_;
  file_scan::JsonlScanner jsonl_scanner_;
  file_scan::ParquetScanner parquet_scanner_;
  /* the values of the record that is currently being produced */
  std::vector<std::string> fields_;
  std::vector<char> nulls_;
  /* the files matched by a glob pattern */
  std::vector<std::string> glob_files_;
  int64_t glob_idx_;
  bool is_glob_;
  bool is_opened_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableOp);
};

} // end namespace sql
} // end namespace oceanbase
#endif /* OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_OP_H_ */
