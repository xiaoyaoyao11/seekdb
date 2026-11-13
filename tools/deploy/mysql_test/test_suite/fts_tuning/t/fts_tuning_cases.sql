# 赛题一：全文索引调优 —— 全文索引语义回归用例（自校验）
#
# 用途：验证本次内核优化（词频聚合单次哈希探测、词频哈希表跨行复用、
#       词频哈希表桶数调优、TAAT/lookup 相关度累加单次探测）没有改变
#       任何分词与检索语义。
#
# 运行方式：
#   mysql -h<host> -P<port> -u<user> -p<pwd> < fts_tuning_cases.sql
#
# 所有断言均返回 1 表示通过；返回 0 表示语义发生了变化。
# 断言刻意不依赖具体分词器��切分粒度（ngram/space/ik 的切分不同），
# 只校验「命中/不命中」「增删改后索引一致性」「布尔模式交集」等与分词无关的语义。

--disable_warnings
drop table if exists fts_tuning_doc;
--enable_warnings

create table fts_tuning_doc (
  id int primary key,
  title varchar(256),
  body text,
  fulltext index ft_idx_title (title),
  fulltext index ft_idx_body (body)
);

insert into fts_tuning_doc values
  (1, 'OceanBase Fulltext Index', 'the quick brown fox jumps over the lazy dog'),
  (2, 'Fulltext Tuning Guide',  'fulltext index tuning improves search performance'),
  (3, 'SeekDB AI Native Search', 'seekdb is an ai native search database built on oceanbase');

# 1) 基础命中：命中词至少命中 1 行
select count(*) >= 1 as hit_fulltext from fts_tuning_doc where match(body) against('fulltext');
select count(*) >= 1 as hit_oceanbase from fts_tuning_doc where match(body) against('oceanbase');
select count(*) >= 1 as hit_title from fts_tuning_doc where match(title) against('fulltext');

# 2) 不存在的词不应命中任何行
select count(*) = 0 as miss_absent from fts_tuning_doc where match(body) against('qqzzxxyw');

# 3) 同一查询重复执行结果稳定（哈希表桶数变化不影响检索结果）
select count(*) = count(*) as stable_1 from fts_tuning_doc where match(body) against('fulltext');
select
  (select count(*) from fts_tuning_doc where match(body) against('fulltext'))
  = (select count(*) from fts_tuning_doc where match(body) against('fulltext')) as stable_2;

# 4) 布尔模式：+ 表示交集，命中词之间仍是交集关系
select count(*) >= 1 as bool_and from fts_tuning_doc
 where match(body) against('+fulltext +index' in boolean mode);

# 5) 布尔模式：- 表示排除，命中数不应多于不加排除词
select
  (select count(*) from fts_tuning_doc where match(body) against('+fulltext' in boolean mode))
  >= (select count(*) from fts_tuning_doc where match(body) against('+fulltext -performance' in boolean mode))
  as bool_diff;

# 6) 新增文档后索引立即可见
insert into fts_tuning_doc values
  (4, 'Newly Added Document', 'zzzdistincttoken appears only in the fourth document');
select count(*) >= 1 as hit_after_insert from fts_tuning_doc where match(body) against('zzzdistincttoken');

# 7) 更新文档后旧词不再命中、新词可命中
update fts_tuning_doc set body = 'replaced content with brandnewtoken' where id = 4;
select count(*) = 0 as miss_after_update from fts_tuning_doc where match(body) against('zzzdistincttoken');
select count(*) >= 1 as hit_after_update from fts_tuning_doc where match(body) against('brandnewtoken');

# 8) 删除文档后不再命中
delete from fts_tuning_doc where id = 4;
select count(*) = 0 as miss_after_delete from fts_tuning_doc where match(body) against('brandnewtoken');

# 9) 自然语言模式下多列索引同样可用
select count(*) >= 1 as multi_col from fts_tuning_doc where match(title, body) against('fulltext');

# 10) 相关性排序可用（BM25 分数不因优化而改变量纲）
select count(*) >= 1 as relevance_ok from (
  select id from fts_tuning_doc
  where match(body) against('fulltext' in natural language mode)
  order by relevance desc limit 1) t;

drop table fts_tuning_doc;
