/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_READER_H_

#include <stdio.h>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace sql
{

enum ObFileTableType
{
  FILE_TABLE_INVALID = 0,
  FILE_TABLE_CSV = 1,
  FILE_TABLE_JSON = 2,
  FILE_TABLE_PARQUET = 3,
  FILE_TABLE_GLOB = 4,
};

// one column of a file table, inferred from the file content
struct ObFileTableColumn
{
  ObFileTableColumn()
    : name_(), type_(common::ObUnknownType), length_(0), nullable_(true)
  {}
  common::ObString name_;
  common::ObObjType type_;   // ObInt32Type/ObInt64Type/ObDoubleType/ObTinyIntType/ObDateType/ObDateTimeType/ObVarcharType
  int64_t length_;           // varchar length or datetime scale
  bool nullable_;
  TO_STRING_KV(K_(name), K_(type), K_(length), K_(nullable));
};

// the definition of a file table, shared by the resolver (schema inference)
// and the execution operator (row reading).
// all memory of a definition is allocated from the stmt/plan allocator.
struct ObFileTableDef
{
  ObFileTableDef()
    : path_(), file_type_(FILE_TABLE_INVALID), has_header_(true), sample_size_(-1),
      null_str_(), columns_(NULL), column_count_(0)
  {}
  common::ObString path_;
  int64_t file_type_;
  bool has_header_;
  int64_t sample_size_;      // -1 means sampling all rows
  common::ObString null_str_;
  ObFileTableColumn *columns_;
  int64_t column_count_;
  TO_STRING_KV(K_(path), K_(file_type), K_(has_header), K_(sample_size), K_(column_count));
};

// streaming reader of csv/json/parquet/glob files.
// every row is returned as textual values, the caller converts them into datums
// according to the inferred column types.
class ObFileTableReader
{
public:
  ObFileTableReader();
  ~ObFileTableReader();

  // infer the schema of the file, fill def.columns_ from the given allocator
  static int infer_schema(common::ObIAllocator &alloc, ObFileTableDef &def);

  int open(common::ObIAllocator &alloc, const ObFileTableDef &def);
  // return OB_ITER_END if there is no more row.
  // a field is NULL if its ptr() is NULL
  int get_next_row(common::ObIArray<common::ObString> &fields);
  void close();

private:
  struct ObParquetCtx;
  int open_file(const char *path);
  int fill_buffer();
  int read_impl_char(char &c, bool &at_eof);
  int next_csv_row(common::ObIArray<common::ObString> &fields);
  int next_json_row(common::ObIArray<common::ObString> &fields);
  int next_json_raw(common::ObIArray<common::ObString> &keys,
                    common::ObIArray<common::ObString> &values,
                    common::ObIArray<bool> &is_null);
  int next_parquet_row(common::ObIArray<common::ObString> &fields);
  int next_glob_row(common::ObIArray<common::ObString> &fields);
  int load_parquet_row_group(int64_t rg_idx);
  int load_parquet_file(const char *path);
  int append_row_char(char c);
  int ensure_row_cap(int64_t need);
  void reset_row();
  int parse_json_line(const char *line, int64_t len,
                      common::ObIArray<common::ObString> &keys,
                      common::ObIArray<common::ObString> &values,
                      common::ObIArray<bool> &is_null);

  common::ObArenaAllocator alloc_;
  FILE *fp_;
  char *buf_;
  int64_t buf_cap_;
  int64_t buf_pos_;
  int64_t buf_len_;
  bool eof_;
  bool header_skipped_;
  char *row_buf_;
  int64_t row_cap_;
  int64_t row_len_;
  int64_t file_type_;
  common::ObArray<common::ObString, common::ModulePageAllocator, true> glob_files_;
  int64_t glob_pos_;
  ObFileTableColumn *columns_;
  int64_t column_count_;
  char *pq_data_;
  int64_t pq_len_;
  int64_t pq_pos_;
  int64_t pq_rows_done_;
  ObParquetCtx *pq_ctx_;
  int64_t pq_rg_idx_;
  int64_t pq_rg_row_;
  int64_t pq_rg_rows_;
  common::ObArray<common::ObString, common::ModulePageAllocator, true> pq_values_;
  bool opened_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableReader);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_TABLE_READER_H_
