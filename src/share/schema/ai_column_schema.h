#pragma once
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include <string>
#include <cstring>

namespace oceanbase { namespace share { namespace schema {
// Strings returned by list_ai_column_definitions belong to its allocator, not the schema.
struct AiColumnDefinition
{
  uint64_t column_id = OB_INVALID_ID;
  uint64_t state_column_id = OB_INVALID_ID;
  uint64_t request_column_id = OB_INVALID_ID;
  uint64_t result_spec_column_id = OB_INVALID_ID;
  int64_t definition_version = 1;
  int64_t max_retries = 3;
  int64_t vector_dimension = 0;
  bool embedding = false;
  common::ObString column_name, normalized_expr, input_expr, model_name, config_expr;
  common::ObSEArray<uint64_t, 4> dependency_ids;
};

inline std::string ai_column_helper_name(const common::ObString &name, int role)
{
  static const char *suffix[] = {"_state", "_request", "_result_spec"};
  std::string result("__ai_");
  result.append(name.ptr(), name.length());
  result += suffix[role];
  return result;
}

namespace ai_column_codec {
static const uint32_t MAX_FIELD = 1024 * 1024;
static const uint32_t MAX_DEPENDENCIES = 4096;
inline void put_u32(std::string &s, uint32_t value)
{
  for (int i = 0; i < 4; ++i) { s.push_back(static_cast<char>(value & 255)); value >>= 8; }
}
inline bool get_u32(const common::ObString &s, int64_t &pos, uint32_t &value)
{
  if (pos < 0 || pos > s.length() - 4) return false;
  value = 0;
  for (int i = 0; i < 4; ++i) value |= uint32_t(static_cast<unsigned char>(s.ptr()[pos++])) << (8 * i);
  return true;
}
inline int put_string(std::string &s, const common::ObString &value)
{
  if (value.length() < 0 || value.length() > MAX_FIELD) return OB_SIZE_OVERFLOW;
  put_u32(s, value.length());
  if (!value.empty()) s.append(value.ptr(), value.length());
  return OB_SUCCESS;
}
inline int get_string(const common::ObString &s, int64_t &pos, common::ObIAllocator &allocator,
                      common::ObString &value)
{
  uint32_t len = 0;
  if (!get_u32(s, pos, len) || len > MAX_FIELD || len > s.length() - pos) return OB_INVALID_DATA;
  const int ret = common::ob_write_string(allocator, common::ObString(len, s.ptr() + pos), value);
  pos += len;
  return ret;
}
}

// No provisional column IDs are serialized: rootservice reassigns them for ALTER ADD.
// Versioned length-prefixed bytes live only in the result_spec helper's extended_type_info.
inline int encode_ai_column_definition(const AiColumnDefinition &d,
    const common::ObIArray<common::ObString> &dependencies,
    common::ObIAllocator &allocator, common::ObString &encoded)
{
  using namespace ai_column_codec;
  int ret = OB_SUCCESS;
  if (d.definition_version != 1 || d.max_retries < 0 || d.max_retries > 100
      || d.vector_dimension < 0 || d.vector_dimension > 65536
      || dependencies.count() > MAX_DEPENDENCIES) return OB_INVALID_ARGUMENT;
  std::string bytes("AIC1");
  put_u32(bytes, d.definition_version); put_u32(bytes, d.max_retries);
  put_u32(bytes, d.vector_dimension); put_u32(bytes, d.embedding ? 1 : 0);
  const common::ObString fields[] = {d.column_name, d.normalized_expr, d.input_expr, d.model_name, d.config_expr};
  for (const auto &field : fields) if (OB_SUCC(ret)) ret = put_string(bytes, field);
  put_u32(bytes, dependencies.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies.count(); ++i) ret = put_string(bytes, dependencies.at(i));
  if (OB_SUCC(ret)) ret = common::ob_write_string(allocator, common::ObString(bytes.size(), bytes.data()), encoded);
  return ret;
}

inline int decode_ai_column_definition(const common::ObString &encoded,
    common::ObIAllocator &allocator, AiColumnDefinition &d,
    common::ObIArray<common::ObString> &dependencies)
{
  using namespace ai_column_codec;
  int ret = OB_SUCCESS;
  int64_t pos = 4;
  uint32_t version = 0, retries = 0, dimension = 0, embedding = 0, count = 0;
  if (encoded.length() < 4 || 0 != MEMCMP(encoded.ptr(), "AIC1", 4)
      || !get_u32(encoded, pos, version) || version != 1
      || !get_u32(encoded, pos, retries) || retries > 100
      || !get_u32(encoded, pos, dimension) || dimension > 65536
      || !get_u32(encoded, pos, embedding) || embedding > 1) return OB_INVALID_DATA;
  d.definition_version = version; d.max_retries = retries;
  d.vector_dimension = dimension; d.embedding = embedding != 0;
  common::ObString *fields[] = {&d.column_name, &d.normalized_expr, &d.input_expr, &d.model_name, &d.config_expr};
  for (auto field : fields) if (OB_SUCC(ret)) ret = get_string(encoded, pos, allocator, *field);
  if (OB_SUCC(ret) && (!get_u32(encoded, pos, count) || count > MAX_DEPENDENCIES)) ret = OB_INVALID_DATA;
  for (uint32_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    common::ObString dep;
    if (OB_FAIL(get_string(encoded, pos, allocator, dep))) {
    } else if (OB_FAIL(dependencies.push_back(dep))) {}
  }
  if (OB_SUCC(ret) && pos != encoded.length()) ret = OB_INVALID_DATA;
  return ret;
}

inline int list_ai_column_definitions(const ObTableSchema &table, common::ObIAllocator &allocator,
                                      common::ObIArray<AiColumnDefinition> &out)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 *result = table.get_column_schema_by_idx(i);
    if (nullptr == result) { ret = OB_ERR_UNEXPECTED; }
    else if (result->is_ai_column()) {
      const ObColumnSchemaV2 *helpers[3] = {};
      for (int role = 0; OB_SUCC(ret) && role < 3; ++role) {
        const std::string name = ai_column_helper_name(result->get_column_name_str(), role);
        helpers[role] = table.get_column_schema(common::ObString(name.size(), name.data()));
        if (!helpers[role] || !helpers[role]->is_ai_helper_column() || !helpers[role]->is_hidden()) ret = OB_INVALID_DATA;
      }
      AiColumnDefinition d;
      common::ObSEArray<common::ObString, 4> dependencies;
      if (OB_SUCC(ret)) {
        const auto &info = helpers[2]->get_extended_type_info();
        if (info.count() != 1) ret = OB_INVALID_DATA;
        else ret = decode_ai_column_definition(info.at(0), allocator, d, dependencies);
      }
      if (OB_SUCC(ret) && d.column_name.case_compare(result->get_column_name_str()) != 0) ret = OB_INVALID_DATA;
      for (int64_t j = 0; OB_SUCC(ret) && j < dependencies.count(); ++j) {
        const ObColumnSchemaV2 *dep = table.get_column_schema(dependencies.at(j));
        if (!dep || dep->is_ai_column() || dep->is_ai_helper_column() || dep->is_generated_column()) ret = OB_NOT_SUPPORTED;
        else ret = d.dependency_ids.push_back(dep->get_column_id());
      }
      if (OB_SUCC(ret)) {
        d.column_id = result->get_column_id();
        d.state_column_id = helpers[0]->get_column_id();
        d.request_column_id = helpers[1]->get_column_id();
        d.result_spec_column_id = helpers[2]->get_column_id();
        ret = out.push_back(d);
      }
    }
  }
  return ret;
}
}}}
