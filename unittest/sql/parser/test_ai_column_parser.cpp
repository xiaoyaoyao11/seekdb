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

// Parser coverage for the asynchronous AI column syntax:
//   * CREATE TABLE  ... (<column> <type> AI COLUMN (<ai_expr>) WITH (...))
//   * ALTER TABLE ADD COLUMN ... AI COLUMN (...)
//   * SHOW AI COLUMNS FROM <table>
//
// The AI column attribute is represented by T_AI_COLUMN_ATTR; options are a
// linked list of T_AI_COLUMN_OPTION nodes.

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "oblib/common/sql_mode/ob_sql_mode.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "objit/common/ob_item_type.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

class ObAIColumnParserTest : public ::testing::Test
{
public:
  ObAIColumnParserTest() : allocator_(ObModIds::OB_SQL_PARSER) {}
  virtual ~ObAIColumnParserTest() {}

  void parse(const char *sql, ParseResult &result)
  {
    ObParser parser(allocator_, DEFAULT_MYSQL_MODE);
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(sql), result));
    ASSERT_TRUE(NULL != result.result_tree_);
    ASSERT_TRUE(result.result_tree_->children_ != NULL);
  }

  void find_ai_column_attr(ParseNode *node, ParseNode *&attr, int64_t &option_cnt)
  {
    if (NULL == node) {
      return;
    }
    if (T_AI_COLUMN_ATTR == node->type_) {
      attr = node;
      option_cnt = 0;
      if (node->num_child_ >= 2 && NULL != node->children_[1]) {
        ParseNode *opt = node->children_[1];
        while (NULL != opt) {
          if (T_LINK_NODE == opt->type_) {
            ++option_cnt;
            opt = opt->children_[0];
          } else {
            ++option_cnt;
            opt = NULL;
          }
        }
      }
      return;
    }
    for (int32_t i = 0; NULL == attr && i < node->num_child_; ++i) {
      find_ai_column_attr(node->children_[i], attr, option_cnt);
    }
  }

  ObArenaAllocator allocator_;
};

// CREATE TABLE with one completion AI column and one embedding AI column.
TEST_F(ObAIColumnParserTest, create_table_with_ai_columns)
{
  ParseResult result;
  const char *sql =
      "create table docs("
      "  id bigint primary key,"
      "  content varchar(200),"
      "  tags json AI COLUMN ("
      "    AI_COMPLETE('doc_complete', concat('Classify: ', content))"
      "  ) WITH (sync_mode='async', max_retries=2),"
      "  emb vector(4) AI COLUMN ("
      "    AI_EMBED('doc_embed', content, 4)"
      "  ) WITH (sync_mode='async', max_retries=3)"
      ")";
  parse(sql, result);
  ParseNode *attr = NULL;
  int64_t option_cnt = 0;
  find_ai_column_attr(result.result_tree_->children_[0], attr, option_cnt);
  ASSERT_TRUE(NULL != attr);
  ASSERT_EQ(2, attr->num_child_);
  // The first AI COLUMN attribute carries the expression text.
  ASSERT_TRUE(NULL != attr->children_[0]);
  ASSERT_GT(attr->children_[0]->str_len_, 0);
  // sync_mode + max_retries
  ASSERT_EQ(2, option_cnt);
}

// ALTER TABLE ADD COLUMN shares the same attribute node.
TEST_F(ObAIColumnParserTest, alter_table_add_ai_column)
{
  ParseResult result;
  const char *sql =
      "alter table docs add column summary varchar(400) AI COLUMN ("
      "  AI_COMPLETE('doc_complete', content)"
      ") WITH (sync_mode='async', max_retries=1)";
  parse(sql, result);
  ParseNode *attr = NULL;
  int64_t option_cnt = 0;
  find_ai_column_attr(result.result_tree_->children_[0], attr, option_cnt);
  ASSERT_TRUE(NULL != attr);
  ASSERT_EQ(1, option_cnt);
}

// Invalid option names and non-async sync modes are rejected by the resolver,
// but must still parse into T_AI_COLUMN_OPTION nodes so that a precise error
// can be reported.
TEST_F(ObAIColumnParserTest, ai_column_options_are_parsed_verbatim)
{
  ParseResult result;
  const char *sql =
      "create table t(id int, tags json AI COLUMN (AI_COMPLETE('m', id))"
      " WITH (sync_mode='sync', unknown_option=1))";
  parse(sql, result);
  ParseNode *attr = NULL;
  int64_t option_cnt = 0;
  find_ai_column_attr(result.result_tree_->children_[0], attr, option_cnt);
  ASSERT_TRUE(NULL != attr);
  ASSERT_EQ(2, option_cnt);
}

// SHOW AI COLUMNS FROM t must be recognised by the SHOW dispatcher.
// Disabled until the T_SHOW_AI_COLUMNS node is wired into the SHOW grammar; it
// documents the expected contract and can be enabled without further edits.
TEST_F(ObAIColumnParserTest, DISABLED_show_ai_columns)
{
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS,
            ObParser(allocator_, DEFAULT_MYSQL_MODE).parse(
                ObString::make_string("show ai columns from docs"), result));
  ASSERT_TRUE(NULL != result.result_tree_);
  ASSERT_TRUE(NULL != result.result_tree_->children_[0]);
  // Expect a SHOW statement whose table item is `docs`.
  ASSERT_EQ(T_SHOW_COLUMNS, result.result_tree_->children_[0]->type_);
}

// A plain column without the AI COLUMN attribute must not produce the node.
TEST_F(ObAIColumnParserTest, plain_column_has_no_ai_attribute)
{
  ParseResult result;
  parse("create table t(id bigint primary key, tags json)", result);
  ParseNode *attr = NULL;
  int64_t option_cnt = 0;
  find_ai_column_attr(result.result_tree_->children_[0], attr, option_cnt);
  ASSERT_TRUE(NULL == attr);
}

}  // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
