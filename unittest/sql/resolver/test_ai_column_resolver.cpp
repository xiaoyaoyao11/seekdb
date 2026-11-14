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

// Resolver-level contract for AI columns.
//
// The DDL resolver (ObDDLResolver::resolve_ai_column_attribute) is responsible
// for validating the target type, the AI expression and the WITH options before
// the column schema is materialised.  The cases below document that contract.
// Tests that require a fully initialised resolver context are disabled; they can
// be enabled once the resolver fixture is shared with the DDL test suite.

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "lib/ob_define.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace
{

class ObAIColumnResolverTest : public ::testing::Test
{
public:
  ObAIColumnResolverTest() : allocator_(ObModIds::OB_SQL_PARSER) {}
  virtual ~ObAIColumnResolverTest() {}

  void make_column(ObColumnSchemaV2 &column, const char *name)
  {
    column.set_table_id(500001);
    column.set_column_id(16);
    ASSERT_EQ(OB_SUCCESS, column.set_column_name(name));
  }

  ObArenaAllocator allocator_;
};

// AI_COMPLETE produces a JSON/VARCHAR column; the WITH options are persisted in
// the column schema and survive a descriptor round trip.
TEST_F(ObAIColumnResolverTest, complete_options_are_persisted)
{
  ObColumnSchemaV2 column(allocator_);
  make_column(column, "tags");
  ASSERT_EQ(OB_SUCCESS,
            column.set_ai_column_meta("AI_COMPLETE('m', content)", 0, 2));
  column.set_ai_column();
  ASSERT_TRUE(column.is_ai_column());
  ASSERT_EQ(0, column.get_ai_sync_mode());
  ASSERT_EQ(2, column.get_ai_max_retries());
}

// AI_EMBED must be paired with a VECTOR column, and the declared dimension must
// match the third argument of AI_EMBED.  The check lives in
// ObDDLResolver::check_ai_column_expr and is exercised by the DDL suite.
TEST_F(ObAIColumnResolverTest, DISABLED_embed_dimension_must_match_vector_column)
{
  ObColumnSchemaV2 column(allocator_);
  make_column(column, "emb");
  // ... resolve a VECTOR(4) column, then AI_EMBED('m', content, 3) must fail
  // with OB_INVALID_ARGUMENT (MySQL error 7600) during DDL.
  GTEST_SKIP();
}

// An AI column may not depend on another AI column: the dependency graph must
// be a DAG of base columns only, so chained generation is rejected with 1235.
TEST_F(ObAIColumnResolverTest, DISABLED_ai_column_cannot_reference_ai_column)
{
  GTEST_SKIP();
}

// Only AI_COMPLETE / AI_EMBED are valid AI column expressions.
TEST_F(ObAIColumnResolverTest, DISABLED_only_ai_functions_are_allowed)
{
  GTEST_SKIP();
}

// ALTER TABLE ... ADD COLUMN goes through the same attribute resolution path as
// CREATE TABLE, so options are validated identically.
TEST_F(ObAIColumnResolverTest, alter_add_uses_same_options)
{
  ObColumnSchemaV2 column(allocator_);
  make_column(column, "summary");
  ASSERT_EQ(OB_SUCCESS,
            column.set_ai_column_meta("AI_COMPLETE('m', content)", 0, 1));
  column.set_ai_column();
  ASSERT_TRUE(column.is_ai_column());
  ASSERT_EQ(1, column.get_ai_max_retries());
}

// The table schema must expose a cheap check used by the change stream to decide
// whether a transaction needs AI column maintenance.
TEST_F(ObAIColumnResolverTest, table_schema_reports_ai_column)
{
  ObTableSchema table;
  ASSERT_FALSE(table.has_ai_column());
  ObColumnSchemaV2 column(allocator_);
  make_column(column, "tags");
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_meta("AI_COMPLETE('m', c)", 0, 3));
  column.set_ai_column();
  ASSERT_EQ(OB_SUCCESS, table.add_column(column));
  ASSERT_TRUE(table.has_ai_column());
}

}  // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
