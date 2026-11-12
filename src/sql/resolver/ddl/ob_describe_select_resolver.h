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

#ifndef OCEANBASE_SQL_RESOLVER_DDL_OB_DESCRIBE_SELECT_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_DDL_OB_DESCRIBE_SELECT_RESOLVER_H_

#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "lib/string/ob_string_buffer.h"

namespace oceanbase
{
namespace sql
{

/**
 * The resolver of the DuckDB style DESCRIBE statement.
 *
 *   DESCRIBE SELECT <select statement>
 *
 * The statement does not execute the select statement, it only reports the
 * column metadata of the statement as a result set that has the six columns
 *
 *   column_name | column_type | null | key | default | extra
 *
 * The result set is produced by building a select statement out of the
 * collected column metadata and by resolving it, so that the ordinary select
 * machinery takes care of the execution of the result set.
 */
class ObDescribeSelectResolver : public ObSelectResolver
{
public:
  explicit ObDescribeSelectResolver(ObResolverParams &params);
  virtual ~ObDescribeSelectResolver();

  virtual int resolve(const ParseNode &parse_tree);

  /**
   * The description of one column of the described select statement.
   */
  struct ColumnDesc
  {
    ColumnDesc() : name_(), type_name_(), nullable_(false) {}
    common::ObString name_;
    common::ObString type_name_;
    bool nullable_;
  };

  /**
   * Collect the column metadata of a resolved select statement.
   *
   * The columns of a file table keep the type that was inferred from the
   * content of the file, the columns of a normal table are mapped to the
   * names of the DuckDB types.
   */
  static int collect_columns(const ObSelectStmt &select_stmt,
                             common::ObIArray<ColumnDesc> &columns);

  /**
   * Map a type of a column of a normal table to the name of the DuckDB type.
   */
  static const char *ob_type_to_duckdb_name(common::ObObjType obj_type, int32_t precision = -1);

  /**
   * Map a type of a column of a file table to the name of the DuckDB type.
   */
  static const char *file_column_type_to_duckdb_name(int32_t col_type);

private:
  /**
   * Resolve the select statement that is described.
   */
  int resolve_described_select(const ParseNode &select_node, ObSelectStmt *&select_stmt);

  /**
   * Build the select statement that reports the described columns.
   */
  int build_describe_select_sql(const common::ObIArray<ColumnDesc> &columns,
                                common::ObString &describe_sql);

  /**
   * Parse and resolve the generated select statement.
   */
  int parse_and_resolve_describe_select(const common::ObString &describe_sql);

  /**
   * Quote a string so that it can be used as a literal of the generated sql.
   */
  static int append_sql_literal(common::ObStringBuffer &buf, const common::ObString &value);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DDL_OB_DESCRIBE_SELECT_RESOLVER_H_
