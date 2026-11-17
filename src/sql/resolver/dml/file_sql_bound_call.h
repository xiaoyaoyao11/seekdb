#pragma once
#include "sql/resolver/dml/file_sql_resolver.h"
#include "sql/resolver/ob_resolver_define.h"
#include <deque>
#include <functional>

namespace oceanbase { namespace sql {
// Fast parameterization replaces SQL literals before normal resolution. Bind
// only their stored scalar values, never evaluate variables or SQL expressions.
inline int file_sql_parse_bound_call(const ParseNode &call, const ParamStore *params, FileSqlPlan &plan)
{
  using namespace common;
  int ret=OB_SUCCESS;
  std::deque<ParseNode> nodes;
  std::deque<std::vector<ParseNode *>> edges;
  std::function<ParseNode *(const ParseNode *,int)> clone=[&](const ParseNode *src,int depth)->ParseNode * {
    if(src==nullptr || ret!=OB_SUCCESS) return nullptr;
    if(depth>16 || nodes.size()>256){ret=OB_SIZE_OVERFLOW;return nullptr;}
    nodes.push_back(*src);ParseNode *dst=&nodes.back();
    if(src->type_==T_QUESTIONMARK){
      const int64_t i=src->value_;
      if(params==nullptr || i<0 || i>=params->count()){ret=OB_INVALID_ARGUMENT;return dst;}
      const ObObjParam &v=params->at(i);
      if(v.is_null()){ret=OB_INVALID_ARGUMENT;return dst;}
      if(ob_is_string_type(v.get_type())){
        ObString text=v.get_string();dst->type_=T_VARCHAR;dst->str_value_=text.ptr();dst->str_len_=text.length();
      }else if(ob_is_integer_type(v.get_type())){
        dst->type_=T_INT;dst->value_=v.get_int();
      }else{ret=OB_INVALID_ARGUMENT;}
      dst->num_child_=0;dst->children_=nullptr;
    }else if(src->num_child_>0){
      edges.emplace_back(src->num_child_,nullptr);auto &children=edges.back();
      dst->children_=children.data();
      for(int64_t i=0;i<src->num_child_ && ret==OB_SUCCESS;++i)children[i]=clone(src->children_[i],depth+1);
    }
    return dst;
  };
  ParseNode *bound=clone(&call,0);
  return ret==OB_SUCCESS ? file_sql_parse_call(*bound,plan) : ret;
}
} }
