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

/**
 * @file ob_parquet_reader.h
 *
 * Reader for the Apache Parquet columnar file format, backing the
 * read_parquet() table function of the "file as database" feature.
 *
 * Like ob_file_scan_utils.h the module is deliberately free of any OceanBase
 * runtime dependency: it only uses the C++17 standard library and the POSIX
 * file interfaces, so that the parsing rules can be exercised by unit tests
 * without spinning up an observer.
 *
 * The reader covers the subset of Parquet that a flat (non nested) table
 * needs:
 *
 *   - the Thrift compact protocol decoder for the file footer,
 *   - data page version 1 and version 2,
 *   - the PLAIN and the RLE / PLAIN_DICTIONARY value encodings,
 *   - the UNCOMPRESSED and the SNAPPY compression codecs,
 *   - column pruning so that only the requested columns are decoded.
 *
 * Everything else (nested schemas, the remaining compression codecs and the
 * remaining encodings) is reported as Status::UNSUPPORTED instead of being
 * silently misread.
 *
 * The values are produced in the very same shape as the CSV and the JSONL
 * readers: every field is a textual value and a parallel array marks the
 * NULL fields.  The OceanBase facing glue in ob_file_table_op.cpp can
 * therefore treat all three formats alike.
 */

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_READER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
namespace file_scan
{

namespace parquet_detail
{

/// The Parquet page types.
enum PageType
{
  PAGE_DATA       = 0,
  PAGE_INDEX      = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2    = 3,
};

/// The header of a single Parquet page.
struct PageHeader
{
  int32_t type                = PAGE_DATA;
  int32_t uncompressed_size   = 0;
  int32_t compressed_size     = 0;
  /// The number of values of a data page.
  int32_t num_values          = 0;
  /// The encoding of the values of a data page.
  int32_t encoding            = 0;
  /// The encoding of the definition levels of a data page.
  int32_t def_level_encoding   = 0;
  /// The encoding of the repetition levels of a data page.
  int32_t rep_level_encoding   = 0;
  /// The number of NULL values of a data page v2.
  int32_t num_nulls           = 0;
  /// The number of rows of a data page v2.
  int32_t num_rows             = 0;
  /// The byte length of the definition levels of a data page v2.
  int32_t def_level_byte_length = 0;
  /// The byte length of the repetition levels of a data page v2.
  int32_t rep_level_byte_length = 0;
  /// True when the values of a data page v2 are compressed.
  bool is_compressed          = false;
  /// The number of entries of a dictionary page.
  int32_t dictionary_num_values = 0;
};

/// The physical description of one leaf column.
struct ColumnDesc
{
  std::string name;
  ColumnType type        = ColumnType::VARCHAR;
  bool nullable           = false;
  int32_t physical_type   = 0;  ///< Parquet Type enum value.
  int32_t type_length     = 0;  ///< Only used by FIXED_LEN_BYTE_ARRAY.
  int32_t converted_type  = -1; ///< Parquet ConvertedType enum value.
  int32_t logical_type    = -1; ///< Parquet LogicalType union variant.
  int32_t timestamp_unit  = 0;  ///< 0 = micros, 1 = millis, 2 = nanos.
  int32_t leaf_index      = 0;  ///< The position of the leaf in the schema.
};

/// The footer description of one column chunk.
struct ChunkDesc
{
  int32_t physical_type    = 0;
  int32_t encoding         = 0;
  int32_t codec            = 0;
  int64_t num_values       = 0;
  int64_t data_page_offset = 0;
  int64_t dictionary_offset = 0;
  int64_t index_offset     = 0;
  int64_t total_compressed  = 0;
};

/// The description of one row group.
struct RowGroupDesc
{
  int64_t num_rows = 0;
  std::vector<ChunkDesc> chunks; ///< Indexed like the leaf columns.
};

} // namespace parquet_detail

/**
 * Streaming reader for a Parquet file.
 *
 * The reader never materialises the file.  It walks the row groups in the
 * order they appear in the footer and, for every row group, it keeps one
 * cursor per projected column.  A cursor decodes one page at a time and
 * caches the decoded page, so the memory footprint stays proportional to the
 * page size and not to the file size.
 */
class ParquetScanner
{
public:
  ParquetScanner() = default;
  ~ParquetScanner();

