#pragma once
#include "sql/resolver/dml/file_sql_bound_call.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/session/ob_sql_session_info.h"
namespace oceanbase { namespace sql {
inline int resolve_file_sql_copy(ObResolverParams &params,const ParseNode &node,
                                ObSelectStmt &stmt,ObSelectIntoItem &into)
{
  using namespace common;
  ObObj enabled;
  int ret=params.session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,enabled);
  if(ret)return ret;
  if(enabled.is_null()||enabled.get_int()==0){LOG_USER_ERROR(OB_NOT_SUPPORTED,"file SQL is disabled");return OB_NOT_SUPPORTED;}
  std::vector<ParseNode *> values{node.children_[0]};
  if(node.children_[1]!=nullptr)for(int i=0;i<node.children_[1]->num_child_;++i)values.push_back(node.children_[1]->children_[i]);
  ParseNode name{},args{},call{};
  name.type_=T_IDENT;name.str_value_="read_csv";name.str_len_=8;
  args.type_=T_EXPR_LIST;args.num_child_=values.size();args.children_=values.data();
  ParseNode *children[]={&name,&args};call.type_=T_FUN_SYS;call.num_child_=2;call.children_=children;
  FileSqlPlan plan;plan.options.header=false;
  ret=file_sql_parse_bound_call(call,params.param_list_,plan);if(ret)return ret;
  if(plan.options.sample_size!=-1||plan.options.delimiter==0||plan.options.quote==0||plan.options.escape==0||
     plan.options.delimiter==plan.options.quote||plan.options.delimiter=='\n'||plan.options.delimiter=='\r'||
     plan.options.quote=='\n'||plan.options.quote=='\r'||plan.options.escape=='\n'||plan.options.escape=='\r'||
     plan.options.nullstr.find_first_of(std::string()+plan.options.delimiter+plan.options.quote+"\r\n")!=std::string::npos)
    return OB_INVALID_ARGUMENT;
  for(int64_t i=0;i<stmt.get_select_item_size();++i){
    const auto &item=stmt.get_select_item(i);const ObString &n=item.alias_name_.empty()?item.expr_name_:item.alias_name_;
    plan.schema.columns.push_back({std::string(n.ptr(),n.length()),FileSqlType::VARCHAR,true});
  }
  if(plan.schema.columns.empty())return OB_NOT_SUPPORTED;
  const std::string descriptor=plan.encode();
  ret=ob_write_string(*params.allocator_,ObString(descriptor.size(),descriptor.data()),into.external_properties_);
  if(ret)return ret;
  ObString path;ret=ob_write_string(*params.allocator_,ObString(plan.path.size(),plan.path.data()),path);if(ret)return ret;
  into.outfile_name_.set_varchar(path);into.is_single_=true;
  stmt.get_query_ctx()->get_query_hint_for_update().get_global_hint().merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
  return OB_SUCCESS;
}
} }
