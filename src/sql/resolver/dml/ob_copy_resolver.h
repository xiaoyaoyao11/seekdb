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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_

#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/dml/ob_copy_stmt.h"
#include "sql/engine/basic/ob_file_copy_writer.h"

namespace oceanbase
{
namespace sql
{

/**
 * The resolver of the COPY statement.
 *
 * The COPY statement of the "file as database" feature exports the result of
 * a query or of a table into a CSV file:
 *
 *   COPY (SELECT id, name FROM read_csv('/tmp/a.csv') ORDER BY id)
 *     TO '/tmp/b.csv' (format csv, header, nullstr 'NULL');
 *
 * The statement is resolved into an ordinary select statement that carries
 * the parameters of the export in its select into item.  The operator that
 * writes the file of a SELECT ... INTO OUTFILE is therefore reused, it only
 * behaves differently when the select into item belongs to a COPY
 * statement: the file holds the names of the columns as its first line, a
 * NULL value is written as an empty field and an existing file is never
 * overwritten.
 */
class ObCopyResolver : public ObDMLResolver
{
public:
  explicit ObCopyResolver(ObResolverParams &params);
  virtual ~ObCopyResolver();
  virtual int resolve(const ParseNode &parse_tree);

  ObSelectStmt *get_copy_stmt() { return copy_stmt_; }

private:
  /// Parse the options of the statement.
  int resolve_copy_options(const ParseNode *options_node);
  /**
   * Read the text of the value of an option.  The value is either a string
   * constant or an identifier, the option "format csv" names its value with
   * an identifier.
   */
  int resolve_copy_option_text(const ParseNode &value_node, common::ObString &text);
  /// Resolve the path of the target file and check that it is allowed.
  int resolve_file_path(const ParseNode *path_node);
  /// Build the select into item that describes the export.
  int build_select_into_item();
  /// Collect the names of the columns of the exported result.
  int collect_column_names();

  ObSelectStmt *copy_stmt_;
  ObSelectIntoItem *into_item_;
  file_scan::CopyOptions copy_options_;
  common::ObString file_path_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DML_OB_COPY_RESOLVER_H_
