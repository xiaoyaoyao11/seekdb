# 赛题一：全文索引调优 —— 全文索引基准测试（统一口径）
#
# 覆盖赛题要求的三个维度：索引构建、分词（写入）、查询。
# 每个阶段是一条独立语句，mysql 客户端会在每条语句后输出耗时（Query OK ... (X sec)），
# 便于在「基线版本」与「优化版本」之间逐项对比。
#
# 运行方式：
#   bash run_fts_tuning_bench.sh <host> <port> <user> <password>
#
# 可调参数（通过环境变量）：
#   BENCH_ROWS      语料行数，默认 50000
#   BENCH_QUERY_REPEAT  单条查询重复次数，默认 10

--disable_warnings
drop table if exists fts_bench_doc;
drop table if exists fts_bench_source;
--enable_warnings

# 1) 造语料：重复的英文句子，词分布贴近真实文档
create table fts_bench_source (id int primary key, body text);

insert into fts_bench_source (id, body)
with recursive seq(n) as (
  select 1
  union all
  select n + 1 from seq where n < 50000
)
select n,
       concat('document ', n, ' about fulltext index tuning in oceanbase seekdb. ',
              'the quick brown fox jumps over the lazy dog while searching documents. ',
              'tokenization posting list merge bm25 relevance ranking are discussed. ',
              'sedentary riverside modification retelling intangible hydrographic administratively')
from seq;

# 2) 写入路径（含分词）：向带全文索引的表批量写入
create table fts_bench_doc (
  id int primary key,
  body text,
  fulltext index ft_idx_bench_body (body)
);

insert into fts_bench_doc (id, body) select id, body from fts_bench_source;

# 3) 索引构建路径：在已有数据的表上建全文索引
create table fts_bench_build (
  id int primary key,
  body text
);
insert into fts_bench_build (id, body) select id, body from fts_bench_source;
create fulltext index ft_idx_bench_build_body on fts_bench_build (body);

# 4) 查询路径
# 4.1 高频词单条件
select count(*) from fts_bench_doc where match(body) against('document');
select count(*) from fts_bench_doc where match(body) against('fulltext');
# 4.2 低频词单条件
select count(*) from fts_bench_doc where match(body) against('hydrographic');
# 4.3 多词自然语言模式
select count(*) from fts_bench_doc where match(body) against('tokenization posting merge');
# 4.4 布尔模式（交集 + 排除）
select count(*) from fts_bench_doc where match(body) against('+fulltext -hydrographic' in boolean mode);
# 4.5 布尔模式（多 token 嵌套）
select count(*) from fts_bench_doc where match(body) against('+document (+fulltext -hydrographic)' in boolean mode);
# 4.6 大结果集 + LIMIT
select count(*) from (select id from fts_bench_doc where match(body) against('document') limit 10) t;
# 4.7 写入路径：更新（触发索引维护）
update fts_bench_doc set body = concat(body, ' extra token ', id) where id <= 5000;

# 5) 清理
drop table fts_bench_doc;
drop table fts_bench_build;
drop table fts_bench_source;