  ParquetScanner(const ParquetScanner &) = delete;
  ParquetScanner &operator=(const ParquetScanner &) = delete;

  /// Open the file and parse the footer.
  Status open(const std::string &path, const ReadOptions &options, Error *error);

  /// Derive the column schema from the footer.  Parquet carries its own
  /// schema, so no pass over the data is required.
  Status infer_schema(std::vector<ColumnSchema> *columns, Error *error);

  /// Produce the next row.  Returns false when the file is exhausted.
  bool next_row(std::vector<std::string> *fields,
                std::vector<char> *nulls,
                Error *error);

  /// Restart the scan from the first row.
  Status rewind(Error *error);

  void close();

  /**
   * Restrict the scan to the given columns.  The names must match the leaf
   * names of the Parquet schema.  When the projection is not set every
   * column of the file is decoded.
   */
  Status set_projection(const std::vector<std::string> &column_names, Error *error);

  const std::vector<std::string> &header_names() const { return header_names_; }
  const ReadOptions &options() const { return options_; }
  const std::string &path() const { return path_; }
  /// The number of rows the footer reports.
  int64_t row_count() const { return row_count_; }

private:
  /// The decoded values of a single page.
  struct PageValues
  {
    std::vector<std::string> values;
    std::vector<char> nulls;
  };

  /// The cursor of one column inside the current row group.
  class ColumnCursor
  {
  public:
    ColumnCursor() = default;

    /// Start the cursor on the given chunk.
    Status start(const std::string &path,
                 const parquet_detail::ChunkDesc &chunk,
                 const parquet_detail::ColumnDesc &desc,
                 Error *error);

    /// Produce the next value of the column.  `is_null` marks a NULL value.
    bool next_value(std::string *value, bool *is_null, Error *error);

    /// Release the cached page.
    void reset();

    int64_t remaining() const { return remaining_; }

  private:
    /// Read the dictionary page of the chunk, if it has one.
    Status load_dictionary(const std::string &path, Error *error);
    /// Decode the next page of the chunk into `page_`.
    Status decode_next_page(const std::string &path, Error *error);
    /// Decode the values of one page.  The definition levels of the page are
    /// taken from `levels_`, they are decoded by the caller because the two
    /// page versions place them differently.
    Status decode_values(const std::string &payload,
                         const parquet_detail::PageHeader &header,
                         Error *error);

    std::string path_;
    parquet_detail::ChunkDesc chunk_;
    parquet_detail::ColumnDesc desc_;
    /// The raw dictionary values, indexed by the dictionary index.
    std::vector<std::string> dictionary_;
    bool has_dictionary_       = false;
    int64_t offset_            = 0; ///< The next byte to read.
    int64_t end_offset_        = 0; ///< The end of the chunk.
    int64_t values_read_       = 0; ///< The values decoded so far.
    int64_t remaining_         = 0; ///< The values that are still to come.
    size_t page_pos_           = 0; ///< The position inside the cached page.
    /// The definition levels of the page that is being decoded.
    std::vector<int64_t> levels_;
    PageValues page_;
    bool page_loaded_          = false;
  };

  /// Read the raw bytes of a page header at `offset`.
  static Status read_page_header(const std::string &path,
                                 int64_t offset,
                                 parquet_detail::PageHeader *header,
                                 int64_t *payload_offset,
                                 int64_t *payload_size,
                                 Error *error);

  /// Parse the footer of the file and fill in the schema and the row groups.
  Status parse_footer(const std::string &path, Error *error);

  /// Move to the next row group, preparing the column cursors.
  Status enter_next_row_group(Error *error);

  std::string path_;
  ReadOptions options_;
  std::vector<std::string> header_names_;
  std::vector<ColumnSchema> schema_;
  std::vector<parquet_detail::ColumnDesc> columns_;
  std::vector<parquet_detail::RowGroupDesc> row_groups_;
  std::vector<ColumnCursor> cursors_;
  /// The leaf columns that the scan produces, in output order.
  std::vector<size_t> projection_;
  bool projection_set_        = false;
  bool opened_                = false;
  bool footer_parsed_         = false;
  bool exhausted_             = false;
  size_t row_group_index_     = 0;
  int64_t row_count_          = 0;
  int64_t rows_emitted_       = 0;
  int64_t current_row_        = 0;
};

} // namespace file_scan
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_READER_H_
