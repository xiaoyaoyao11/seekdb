/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <stdint.h>

namespace oceanbase
{
namespace common { class ObISQLClient; }
namespace share
{
namespace schema
{
class ObTableSchema;

// All writes use the caller's DDL transaction; this service never commits it.
class AiColumnSqlService
{
public:
  // The table must contain the final, remapped physical column IDs.
  static int sync_definitions(common::ObISQLClient &client, const ObTableSchema &table);
  static int delete_definitions(common::ObISQLClient &client,
                                uint64_t table_id, int64_t schema_version);
  static int delete_definition(common::ObISQLClient &client,
                               uint64_t table_id, uint64_t column_id, int64_t schema_version);
};
} // namespace schema
} // namespace share
} // namespace oceanbase
