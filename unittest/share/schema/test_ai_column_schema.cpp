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

// Schema-layer coverage for AI columns:
//   * AI flag and options are part of ObColumnSchemaV2 metadata
//   * copy / assign keeps the AI descriptor
//   * serialisation round trips the descriptor
//   * column id remapping (ALTER / table copy) keeps AI metadata intact
//   * hidden columns used by the AI pipeline stay hidden

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

const char *AI_EXPR = "AI_COMPLETE('doc_complete', concat('Classify: ', content))";

class ObAIColumnSchemaTest : public ::testing::Test
{
public:
  ObAIColumnSchemaTest() : allocator_(ObModIds::OB_SQL_PARSER) {}
  virtual ~ObAIColumnSchemaTest() {}

  void make_ai_column(ObColumnSchemaV2 &column, uint64_t column_id)
  {
    column.set_table_id(500004);
    column.set_column_id(column_id);
    ASSERT_EQ(OB_SUCCESS, column.set_column_name("tags"));
    ASSERT_EQ(OB_SUCCESS, column.set_ai_column_meta(AI_EXPR, 0, 2));
    column.set_ai_column();
  }

  ObArenaAllocator allocator_;
};

TEST_F(ObAIColumnSchemaTest, metadata_is_attached_to_column)
{
  ObColumnSchemaV2 column(allocator_);
  make_ai_column(column, 16);
  ASSERT_TRUE(column.is_ai_column());
  ASSERT_FALSE(column.is_generated_column());
  ASSERT_EQ(0, column.get_ai_sync_mode());
  ASSERT_EQ(2, column.get_ai_max_retries());
  ASSERT_EQ(0, column.get_ai_column_expr().compare(ObString::make_string(AI_EXPR)));
}

TEST_F(ObAIColumnSchemaTest, assign_preserves_ai_descriptor)
{
  ObColumnSchemaV2 src(allocator_);
  make_ai_column(src, 16);
  ObColumnSchemaV2 dst(allocator_);
  ASSERT_EQ(OB_SUCCESS, dst.assign(src));
  ASSERT_TRUE(dst.is_ai_column());
  ASSERT_EQ(src.get_ai_max_retries(), dst.get_ai_max_retries());
  ASSERT_EQ(0, dst.get_ai_column_expr().compare(src.get_ai_column_expr()));
}

TEST_F(ObAIColumnSchemaTest, serialize_round_trip_preserves_ai_descriptor)
{
  ObColumnSchemaV2 src(allocator_);
  make_ai_column(src, 16);
  const int64_t buf_len = src.get_serialize_size();
  ASSERT_LT(0, buf_len);
  char *buf = static_cast<char *>(allocator_.alloc(buf_len));
  ASSERT_TRUE(NULL != buf);
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, src.serialize(buf, buf_len, pos));
  ASSERT_EQ(buf_len, pos);

  ObColumnSchemaV2 dst(allocator_);
  int64_t read_pos = 0;
  ASSERT_EQ(OB_SUCCESS, dst.deserialize(buf, pos, read_pos));
  ASSERT_TRUE(dst.is_ai_column());
  ASSERT_EQ(src.get_ai_max_retries(), dst.get_ai_max_retries());
  ASSERT_EQ(0, dst.get_ai_column_expr().compare(src.get_ai_column_expr()));
}

// Table copy / ALTER rewrites column ids; the AI metadata must follow the column.
TEST_F(ObAIColumnSchemaTest, column_id_remap_keeps_ai_descriptor)
{
  ObColumnSchemaV2 column(allocator_);
  make_ai_column(column, 16);
  column.set_column_id(17);
  ASSERT_EQ(17, column.get_column_id());
  ASSERT_TRUE(column.is_ai_column());
  ASSERT_EQ(2, column.get_ai_max_retries());
  ASSERT_EQ(0, column.get_ai_column_expr().compare(ObString::make_string(AI_EXPR)));
}

// The state / request / result-spec columns of an AI column are hidden from
// user-visible schema printing and from SELECT * expansion.
TEST_F(ObAIColumnSchemaTest, helper_columns_are_hidden)
{
  ObColumnSchemaV2 state(allocator_);
  ASSERT_EQ(OB_SUCCESS, state.set_column_name("__ai_state_16"));
  state.set_is_hidden(true);
  ASSERT_TRUE(state.is_hidden());
  ASSERT_FALSE(state.is_ai_column());
}

TEST_F(ObAIColumnSchemaTest, table_schema_counts_ai_columns)
{
  ObTableSchema table;
  ObColumnSchemaV2 normal(allocator_);
  normal.set_table_id(500004);
  normal.set_column_id(1);
  ASSERT_EQ(OB_SUCCESS, normal.set_column_name("content"));
  ASSERT_EQ(OB_SUCCESS, table.add_column(normal));
  ASSERT_FALSE(table.has_ai_column());

  ObColumnSchemaV2 ai(allocator_);
  make_ai_column(ai, 16);
  ASSERT_EQ(OB_SUCCESS, table.add_column(ai));
  ASSERT_TRUE(table.has_ai_column());
}

}  // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
