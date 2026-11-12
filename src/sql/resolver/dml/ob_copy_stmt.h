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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_COPY_STMT_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_COPY_STMT_H_

#include "sql/engine/basic/ob_file_copy_writer.h"

namespace oceanbase
{
namespace sql
{

/**
 * The constants of the COPY statement of the "file as database" feature.
 *
 * The statement exports the result of a query or of a table into a CSV
 * file:
 *
 *   COPY (SELECT id, name FROM read_csv('/tmp/a.csv') ORDER BY id)
 *     TO '/tmp/b.csv' (format csv, header, nullstr 'NULL');
 *
 * The statement is resolved into an ordinary select statement that carries
 * the parameters of the export in its select into item, the operator that
 * writes the file of a SELECT ... INTO OUTFILE is therefore reused.
 */
enum ObCopyStmtType
{
  /// The type of the statement of the parse tree of a COPY statement.
  OB_COPY_STMT_TYPE = T_COPY_STMT,
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DML_OB_COPY_STMT_H_
