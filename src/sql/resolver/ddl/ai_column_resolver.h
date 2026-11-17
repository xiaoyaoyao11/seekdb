#pragma once
#include "share/schema/ai_column_schema.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/ob_item_type.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_ai_model_mgr.h"
#include <string>
#include <cstring>

namespace oceanbase { namespace sql {
namespace ai_column_detail {
inline common::ObString text(const ParseNode *n)
{ return n ? common::ObString(n->str_len_, n->str_value_) : common::ObString(); }
inline bool named(const ParseNode *n, const char *name)
{ return n && text(n).case_compare(name) == 0; }
inline void quoted(std::string &out, const common::ObString &s, char quote)
{
  if (quote == '\'') {
    // Persist SQL that is independent of BOTH the defining/business session's
    // SQL mode and the worker session's mode. The native charset-introduced
    // hex literal parses as T_VARCHAR (also on SHOW CREATE/recreate), not binary.
    // Keep safe strings readable; neither form contains SQL backslash escapes.
    bool hex = false;
    for (int i = 0; i < s.length(); ++i) {
      hex |= s.ptr()[i] == '\\' || static_cast<unsigned char>(s.ptr()[i]) < 0x20;
    }
    out += hex ? "_utf8mb4 X'" : "_utf8mb4'";
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < s.length(); ++i) {
      const unsigned char c = static_cast<unsigned char>(s.ptr()[i]);
      if (hex) {
        out += digits[c >> 4]; out += digits[c & 15];
      } else {
        if (c == '\'') out += '\'';
        out += c;
      }
    }
    out += '\'';
  } else {
    out += quote;
    for (int i = 0; i < s.length(); ++i) {
      if (s.ptr()[i] == quote) out += quote;
      out += s.ptr()[i];
    }
    out += quote;
  }
}
inline int copy(common::ObIAllocator &allocator, const std::string &s, common::ObString &out)
{ return common::ob_write_string(allocator, common::ObString(s.size(), s.data()), out); }

// Closed AST allowlist: no text scanning can confuse quoted text with a dependency,
// RAND/NOW, a subquery, a variable, a UDF, or a nested remote model invocation.
inline int input(const ParseNode *n, std::string &out,
                 common::ObIArray<common::ObString> &dependencies, int depth = 0)
{
  int ret = OB_SUCCESS;
  if (!n || depth > 64) return OB_ERR_INVALID_VECTOR_DIM;
  if (n->type_ == T_COLUMN_REF) {
    if (n->num_child_ != 3 || !n->children_[2] || n->children_[0] || n->children_[1]) return OB_NOT_SUPPORTED;
    common::ObString name = text(n->children_[2]);
    bool found = false;
    for (int64_t i = 0; i < dependencies.count(); ++i) found |= dependencies.at(i).case_compare(name) == 0;
    if (!found && OB_FAIL(dependencies.push_back(name))) return ret;
    quoted(out, name, '`');
  } else if (n->type_ == T_VARCHAR || n->type_ == T_CHAR) {
    quoted(out, text(n), '\'');
  } else if (n->type_ == T_INT || n->type_ == T_UINT64) {
    out += n->type_ == T_INT ? std::to_string(n->value_) : std::to_string(static_cast<uint64_t>(n->value_));
  } else if (n->type_ == T_NULL) {
    out += "NULL";
  } else if (n->type_ == T_BOOL) {
    out += n->value_ ? "TRUE" : "FALSE";
  } else if (n->type_ == T_FUN_SYS && n->num_child_ == 2 && n->children_[0] && n->children_[1]) {
    const ParseNode *name = n->children_[0];
    static const char *allowed[] = {"concat", "concat_ws", "lower", "upper", "trim", "ltrim", "rtrim",
      "substring", "substr", "replace", "coalesce", "ifnull", "nullif", "length", "char_length",
      "abs", "round", "json_object", "json_array", "json_extract", "json_unquote", "ai_prompt"};
    bool supported = false;
    for (auto candidate : allowed) supported |= named(name, candidate);
    if (!supported) return OB_ERR_INVALID_VECTOR_DIM;
    common::ObString func = text(name); out.append(func.ptr(), func.length()); out += '(';
    const ParseNode *args = n->children_[1];
    for (int i = 0; OB_SUCC(ret) && i < args->num_child_; ++i) {
      if (i) out += ',';
      ret = input(args->children_[i], out, dependencies, depth + 1);
    }
    out += ')';
  } else {
    const char *op = nullptr;
    switch (n->type_) {
      case T_OP_ADD: op = "+"; break;
      case T_OP_MINUS: op = "-"; break;
      case T_OP_MUL: op = "*"; break;
      case T_OP_DIV: op = "/"; break;
      case T_OP_MOD: op = "%"; break;
      case T_OP_EQ: op = "="; break;
      case T_OP_NE: op = "<>"; break;
      case T_OP_LT: op = "<"; break;
      case T_OP_LE: op = "<="; break;
      case T_OP_GT: op = ">"; break;
      case T_OP_GE: op = ">="; break;
      case T_OP_AND: op = " AND "; break;
      case T_OP_OR: op = " OR "; break;
      case T_OP_NEG: op = "-"; break;
      case T_OP_POS: op = "+"; break;
      default: return OB_ERR_INVALID_VECTOR_DIM;
    }
    if (n->num_child_ != 1 && n->num_child_ != 2) return OB_ERR_INVALID_VECTOR_DIM;
    out += '(';
    if (n->num_child_ == 1) out += op;
    ret = input(n->children_[0], out, dependencies, depth + 1);
    if (OB_SUCC(ret) && n->num_child_ == 2) { out += op; ret = input(n->children_[1], out, dependencies, depth + 1); }
    out += ')';
  }
  return ret;
}
}

inline const ParseNode *find_ai_column_attribute(const ParseNode *definition)
{
  if (!definition || definition->num_child_ < 3 || !definition->children_[2]) return nullptr;
  const ParseNode *attrs = definition->children_[2];
  for (int i = 0; i < attrs->num_child_; ++i) {
    if (attrs->children_[i] && attrs->children_[i]->type_ == T_AI_COLUMN) return attrs->children_[i];
  }
  return nullptr;
}

inline int resolve_ai_column_attribute(const ParseNode &attr, const share::schema::ObColumnSchemaV2 &column,
    common::ObIAllocator &allocator, share::schema::AiColumnDefinition &def,
    common::ObIArray<common::ObString> &dependencies, const ParseNode *type_node = nullptr)
{
  using namespace ai_column_detail;
  int ret = OB_SUCCESS;
  if (attr.type_ != T_AI_COLUMN || attr.num_child_ != 2 || !attr.children_ || !attr.children_[0]) return OB_ERR_INVALID_VECTOR_DIM;
  const ParseNode *call = attr.children_[0];
  if (call->type_ != T_FUN_SYS || call->num_child_ != 2 || !call->children_[0] || !call->children_[1]) return OB_ERR_INVALID_VECTOR_DIM;
  const ParseNode *name = call->children_[0], *args = call->children_[1];
  const bool embedding = named(name, "ai_embed");
  if ((!embedding && !named(name, "ai_complete")) || args->num_child_ < 2 || args->num_child_ > 3
      || !args->children_[0] || args->children_[0]->type_ != T_VARCHAR || text(args->children_[0]).empty()) return OB_ERR_INVALID_VECTOR_DIM;
  if (!column.is_nullable() || column.is_generated_column() || column.is_hidden()
      || column.is_invisible_column() || column.is_autoincrement()
      || !column.get_cur_default_value().is_null()) return OB_NOT_SUPPORTED;
  def.embedding = embedding; def.column_id = column.get_column_id();
  if (OB_FAIL(common::ob_write_string(allocator, column.get_column_name_str(), def.column_name))) return ret;
  if (OB_FAIL(common::ob_write_string(allocator, text(args->children_[0]), def.model_name))) return ret;
  if (embedding) {
    if (!type_node || type_node->type_ != T_COLLECTION || type_node->int32_values_[0] != 1
        || type_node->num_child_ != 1 || !type_node->children_[0]) return OB_ERR_INVALID_VECTOR_DIM;
    def.vector_dimension = type_node->children_[0]->value_;
    if (def.vector_dimension <= 0 || def.vector_dimension > 16000) return OB_ERR_INVALID_VECTOR_DIM;
    if (args->num_child_ == 3) {
      if (!args->children_[2] || args->children_[2]->type_ != T_INT) return OB_ERR_INVALID_VECTOR_DIM;
      if (args->children_[2]->value_ != def.vector_dimension) {
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, def.vector_dimension, args->children_[2]->value_);
        return OB_ERR_INVALID_VECTOR_DIM;
      }
    }
  } else if (!(column.is_json() || column.get_data_type() == common::ObIntType
                       || common::ob_is_string_type(column.get_data_type()))) {
    return OB_ERR_INVALID_VECTOR_DIM;
  } else if (args->num_child_ == 3) {
    // Provider config overrides (including messages/model) are not a DDL option.
    return OB_ERR_INVALID_VECTOR_DIM;
  }
  const ParseNode *options = attr.children_[1];
  bool sync_seen = false, retries_seen = false;
  if (options && options->type_ != T_EXPR_LIST) return OB_ERR_INVALID_VECTOR_DIM;
  for (int i = 0; options && OB_SUCC(ret) && i < options->num_child_; ++i) {
    const ParseNode *opt = options->children_[i];
    if (!opt || opt->type_ != T_OP_EQ || opt->num_child_ != 2 || !opt->children_[0] || !opt->children_[1]) return OB_ERR_INVALID_VECTOR_DIM;
    const ParseNode *key = opt->children_[0], *value = opt->children_[1];
    if (key->type_ != T_COLUMN_REF || key->num_child_ != 3 || key->children_[0] || key->children_[1]) return OB_ERR_INVALID_VECTOR_DIM;
    key = key->children_[2];
    if (named(key, "sync_mode") && !sync_seen && value->type_ == T_VARCHAR && named(value, "async")) sync_seen = true;
    else if (named(key, "max_retries") && !retries_seen && value->type_ == T_INT && value->value_ >= 0 && value->value_ <= 100) {
      retries_seen = true; def.max_retries = value->value_;
    } else return OB_ERR_INVALID_VECTOR_DIM;
  }
  std::string input_expr;
  if (OB_FAIL(input(args->children_[1], input_expr, dependencies))) return ret;
  if (OB_FAIL(copy(allocator, input_expr, def.input_expr))) return ret;
  const char *instruction = column.is_json() ? "Return exactly one valid JSON value"
    : column.get_data_type() == common::ObIntType ? "Return exactly one base-10 integer" : "";
  std::string normalized = embedding ? "AI_EMBED(" : "AI_COMPLETE(";
  quoted(normalized, def.model_name, '\''); normalized += ','; normalized += input_expr;
  std::string config;
  if (embedding) {
    normalized += ','; normalized += std::to_string(def.vector_dimension);
  } else if (instruction[0]) {
    // Override provider messages with a SYSTEM instruction while preserving the USER expression.
    config = "JSON_OBJECT('messages',JSON_ARRAY(JSON_OBJECT('role','system','content',";
    quoted(config, common::ObString::make_string(instruction), '\'');
    config += "),JSON_OBJECT('role','user','content',"; config += input_expr; config += ")))";
    normalized += ','; normalized += config;
  }
  normalized += ')';
  if (OB_FAIL(copy(allocator, config, def.config_expr))) return ret;
  return copy(allocator, normalized, def.normalized_expr);
}

