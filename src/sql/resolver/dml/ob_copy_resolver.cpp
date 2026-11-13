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

#include "sql/resolver/dml/ob_copy_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/engine/basic/ob_file_copy_writer.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
/// The children of the T_COPY_STMT parse node.
enum ObCopyParseIdx
{
  COPY_PARSE_SOURCE = 0,
  COPY_PARSE_PATH   = 1,
  COPY_PARSE_OPTION = 2,
};

/// The children of the T_COPY_OPTION parse node.
enum ObCopyOptionIdx
{
  COPY_OPTION_NAME  = 0,
  COPY_OPTION_VALUE = 1,
};

} // anonymous namespace

ObCopyResolver::ObCopyResolver(ObResolverParams &params)
    : ObDMLResolver(params),
      copy_stmt_(NULL),
      into_item_(NULL),
      copy_options_(),
      file_path_()
{
}

ObCopyResolver::~ObCopyResolver()
{
}

int ObCopyResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  bool enable_file_sql = false;
  if (T_COPY_STMT != parse_tree.type_ || 3 != parse_tree.num_child_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected parse tree of the copy statement",
             "type", parse_tree.type_, "num_child", parse_tree.num_child_);
  } else {
    // the copy statement has to be enabled explicitly
    if (OB_ISNULL(session_info_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(session_info_->get_enable_file_sql(enable_file_sql))) {
    } else if (!enable_file_sql) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the copy statement");
    }
  }

  // the target of the export
  if (OB_SUCC(ret)) {
    if (OB_FAIL(resolve_file_path(parse_tree.children_[COPY_PARSE_PATH]))) {
    }
  }

  // the options of the statement
  if (OB_SUCC(ret)) {
    if (OB_FAIL(resolve_copy_options(parse_tree.children_[COPY_PARSE_OPTION]))) {
    }
  }

  // the query that is exported, the statement resolves into an ordinary
  // select statement that carries the parameters of the export
  if (OB_SUCC(ret)) {
    const ParseNode *source_node = parse_tree.children_[COPY_PARSE_SOURCE];
    if (OB_ISNULL(source_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the copy statement has no source");
    } else {
      ObSelectResolver select_resolver(params_);
      if (OB_FAIL(select_resolver.resolve(*source_node))) {
      } else if (OB_ISNULL(copy_stmt_ = select_resolver.get_select_stmt())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to resolve the source of the copy statement");
      } else {
        // the statement of the copy statement is the select statement that
        // holds the parameters of the export
        set_basic_stmt(copy_stmt_);
      }
    }
  }

  // the parameters of the export
  if (OB_SUCC(ret)) {
    if (OB_FAIL(build_select_into_item())) {
    }
  }

  // the names of the columns are the header of the exported file
  if (OB_SUCC(ret)) {
    if (OB_FAIL(collect_column_names())) {
    }
  }

  return ret;
}

int ObCopyResolver::resolve_copy_option_text(const ParseNode &value_node, ObString &text)
{
  int ret = OB_SUCCESS;
  if (T_IDENT == value_node.type_) {
    // the value of an option is either a string constant or an identifier,
    // the option "format csv" names its value with an identifier
    text.assign_ptr(value_node.str_value_, value_node.str_len_);
  } else {
    ret = resolve_str_const(value_node, text);
  }
  return ret;
}

