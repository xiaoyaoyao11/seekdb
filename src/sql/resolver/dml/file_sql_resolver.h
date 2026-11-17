#pragma once
#include "sql/engine/basic/file_sql_plan.h"
#include "share/ob_errno.h"
#include "sql/parser/parse_node.h"
#include <set>
#include <algorithm>

namespace oceanbase { namespace sql {
inline std::string file_sql_node_text(const ParseNode &n) {
  return n.str_value_ == nullptr ? std::string() : std::string(n.str_value_, n.str_len_);
}
inline std::string file_sql_lower(std::string s) {
  for (char &c:s) if (c>='A' && c<='Z') c+='a'-'A';
  return s;
}
inline bool file_sql_is_call(const ParseNode *n) {
  if (n==nullptr || n->type_!=T_FUN_SYS || n->num_child_!=2 || n->children_[0]==nullptr) return false;
  const std::string name=file_sql_lower(file_sql_node_text(*n->children_[0]));
  return name=="read_csv" || name=="read_json" || name=="read_ndjson" || name=="read_parquet" || name=="glob";
}
// Options are syntax, not executable expressions. Reject variables/subqueries,
// duplicate/unknown options and nonliteral values before touching the filesystem.
inline int file_sql_parse_call(const ParseNode &call, FileSqlPlan &plan) {
  using namespace common;
  const auto *args=call.children_[1];
  if (args==nullptr || args->num_child_<1 || args->children_[0]==nullptr || args->children_[0]->type_!=T_VARCHAR) return OB_INVALID_ARGUMENT;
  plan.path=file_sql_node_text(*args->children_[0]);
  if (plan.path.empty() || plan.path.find('\0')!=std::string::npos) return OB_INVALID_ARGUMENT;
  std::string name=file_sql_lower(file_sql_node_text(*call.children_[0]));
  plan.options.format=name=="glob" ? "glob" : name.substr(5);
  std::set<std::string> seen;
  for (int i=1;i<args->num_child_;++i) {
    const ParseNode *arg=args->children_[i];
    if (arg==nullptr || arg->type_!=T_OP_EQ || arg->num_child_!=2 || arg->children_[0]==nullptr || arg->children_[1]==nullptr) return OB_ERR_PARSE_SQL;
    const ParseNode *key=arg->children_[0], *val=arg->children_[1];
    if (key->type_!=T_COLUMN_REF || key->num_child_!=3 || key->children_[0]!=nullptr || key->children_[1]!=nullptr || key->children_[2]==nullptr) return OB_INVALID_ARGUMENT;
    std::string k=file_sql_lower(file_sql_node_text(*key->children_[2]));
    if (k=="delimiter") k="delim";
    if (!seen.insert(k).second) return OB_INVALID_ARGUMENT;
    if (k=="format") {
      if ((name!="read_json" && name!="read_ndjson") || val->type_!=T_VARCHAR ||
          file_sql_lower(file_sql_node_text(*val))!="newline_delimited") return OB_INVALID_ARGUMENT;
    } else if (k=="header") {
      if ((val->type_!=T_BOOL && val->type_!=T_INT) || (val->value_!=0 && val->value_!=1)) return OB_INVALID_ARGUMENT;
      plan.options.header=val->value_!=0;
    } else if (k=="delim" || k=="quote" || k=="escape" || k=="nullstr") {
      if (val->type_!=T_VARCHAR) return OB_INVALID_ARGUMENT;
      const std::string text=file_sql_node_text(*val);
      if (text.find('\0')!=std::string::npos) return OB_INVALID_ARGUMENT;
      if (k=="nullstr") plan.options.nullstr=text;
      else {
        if (text.size()!=1) return OB_INVALID_ARGUMENT;
        if (k=="delim") plan.options.delimiter=text[0];
        else if (k=="quote") plan.options.quote=text[0];
        else plan.options.escape=text[0];
      }
    } else if (k=="sample_size") {
      int64_t value=0;
      if (val->type_==T_INT) value=val->value_;
      else if (val->type_==T_OP_NEG && val->num_child_==1 && val->children_[0]!=nullptr && val->children_[0]->type_==T_INT) value=-val->children_[0]->value_;
      else return OB_INVALID_ARGUMENT;
      if (value!=-1 && value<=0) return OB_INVALID_ARGUMENT;
      plan.options.sample_size=value;
    } else return OB_INVALID_ARGUMENT;
  }
  return OB_SUCCESS;
}
} }
