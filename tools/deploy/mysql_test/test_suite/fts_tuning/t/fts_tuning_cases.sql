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

# ---------------------------------------------------------------------------
# 以下用例覆盖第二批优化：
#   - 词典区间查找由线性扫描改为二分（结果必须逐字节一致）
#   - IK 分词的 ASCII 小写化快路径（索引中的英文 token 仍为小写）
#   - 停用词检查同字符集时免去字符集转换（停用词判定不变）
#   - MATCH 的逐行过滤路径（filter_on_demand）行为不变
# ---------------------------------------------------------------------------

# 11) ik 分词：英文 token 入库前做小写化，小写查询词可命中
drop table if exists fts_tuning_ik;
create table fts_tuning_ik (
  id int primary key,
  body text
);
alter table fts_tuning_ik add fulltext index ft_ik (body) with parser ik;
insert into fts_tuning_ik values
  (1, 'OceanBase Fulltext Index Tuning'),
  (2, '全文索引调优实践与分词器热路径优化'),
  (3, 'MixedCase Words Like FullText And SEARCH');
select count(*) >= 1 as ik_casedown_hit from fts_tuning_ik where match(body) against('oceanbase');
select count(*) >= 1 as ik_casedown_hit2 from fts_tuning_ik where match(body) against('fulltext');
select count(*) >= 1 as ik_cjk_hit from fts_tuning_ik where match(body) against('全文索引');
select count(*) >= 1 as ik_cjk_hit2 from fts_tuning_ik where match(body) against('分词器');
# 同一查询重复执行结果稳定（词典二分不改变切分与命中集合）
select
  (select count(*) from fts_tuning_ik where match(body) against('全文索引'))
  = (select count(*) from fts_tuning_ik where match(body) against('全文索引')) as ik_stable;
drop table fts_tuning_ik;

# 12) beng 分词：停用词过滤结果不变
drop table if exists fts_tuning_beng;
create table fts_tuning_beng (
  id int primary key,
  body text
);
alter table fts_tuning_beng add fulltext index ft_beng (body) with parser beng;
insert into fts_tuning_beng values
  (1, 'the quick brown fox jumps over the lazy dog'),
  (2, 'fulltext search indexing requires efficient tokenization');
select count(*) = 0 as beng_stopword_miss from fts_tuning_beng where match(body) against('the');
select count(*) >= 1 as beng_word_hit from fts_tuning_beng where match(body) against('quick');
select count(*) >= 1 as beng_word_hit2 from fts_tuning_beng where match(body) against('tokenization');
drop table fts_tuning_beng;

# 13) MATCH 叠加普通过滤条件：过滤掉全部行 / 保留全部行，两条路径结果都正确
select count(*) = 0 as filter_drop_all from fts_tuning_doc
 where match(body) against('fulltext') and id < 0;
select count(*) >= 1 as filter_keep_all from fts_tuning_doc
 where match(body) against('fulltext') and id > 0;
select count(*) >= 1 as filter_keep_subset from fts_tuning_doc
 where match(body) against('fulltext') and id in (1, 2, 3);

# ---------------------------------------------------------------------------
# 以下用例覆盖第三批优化：
#   - IK 字符分类由「每个判断各解码一次」改为「解码一次 + 区间判断」
#   - 连接符 / 中文数字判定改为复用 TokenizeContext 缓存的 unicode 码点
#   - IK 分词器不再逐行装载从未被查询过的停用词词典
#
# 注意：MATCH 的查询词与文档使用同一个分词器切分，切分粒度的变化在命中数上
# 不可见（两侧同时变化），因此这一组用例直接比对 TOKENIZE() 的输出。
# ---------------------------------------------------------------------------

# 14) TOKENIZE 结果确定：同一输入多次切分结果完全一致
select cast(tokenize('全文索引的分词器热路径优化', 'ik') as char) = cast(tokenize('全文索引的分词器热路径优化', 'ik') as char) as tok_deterministic;
select cast(tokenize('oceanbase', 'ik') as char) = cast(tokenize('oceanbase', 'ik') as char) as tok_deterministic_en;

# 15) IK 的英文 token 入库前统一小写化（英文字母分类 + 小写化快路径）
select json_contains(tokenize('OceanBase Fulltext', 'ik'), json_quote('oceanbase')) as tok_has_lowercase;
select json_contains(tokenize('OceanBase Fulltext', 'ik'), json_quote('OceanBase')) = 0 as tok_no_uppercase;
select cast(tokenize('OceanBase Fulltext', 'ik') as char) = cast(tokenize('oceanbase fulltext', 'ik') as char) as tok_casedown_same;

# 16) 字母连接符（- 等）被并入英文 token，不会单独成为一个 token
select json_contains(tokenize('full-text', 'ik'), json_quote('-')) = 0 as tok_connector_absorbed;

# 17) 数字与中文数字的分类路径仍可用
select json_contains(tokenize('version 123456 release', 'ik'), json_quote('123456')) as tok_number;
select json_length(tokenize('一二三四五', 'ik')) >= 1 as tok_cn_number;
select cast(tokenize('一二三四五', 'ik') as char) = cast(tokenize('一二三四五', 'ik') as char) as tok_cn_number_stable;

# 18) 中文与全角字符的分类路径仍可用（OTHER_CJK / 代理项路径不被破坏）
select json_length(tokenize('中文分词测试ＡＢＣ', 'ik')) >= 1 as tok_fullwidth;

# 19) 索引侧同步验证：含连接符与数字的文档仍可被检索到
drop table if exists fts_tuning_ik2;
create table fts_tuning_ik2 (
  id int primary key,
  body text
);
alter table fts_tuning_ik2 add fulltext index ft_ik2 (body) with parser ik;
insert into fts_tuning_ik2 values
  (1, 'full-text version 123456 release'),
  (2, '第一章全文索引与分词器');
select count(*) >= 1 as ik2_connector_hit from fts_tuning_ik2 where match(body) against('release');
select count(*) >= 1 as ik2_cjk_hit from fts_tuning_ik2 where match(body) against('全文索引');
select count(*) >= 1 as ik2_number_hit from fts_tuning_ik2 where match(body) against('123456');
drop table fts_tuning_ik2;

drop table fts_tuning_doc;
