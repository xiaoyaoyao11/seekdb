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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_describe_select_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
namespace sql
{

ObDescribeSelectResolver::ObDescribeSelectResolver(ObResolverParams &params)
    : ObSelectResolver(params)
{
}

ObDescribeSelectResolver::~ObDescribeSelectResolver()
{
}

/**
 * The names of the six columns of the result set of a DESCRIBE statement.
 *
 * The names follow the DuckDB style, they are lower case.  Three of them
 * are reserved words of MySQL, they are quoted with a backtick in the
 * generated statement.
 */
static constexpr char DESCRIBE_COLUMN_NAME[] = "column_name";
static constexpr char DESCRIBE_COLUMN_TYPE[] = "column_type";
static constexpr char DESCRIBE_COLUMN_NULL[] = "null";
static constexpr char DESCRIBE_COLUMN_KEY[] = "key";
static constexpr char DESCRIBE_COLUMN_DEFAULT[] = "default";
static constexpr char DESCRIBE_COLUMN_EXTRA[] = "extra";

/**
 * Map the type of a column of a normal table to the name of the DuckDB type.
 *
 * The file tables have their own set of types, they are described by
 * file_column_type_to_duckdb_name.
 *
 * A tinyint that has a precision of one is the boolean of MySQL, every other
 * integer is reported as a BIGINT.
 */
const char *ObDescribeSelectResolver::ob_type_to_duckdb_name(common::ObObjType obj_type,
                                                            int32_t precision)
{
  const char *type_name = "VARCHAR";
  switch (obj_type) {
    case common::ObNullType:
      type_name = "NULL";
      break;
    case common::ObTinyIntType:
      if (1 == precision) {
        type_name = "BOOLEAN";
      } else {
        type_name = "BIGINT";
      }
      break;
    case common::ObIntType:
    case common::ObInt32Type:
    case common::ObMediumIntType:
    case common::ObSmallIntType:
    case common::ObUTinyIntType:
    case common::ObUSmallIntType:
    case common::ObUMediumIntType:
    case common::ObUInt32Type:
    case common::ObUInt64Type:
      type_name = "BIGINT";
      break;
    case common::ObFloatType:
    case common::ObDoubleType:
    case common::ObUFloatType:
    case common::ObUDoubleType:
      type_name = "DOUBLE";
      break;
    case common::ObDateType:
      type_name = "DATE";
      break;
    case common::ObDateTimeType:
    case common::ObTimestampType:
      type_name = "DATETIME";
      break;
    case common::ObVarcharType:
    case common::ObCharType:
    case common::ObHexStringType:
    case common::ObTinyTextType:
    case common::ObTextType:
    case common::ObMediumTextType:
    case common::ObLongTextType:
      type_name = "VARCHAR";
      break;
    default:
      type_name = "VARCHAR";
      break;
  }
  return type_name;
}

/**
 * Map the type of a column of a file table to the name of the DuckDB type.
 *
 * The values of the enumeration are the same as the ones of
 * file_scan::ColumnType, so the names are exactly the ones that
 * file_scan::to_string returns.
 */
const char *ObDescribeSelectResolver::file_column_type_to_duckdb_name(int32_t col_type)
{
  const char *type_name = "VARCHAR";
  switch (col_type) {
    case OB_FILE_COLUMN_NULL:
      type_name = "NULL";
      break;
    case OB_FILE_COLUMN_BOOLEAN:
      type_name = "BOOLEAN";
      break;
    case OB_FILE_COLUMN_BIGINT:
      type_name = "BIGINT";
      break;
    case OB_FILE_COLUMN_DOUBLE:
      type_name = "DOUBLE";
      break;
    case OB_FILE_COLUMN_DATE:
      type_name = "DATE";
      break;
    case OB_FILE_COLUMN_DATETIME:
      type_name = "DATETIME";
      break;
    case OB_FILE_COLUMN_VARCHAR:
    default:
      type_name = "VARCHAR";
      break;
  }
  return type_name;
}

/**
 * Collect the column metadata of a resolved select statement.
 *
 * The select items of a star are already expanded by the resolver, so the
 * items of the statement are exactly the columns that the statement returns.
 */
int ObDescribeSelectResolver::collect_columns(const ObSelectStmt &select_stmt,
                                              common::ObIArray<ColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<SelectItem> &select_items = select_stmt.get_select_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    const SelectItem &select_item = select_items.at(i);
    if (OB_ISNULL(select_item.expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the select item of the described statement has no expression");
    } else {
      ColumnDesc column;
      // the name of the column, an alias wins over the name of the column
      if (!select_item.alias_name_.empty()) {
        column.name_ = select_item.alias_name_;
      } else if (!select_item.expr_name_.empty()) {
        column.name_ = select_item.expr_name_;
      } else {
        column.name_ = select_item.expr_->get_column_name();
      }
      // an expression without a name is described by its text
      if (column.name_.empty()) {
        char name_buf[OB_MAX_COLUMN_NAME_LENGTH];
        int64_t pos = 0;
        if (OB_SUCC(select_item.expr_->get_name(name_buf, sizeof(name_buf), pos))
            && 0 < pos) {
          column.name_ = common::ObString(name_buf, static_cast<int32_t>(pos));
        }
      }
      // the type of the column, a column of a file table keeps the type that
      // was inferred from the content of the file
      const ObColumnRefRawExpr *column_ref = select_item.expr_->is_column_ref_expr()
                                             ? static_cast<const ObColumnRefRawExpr *>(select_item.expr_)
                                             : NULL;
      if (OB_NOT_NULL(column_ref)) {
        // the table item is needed as a mutable object, the getter of the file
        // table definition is not a const one
        TableItem *table_item = const_cast<ObSelectStmt &>(select_stmt)
                                 .get_table_item_by_id(column_ref->get_table_id());
        if (OB_NOT_NULL(table_item) && table_item->is_file_table()) {
          const ObFileTableDef *table_def = table_item->get_file_table_def();
          if (OB_NOT_NULL(table_def)) {
            for (int64_t j = 0; j < table_def->all_cols_.count(); ++j) {
              const ObFileTableColDef *col_def = table_def->all_cols_.at(j);
              if (OB_NOT_NULL(col_def) && col_def->column_id_ == column_ref->get_column_id()) {
                column.type_name_ = file_column_type_to_duckdb_name(col_def->col_type_);
                column.nullable_ = col_def->nullable_;
                break;
              }
            }
          }
        }
      }
      if (column.type_name_.empty()) {
        const sql::ObRawExprResType &result_type = select_item.expr_->get_result_type();
        column.type_name_ = ob_type_to_duckdb_name(result_type.get_type(),
                                                   static_cast<int32_t>(result_type.get_precision()));
        // an expression is nullable unless it is known to be not null
        column.nullable_ = !select_item.expr_->is_not_null_for_read();
      }
      if (OB_FAIL(columns.push_back(column))) {
        LOG_WARN("failed to collect the column of the described statement", K(ret));
      }
    }
  }
  return ret;
}

/**
 * Quote a string so that it can be used as a literal of the generated sql.
 */
int ObDescribeSelectResolver::append_sql_literal(common::ObStringBuffer &buf,
                                                  const common::ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(buf.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if ('\\' == c || '\'' == c) {
        if (OB_FAIL(buf.append("\\", 1))) {
          break;
        }
      }
      if (OB_FAIL(buf.append(&c, 1))) {
        break;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(buf.append("'"))) {
    } else if (OB_SUCC(ret)) {
      // nothing to do
    }
  }
  return ret;
}

/**
 * Build the select statement that reports the described columns.
 *
 * The statement has one row per described column, the three columns that
 * DuckDB leaves empty are reported as SQL NULL.
 *
 * The names of the columns are quoted with a backtick because three of them
 * are reserved words of MySQL, a backtick is always the quote of an
 * identifier, independently of the sql mode of the session.
 */
int ObDescribeSelectResolver::build_describe_select_sql(const common::ObIArray<ColumnDesc> &columns,
                                                        common::ObString &describe_sql)
{
  int ret = OB_SUCCESS;
  // the buffer has to know the allocator, otherwise it refuses to grow
  common::ObStringBuffer buf(allocator_);
  if (0 == columns.count()) {
    // there is nothing to describe, the result set is empty but it still has
    // the six columns
    if (OB_FAIL(buf.append("SELECT NULL AS `"))) {
    } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_NAME))) {
    } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
    } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_TYPE))) {
    } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
    } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_NULL))) {
    } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
    } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_KEY))) {
    } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
    } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_DEFAULT))) {
    } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
    } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_EXTRA))) {
    } else if (OB_FAIL(buf.append("` FROM dual WHERE 1 = 0"))) {
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      const ColumnDesc &column = columns.at(i);
      if (i > 0 && OB_FAIL(buf.append(" UNION ALL "))) {
        break;
      }
      if (OB_FAIL(buf.append("SELECT "))) {
      } else if (OB_FAIL(append_sql_literal(buf, column.name_))) {
      } else if (OB_FAIL(buf.append(" AS `"))) {
      } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_NAME))) {
      } else if (OB_FAIL(buf.append("`, "))) {
      } else if (OB_FAIL(append_sql_literal(buf, column.type_name_))) {
      } else if (OB_FAIL(buf.append(" AS `"))) {
      } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_TYPE))) {
      } else if (OB_FAIL(buf.append("`, "))) {
      } else if (OB_FAIL(append_sql_literal(buf, column.nullable_ ? "YES" : "NO"))) {
      } else if (OB_FAIL(buf.append(" AS `"))) {
      } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_NULL))) {
      } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
      } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_KEY))) {
      } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
      } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_DEFAULT))) {
      } else if (OB_FAIL(buf.append("`, NULL AS `"))) {
      } else if (OB_FAIL(buf.append(DESCRIBE_COLUMN_EXTRA))) {
      } else if (OB_FAIL(buf.append("` FROM dual"))) {
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(*allocator_, buf.string(), describe_sql))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