int ObCopyResolver::resolve_copy_options(const ParseNode *options_node)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(options_node)) {
    // the options are optional, the defaults are used
  } else if (T_COPY_OPTIONS != options_node->type_ || 1 != options_node->num_child_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected parse tree of the options of the copy statement",
             "type", options_node->type_, "num_child", options_node->num_child_);
  } else {
    ParseNode *list_node = options_node->children_[0];
    if (OB_ISNULL(list_node)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < list_node->num_child_; ++i) {
        ParseNode *option_node = list_node->children_[i];
        if (OB_ISNULL(option_node) || T_COPY_OPTION != option_node->type_
            || 2 != option_node->num_child_) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected parse tree of an option of the copy statement");
        } else {
          ParseNode *name_node = option_node->children_[COPY_OPTION_NAME];
          ParseNode *value_node = option_node->children_[COPY_OPTION_VALUE];
          ObString option_name;
          if (OB_ISNULL(name_node) || OB_ISNULL(value_node)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            option_name.assign_ptr(name_node->str_value_, name_node->str_len_);
            if (0 == option_name.case_compare("format")) {
              // only the csv format is supported, the format of the exported
              // file is always a csv file
              ObString format_str;
              if (OB_FAIL(resolve_copy_option_text(*value_node, format_str))) {
              } else if (0 != format_str.case_compare("csv")) {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "the format of the copy statement");
              }
            } else if (0 == option_name.case_compare("header")) {
              if (T_BOOL != value_node->type_ && T_INT != value_node->type_) {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the header option of the copy statement");
              } else {
                copy_options_.has_header_ = (0 != value_node->value_);
              }
            } else if (0 == option_name.case_compare("delimiter")
                       || 0 == option_name.case_compare("sep")) {
              ObString delimiter_str;
              if (OB_FAIL(resolve_copy_option_text(*value_node, delimiter_str))) {
              } else if (1 != delimiter_str.length()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the delimiter of the copy statement");
              } else {
                copy_options_.delimiter_ = delimiter_str.ptr()[0];
              }
            } else if (0 == option_name.case_compare("quote")) {
              ObString quote_str;
              if (OB_FAIL(resolve_copy_option_text(*value_node, quote_str))) {
              } else if (1 != quote_str.length()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the quote of the copy statement");
              } else {
                copy_options_.quote_ = quote_str.ptr()[0];
              }
            } else if (0 == option_name.case_compare("escape")) {
              ObString escape_str;
              if (OB_FAIL(resolve_copy_option_text(*value_node, escape_str))) {
              } else if (1 != escape_str.length()) {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the escape of the copy statement");
              } else {
                copy_options_.escape_ = escape_str.ptr()[0];
              }
            } else if (0 == option_name.case_compare("nullstr")
                       || 0 == option_name.case_compare("null_string")) {
              ObString null_str;
              if (OB_FAIL(resolve_copy_option_text(*value_node, null_str))) {
              } else {
                copy_options_.null_string_ = std::string(null_str.ptr(), null_str.length());
              }
            } else {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "the option of the copy statement");
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObCopyResolver::resolve_file_path(const ParseNode *path_node)
{
  int ret = OB_SUCCESS;
  ObString path_str;
  ObString secure_file_priv;
  if (OB_ISNULL(path_node) || T_VARCHAR != path_node->type_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the copy statement has no target file");
  } else if (OB_FAIL(resolve_str_const(*path_node, path_str))) {
  } else if (path_str.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the target file of the copy statement");
  } else if (OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(session_info_->get_secure_file_priv(secure_file_priv))) {
  } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, path_str))) {
    if (OB_ERR_NO_PRIVILEGE == ret) {
      ret = OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
      LOG_WARN("failed to check the secure path of the copy statement",
               K(ret), K(secure_file_priv), K(path_str));
    }
  } else {
    file_path_ = path_str;
  }
  return ret;
}

int ObCopyResolver::build_select_into_item()
{
  int ret = OB_SUCCESS;
  common::ObIAllocator *allocator = allocator_;
  if (OB_ISNULL(allocator)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(into_item_ = static_cast<ObSelectIntoItem *>(
                          allocator->alloc(sizeof(ObSelectIntoItem))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    into_item_ = new (into_item_) ObSelectIntoItem();
    into_item_->into_type_ = T_INTO_OUTFILE;
    // the name of the target file
    into_item_->outfile_name_.set_varchar(file_path_);
    into_item_->outfile_name_.set_collation_type(ObCharset::get_system_collation());
    // the file is a csv file, the field terminator is the delimiter and the
    // line terminator is the line feed
    into_item_->field_str_.set_varchar(ObString((char *)&copy_options_.delimiter_, 1));
    into_item_->field_str_.set_collation_type(ObCharset::get_system_collation());
    into_item_->line_str_.set_varchar(DEFAULT_LINE_TERM_STR);
    into_item_->line_str_.set_collation_type(ObCharset::get_system_collation());
    // the values of the file are not escaped by the machinery of the
    // SELECT ... INTO OUTFILE, the writer of the COPY statement does the
    // escaping itself
    into_item_->escaped_cht_.meta_.set_char();
    into_item_->escaped_cht_.set_char_value(NULL, 0);
    into_item_->escaped_cht_.set_collation_type(ObCharset::get_system_collation());
    // the file is written by a single writer
    into_item_->is_single_ = true;
    // the parameters of the COPY statement
    into_item_->is_copy_ = true;
    into_item_->has_header_ = copy_options_.has_header_;
    if (!copy_options_.null_string_.empty()) {
      if (OB_FAIL(ob_write_string(*allocator,
                                  ObString(copy_options_.null_string_.data(),
                                           copy_options_.null_string_.size()),
                                  into_item_->null_string_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    if (OB_SUCC(ret)) {
      copy_stmt_->set_select_into(into_item_);
    }
  }
  return ret;
}

int ObCopyResolver::collect_column_names()
{
  int ret = OB_SUCCESS;
  // The names of the columns of the exported result are the header of the
  // file.  They are taken from the select items of the statement.
  const ObIArray<SelectItem> &select_items = copy_stmt_->get_select_items();
  for (int64_t i = 0; OB_SUCC(ret) && i < select_items.count(); ++i) {
    const SelectItem &item = select_items.at(i);
    ObString name;
    if (item.alias_name_.length() > 0) {
      name = item.alias_name_;
    } else if (item.expr_name_.length() > 0) {
      name = item.expr_name_;
    } else {
      // a column without a name is exported under the name of the column
      char buf[32];
      const int64_t len = snprintf(buf, sizeof(buf), "column_%ld", i + 1);
      if (len <= 0 || len >= static_cast<int64_t>(sizeof(buf))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to build the name of an exported column", K(i));
      } else {
        name = ObString(buf, len);
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(into_item_->column_names_.push_back(name))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to collect the names of the exported columns");
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