inline int validate_ai_column_model(share::schema::ObSchemaGetterGuard &guard,
                                    const share::schema::AiColumnDefinition &definition)
{
  const share::schema::ObAiModelSchema *model = nullptr;
  int ret = guard.get_ai_model_schema(definition.model_name, model);
  if (OB_SUCC(ret) && !model) {
    ret = OB_AI_FUNC_MODEL_NOT_FOUND;
    LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND, definition.model_name.length(), definition.model_name.ptr());
  }
  if (OB_SUCC(ret) && model->get_type() != (definition.embedding
        ? share::EndpointType::DENSE_EMBEDDING : share::EndpointType::COMPLETION)) ret = OB_ERR_INVALID_VECTOR_DIM;
  return ret;
}

inline int make_ai_column_helper(const share::schema::ObColumnSchemaV2 &result,
    const share::schema::AiColumnDefinition &definition, const common::ObIArray<common::ObString> &dependencies,
    common::ObIAllocator &allocator, int role, uint64_t id, share::schema::ObColumnSchemaV2 &helper)
{
  using namespace share::schema;
  int ret = OB_SUCCESS;
  const std::string name = ai_column_helper_name(result.get_column_name_str(), role);
  helper.set_table_id(result.get_table_id());
  helper.set_column_id(id); helper.set_nullable(true); helper.set_is_hidden(true);
  helper.add_column_flag(AI_HELPER_COLUMN_FLAG);
  helper.set_data_type(role == 0 ? common::ObIntType : role == 1 ? common::ObLongTextType : common::ObVarcharType);
  helper.set_data_length(role == 1 ? common::ObAccuracy::MAX_ACCURACY[common::ObLongTextType].get_length() : role == 2 ? 1024 : 8);
  helper.set_charset_type(common::CHARSET_UTF8MB4); helper.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
  common::ObString encoded;
  common::ObSEArray<common::ObString, 1> info;
  if (OB_FAIL(helper.set_column_name(common::ObString(name.size(), name.data())))) {
  } else if (role == 2 && OB_FAIL(encode_ai_column_definition(definition, dependencies, allocator, encoded))) {
  } else {
    if (role != 2) encoded = common::ObString::make_string(role == 0 ? "AICS" : "AICQ");
    if (OB_FAIL(info.push_back(encoded))) {} else ret = helper.set_extended_type_info(info);
  }
  return ret;
}

inline int append_ai_column_helpers(share::schema::ObTableSchema &table,
    const share::schema::ObColumnSchemaV2 &result, const share::schema::AiColumnDefinition &definition,
    const common::ObIArray<common::ObString> &dependencies, common::ObIAllocator &allocator, uint64_t &id)
{
  int ret = OB_SUCCESS;
  // Copy before adding: table.add_column can reallocate the schema's pointer array.
  for (int role = 0; OB_SUCC(ret) && role < 3; ++role) {
    share::schema::ObColumnSchemaV2 helper;
    if (OB_FAIL(make_ai_column_helper(result, definition, dependencies, allocator, role, ++id, helper))) {}
    else if (table.get_column_schema(helper.get_column_name_str())) ret = OB_ERR_COLUMN_DUPLICATE;
    else ret = table.add_column(helper);
  }
  return ret;
}

// Call after all CREATE columns have been resolved, including later-declared AI columns.
inline int validate_ai_column_dependencies(const share::schema::ObTableSchema &table, common::ObIAllocator &allocator)
{
  common::ObSEArray<share::schema::AiColumnDefinition, 4> definitions;
  return share::schema::list_ai_column_definitions(table, allocator, definitions);
}
}}