/**
 * Parse and resolve the generated select statement.
 *
 * The statement is resolved by the select resolver of this object, so that the
 * result set of the DESCRIBE statement is an ordinary select statement.
 */
int ObDescribeSelectResolver::parse_and_resolve_describe_select(const common::ObString &describe_sql)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session_info_) || OB_ISNULL(params_.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ParseResult select_result;
    ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
    if (OB_FAIL(parser.parse(describe_sql, select_result))) {
      LOG_WARN("failed to parse the generated describe statement", K(ret));
    } else if (OB_ISNULL(select_result.result_tree_)
               || 1 != select_result.result_tree_->num_child_
               || OB_ISNULL(select_result.result_tree_->children_)
               || OB_ISNULL(select_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected parse tree of the generated describe statement");
    } else if (OB_FAIL(ObSelectResolver::resolve(*select_result.result_tree_->children_[0]))) {
      LOG_WARN("failed to resolve the generated describe statement", K(ret));
    } else {
      // nothing to do
    }
  }
  return ret;
}

/**
 * Resolve the select statement that is described.
 *
 * The statement is resolved by a child resolver, so that the columns of a
 * file table are inferred while the statement is resolved.
 */
int ObDescribeSelectResolver::resolve_described_select(const ParseNode &select_node,
                                                        ObSelectStmt *&select_stmt)
{
  int ret = OB_SUCCESS;
  ObStmt *child_stmt = NULL;
  ObResolver resolver(params_);
  if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, select_node, child_stmt))) {
    LOG_WARN("failed to resolve the described statement", K(ret));
  } else if (OB_ISNULL(child_stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to generate the statement of the described select");
  } else if (stmt::T_SELECT != child_stmt->get_stmt_type()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the statement of DESCRIBE");
  } else {
    select_stmt = static_cast<ObSelectStmt *>(child_stmt);
  }
  // the child resolver has changed the type of the statement of the session,
  // the statement of the session is the DESCRIBE statement
  if (OB_NOT_NULL(session_info_)) {
    session_info_->set_stmt_type(stmt::T_DESCRIBE_SELECT_STMT);
  }
  return ret;
}

int ObDescribeSelectResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *select_stmt = NULL;
  common::ObSEArray<ColumnDesc, 8, common::ModulePageAllocator, true> columns;
  common::ObString describe_sql;
  if (T_DESCRIBE_SELECT_STMT != parse_tree.type_ || 1 != parse_tree.num_child_
      || OB_ISNULL(parse_tree.children_) || OB_ISNULL(parse_tree.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid DESCRIBE SELECT syntax", K(parse_tree.type_), K(parse_tree.num_child_));
  } else if (OB_FAIL(resolve_described_select(*parse_tree.children_[0], select_stmt))) {
  } else if (OB_FAIL(collect_columns(*select_stmt, columns))) {
  } else if (OB_FAIL(build_describe_select_sql(columns, describe_sql))) {
  } else if (OB_FAIL(parse_and_resolve_describe_select(describe_sql))) {
  }
  if (OB_LIKELY(OB_SUCCESS == ret && OB_NOT_NULL(stmt_))) {
    if (OB_UNLIKELY(stmt::T_SELECT != stmt_->get_stmt_type())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected stmt type", K(stmt_->get_stmt_type()));
    } else {
      // the statement reports the columns of another statement, it does not
      // change the state of the session
      ObSelectStmt *describe_stmt = static_cast<ObSelectStmt *>(stmt_);
      describe_stmt->set_select_type(NOT_AFFECT_FOUND_ROWS);
      if (OB_FAIL(describe_stmt->formalize_stmt(session_info_))) {
        LOG_WARN("failed to formalize the statement of DESCRIBE", K(ret));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
