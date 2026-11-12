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

/**
 * @file test_file_sql_parser.cpp
 *
 * Unit tests of the SQL surface of the file table functions.
 *
 * The tests cover the read functions, the alias, the named parameters and
 * the glob function.  Every statement has to be accepted by the parser, the
 * semantic checks happen later during the resolution.
 */

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "lib/returncode/returncode.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/parser/sql_parser_base.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{
namespace
{

class TestFileSqlParser : public ::testing::Test
{
public:
  void SetUp() override {}
  void TearDown() override {}

protected:
  /// Parse a statement and report whether the parser accepted it.
  static bool accept(const char *sql)
  {
    ObArenaAllocator allocator;
    ObSQLMode sql_mode = 0;
    ObParser parser(allocator, sql_mode);
    ParseResult parse_result;
    common::ObString statement(sql);
    const int ret = parser.parse_sql(statement, parse_result, false);
    parser.free_result(parse_result);
    return OB_SUCCESS == ret;
  }

  /// Parse a statement and report the type of the node of the statement.
  static ObItemType statement_type(const char *sql)
  {
    ObArenaAllocator allocator;
    ObSQLMode sql_mode = 0;
    ObParser parser(allocator, sql_mode);
    ParseResult parse_result;
    common::ObString statement(sql);
    ObItemType type = T_INVALID;
    if (OB_SUCCESS == parser.parse_sql(statement, parse_result, false)
        && NULL != parse_result.result_tree_
        && NULL != parse_result.result_tree_->children_
        && 1 == parse_result.result_tree_->num_child_
        && NULL != parse_result.result_tree_->children_[0]) {
      type = parse_result.result_tree_->children_[0]->type_;
    }
    parser.free_result(parse_result);
    return type;
  }
};

TEST_F(TestFileSqlParser, read_csv)
{
  EXPECT_TRUE(accept("select * from read_csv('/tmp/data.csv')"));
  EXPECT_TRUE(accept("select id, name from read_csv('/tmp/data.csv') as f"));
  EXPECT_TRUE(accept("select * from read_csv('/tmp/data.csv') f"));
}

TEST_F(TestFileSqlParser, read_csv_with_options)
{
  EXPECT_TRUE(accept("select * from read_csv('/tmp/data.csv', header = true)"));
  EXPECT_TRUE(accept("select * from read_csv('/tmp/data.csv', sample_size = -1)"));
  EXPECT_TRUE(accept("select * from read_csv('/tmp/data.csv', header = true, sample_size = -1)"));
  EXPECT_TRUE(accept("select * from read_csv('/tmp/data.csv', 'csv')"));
}

TEST_F(TestFileSqlParser, read_json)
{
  EXPECT_TRUE(accept("select * from read_json('/tmp/data.jsonl')"));
  EXPECT_TRUE(accept("select * from read_json('/tmp/data.jsonl', format = 'newline_delimited')"));
  EXPECT_TRUE(accept("select * from read_ndjson('/tmp/data.jsonl')"));
}

TEST_F(TestFileSqlParser, read_parquet)
{
  EXPECT_TRUE(accept("select * from read_parquet('/tmp/data.parquet')"));
  EXPECT_TRUE(accept("select count(*) from read_parquet('/tmp/data.parquet') p"));
}

TEST_F(TestFileSqlParser, glob)
{
  EXPECT_TRUE(accept("select * from glob('/tmp/*.csv')"));
  EXPECT_TRUE(accept("select file from glob('/tmp/*') as g"));
}

TEST_F(TestFileSqlParser, file_table_in_a_join)
{
  EXPECT_TRUE(accept("select a.id, b.id from read_csv('/tmp/a.csv') a "
                     "inner join read_csv('/tmp/b.csv') b on a.id = b.id"));
  EXPECT_TRUE(accept("select a.id from read_csv('/tmp/a.csv') a "
                     "left join read_parquet('/tmp/b.parquet') b on a.id = b.id"));
}

TEST_F(TestFileSqlParser, ordinary_sql_is_not_affected)
{
  EXPECT_TRUE(accept("select * from t1"));
  EXPECT_TRUE(accept("select * from t1 t where t.c1 = 1 order by t.c1 limit 10"));
  EXPECT_TRUE(accept("select glob from t1"));
  EXPECT_TRUE(accept("select read_csv from t1"));
}

TEST_F(TestFileSqlParser, broken_statements_are_rejected)
{
  EXPECT_FALSE(accept("select * from read_csv("));
  EXPECT_FALSE(accept("select * from read_csv()"));
  EXPECT_FALSE(accept("select * from read_csv('/tmp/a.csv' read_csv('/tmp/b.csv')"));
}

/**
 * The DuckDB style DESCRIBE of a select statement.
 *
 * The statement describes the columns of the select statement, it is not the
 * explanation of its plan.
 */
TEST_F(TestFileSqlParser, describe_select)
{
  EXPECT_TRUE(accept("describe select * from read_csv('/tmp/data.csv')"));
  EXPECT_TRUE(accept("desc select * from read_csv('/tmp/data.csv')"));
  EXPECT_TRUE(accept("describe select id, name from read_csv('/tmp/data.csv') f"));
  EXPECT_TRUE(accept("describe select * from t1"));
  EXPECT_TRUE(accept("describe (select * from t1)"));
  EXPECT_TRUE(accept("describe with c as (select 1 from dual) select * from c"));
}

TEST_F(TestFileSqlParser, describe_select_has_its_own_node_type)
{
  EXPECT_EQ(T_DESCRIBE_SELECT_STMT, statement_type("describe select * from t1"));
  EXPECT_EQ(T_DESCRIBE_SELECT_STMT, statement_type("desc select * from t1"));
  EXPECT_EQ(T_DESCRIBE_SELECT_STMT, statement_type("DESCRIBE select 1 from dual"));
}

/**
 * The ordinary statements of the EXPLAIN family are not affected.
 */
TEST_F(TestFileSqlParser, explain_is_not_affected)
{
  EXPECT_EQ(T_EXPLAIN, statement_type("explain select * from t1"));
  EXPECT_EQ(T_EXPLAIN, statement_type("explain select * from read_csv('/tmp/a.csv')"));
  EXPECT_EQ(T_EXPLAIN, statement_type("explain delete from t1"));
  EXPECT_EQ(T_EXPLAIN, statement_type("explain insert into t1 values (1)"));
  EXPECT_EQ(T_EXPLAIN, statement_type("explain update t1 set c1 = 1"));
  // a describe of a statement that is not a select statement still explains it
  EXPECT_EQ(T_EXPLAIN, statement_type("describe delete from t1"));
  EXPECT_EQ(T_EXPLAIN, statement_type("describe insert into t1 values (1)"));
  EXPECT_EQ(T_EXPLAIN, statement_type("describe update t1 set c1 = 1"));
}

/**
 * The ordinary DESCRIBE of a relation is not affected, it still shows the
 * columns of the relation.
 */
TEST_F(TestFileSqlParser, describe_relation_is_not_affected)
{
  EXPECT_EQ(T_SHOW_COLUMNS, statement_type("describe t1"));
  EXPECT_EQ(T_SHOW_COLUMNS, statement_type("desc t1"));
  EXPECT_EQ(T_SHOW_COLUMNS, statement_type("describe db1.t1"));
  EXPECT_TRUE(accept("describe t1"));
  EXPECT_TRUE(accept("desc t1"));
}

} // namespace
} // namespace sql
} // namespace oceanbase
