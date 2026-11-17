#pragma once
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/engine/basic/file_sql_plan.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include <string>

namespace oceanbase { namespace sql {
// DESCRIBE resolves the child for metadata only. Its rows are never executed or
// materialized. The small metadata relation is evaluated by the native engine.
inline int resolve_file_sql_describe(ObResolverParams &params, const ParseNode &node, ObStmt *&stmt)
{
  using namespace common;
  if(node.num_child_!=5 || node.children_[4]==nullptr || node.children_[4]->type_!=T_SELECT)
    return OB_INVALID_ARGUMENT;
  ObResolver resolver(params); ObStmt *child=nullptr;
  int ret=resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,*node.children_[4],child);
  if(ret!=OB_SUCCESS) return ret;
  if(child==nullptr || !child->is_select_stmt()) return OB_ERR_UNEXPECTED;
  ObSelectStmt *select=static_cast<ObSelectStmt *>(child);
  std::string query;
  auto literal=[](const ObString &s) {
    static const char hex[]="0123456789abcdef";
    std::string v="CONVERT(X'";
    for(int64_t i=0;i<s.length();++i){unsigned char c=s.ptr()[i];v+=hex[c>>4];v+=hex[c&15];}
    v+="' USING utf8mb4)";return v;
  };
  for(int64_t i=0;i<select->get_select_item_size();++i){
    const SelectItem &item=select->get_select_item(i);
    if(item.expr_==nullptr) return OB_ERR_UNEXPECTED;
    const auto &type=item.expr_->get_result_type();
    const char *name=ob_is_integer_type(type.get_type())?"BIGINT":
      ob_is_double_type(type.get_type())||ob_is_float_type(type.get_type())?"DOUBLE":
      type.is_date()?"DATE":type.is_datetime()?"DATETIME":type.is_null()?"NULL":"VARCHAR";
    bool nullable=!type.has_result_flag(NOT_NULL_FLAG);
    if(item.expr_->is_column_ref_expr()){
      auto *column=static_cast<ObColumnRefRawExpr *>(item.expr_);
      const TableItem *table=select->get_table_item_by_id(column->get_table_id());
      if(table!=nullptr && !table->file_sql_descriptor_.empty()){
        FileSqlPlan plan;ret=plan.decode(table->file_sql_descriptor_);if(ret) return ret;
        uint64_t ordinal=column->get_column_id()-OB_APP_MIN_COLUMN_ID;
        if(ordinal>=plan.schema.columns.size()) return OB_ERR_UNEXPECTED;
        const auto &c=plan.schema.columns[ordinal];
        static const char *names[]={"NULL","BIGINT","DOUBLE","BOOLEAN","VARCHAR","DATE","DATETIME"};
        name=names[static_cast<int>(c.type)];
      }
    }
    if(i) query+=" UNION ALL ";
    query+="SELECT ";query+=literal(item.alias_name_.empty()?item.expr_name_:item.alias_name_);
    query+=" AS column_name, ";query+=literal(ObString::make_string(name));
    query+=" AS column_type, ";query+=literal(ObString::make_string(nullable?"YES":"NO"));
    query+=" AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra";
  }
  if(query.empty()) return OB_INVALID_ARGUMENT;
  ObParser parser(*params.allocator_,params.session_info_->get_sql_mode());
  ParseResult parsed;
  ret=parser.parse(ObString(query.size(),query.data()),parsed);
  if(ret==OB_SUCCESS){
    if(parsed.result_tree_==nullptr || parsed.result_tree_->num_child_!=1 || parsed.result_tree_->children_[0]==nullptr)
      ret=OB_ERR_UNEXPECTED;
    else ret=resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,*parsed.result_tree_->children_[0],stmt);
  }
  parser.free_result(parsed);
  return ret;
}
} }
