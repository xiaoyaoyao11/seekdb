# MiniVectorDB（2026-competition-round2）

单机、单进程、单连接的轻量向量数据库，按 `STDIO_PROTOCOL.md` 的 JSONL 协议与
mysqltest 驱动交互。实现语言为 **C++17**，除标准库外无第三方依赖（不使用
SQLite/RocksDB/Faiss/HNSWlib 等现成存储或向量索引引擎）。

## 1. 构建、启动与数据目录

```bash
docker build -t minivectordb:latest .

mkdir -p "$(mktemp -d)/data"   # 或任意空目录
export VECTORDB_IMAGE='minivectordb:latest'
docker run --rm -i --name mvdb-local \
  --network=none --cpus=2 --memory=2g --memory-swap=2g --pids-limit=128 \
  --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --mount "type=bind,src=$PWD/data,dst=/data" \
  "$VECTORDB_IMAGE"
```

- 入口：`/app/minivectordb --data-dir /data`，全部参数写在 `ENTRYPOINT` 内，
  判题器即使覆盖 `CMD` 也不会丢掉 `--data-dir`。
- 单阶段 `ubuntu:24.04`：构建期 apt 安装 g++ 与 libstdc++/libgcc（带 3 次重试，
  规避包索引下载截断导致 `no image -> no admission`），产物动态链接，运行期不联网。
  与题包官方示例一致，也与 4 个公开通过的参赛提交同形态。
- 所有持久化文件只写 `/data`，当前实现只有一个文件 `/data/wal.log`。不启动端口监听，
  不申请 TTY，不向 stdout 写日志（日志走 stderr）。
- 启动时会主动输出一行 `{"type":"ready"}` 并 flush，之后才等待第一条请求。

## 2. 从空数据目录开始的完整示例

把下面每行作为一条 JSONL 请求写入容器 stdin（重启 = 关闭进程后用同一 `/data` 再启动）：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(64), embedding VECTOR(4));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'database','事务与恢复','[1,0,0,0]'),(2,'database','向量索引','[0,1,0,0]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0.9,0.1,0,0]') AS distance FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":6,"op":"execute","sql":"UPDATE docs SET embedding = '[0.8,0.2,0,0]' WHERE id = 1;","search_mode":"exact"}
{"id":7,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":8,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
{"id":9,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":10,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":11,"op":"execute","sql":"INSERT INTO docs VALUES (3,'travel','旅行攻略','[0,0,1,0]');","search_mode":"exact"}
{"id":12,"op":"execute","sql":"COMMIT;","search_mode":"exact"}
{"id":13,"op":"shutdown"}
```

`id` 原样回传、`ok` 为布尔；查询返回 `columns`/`rows`，DML 返回 `affected_rows`。
关闭后重新用同一目录启动，再发
`{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}`
应得到 `[[1],[2],[3]]`：`ROLLBACK` 的修改不生效，`COMMIT` 的修改保留。

题包自带的公开自测（20 个用例）入口：

```bash
export VECTORDB_IMAGE='minivectordb:latest'
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --handshake-only --output protocol-results/handshake
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output protocol-results/public
```

无 Docker 的开发机可用 `python3 tools/local_check.py`（等价的阻塞式驱动，复用官方
`_assertions.py` 与 `manifest.json`）和 `python3 tools/extra_check.py`（边界/崩溃/规模）。

## 3. SQL 支持范围与错误处理

| 类别 | 支持 |
| --- | --- |
| 表结构 | `CREATE TABLE`：BIGINT / VARCHAR(n) / VECTOR(n)，一个 BIGINT 主键，最多一个向量列；`CREATE VECTOR INDEX <name> ON <table>(<vector_col>)` |
| 数据操作 | `INSERT [INTO t (cols)] VALUES (...), (...)`、`SELECT`、`UPDATE ... SET ... [WHERE]`、`DELETE FROM ... [WHERE]` |
| 条件 | `= < <= > >= !=` 与 `AND`；普通列投影、`SELECT *` |
| 排序 | `ORDER BY <列/别名> [ASC|DESC]`、`LIMIT n`；并列项一律按主键升序；无 ORDER BY 时按主键升序输出 |
| 向量 | `L2_DISTANCE(col, '[...]') [AS alias]`；**先按 WHERE 过滤再算 TopK**；距离用存储后的 float32；并列按主键升序；结果不足 K 条返回全部 |
| 事务 | `BEGIN` / `COMMIT` / `ROLLBACK`；事务内查询与向量检索看到自己的修改；未显式 BEGIN 时每条 DML 自动提交 |
| 模式 | `search_mode` 的 `exact` 与 `ann`：两者都走精确扫描，ANN 召回率恒为 1.0，但结果与当前可见记录始终一致 |

不支持的语句（如 `ALTER TABLE`、`DROP TABLE`、聚合、JOIN、子查询、NULL 语义）返回
`ok:false` 且带非空 `error.code`，不会静默执行成别的含义。主要错误码：
`duplicate_key`、`dimension_mismatch`、`invalid_vector`、`vector_overflow`、
`value_too_long`、`type_mismatch`、`column_not_found`、`table_not_found`、
`table_exists`、`syntax_error`、`unsupported_sql`、`storage_error`。

约束：VARCHAR 长度按 Unicode 字符数、字符串比较按码点序（UTF-8 字节序等价）；
BIGINT 用 int64 原样输出；向量必须是有限 float32、维度严格匹配，拒绝 NaN/Infinity/溢出。

## 4. 简要设计与验证记录

**记录布局**：`/data/wal.log` 是唯一持久化文件，采用追加式事务日志。每条记录一行，
格式为 `<FNV-1a CRC16 进制>\t<JSON ops 数组>\n`。ops 有四种：
`0` 建表（列定义）、`1` 写入/覆盖一行、`2` 删除一行、`3` 建立向量索引标记。
表定义与数据都来自同一条日志流，避免目录/数据两个文件之间不一致。

**提交持久化**：每个事务（或自动提交的单条 DML）在返回成功前，先把完整记录
一次 `fwrite`，再 `fflush` + `fsync`，然后才把修改应用到内存状态。因此"已返回
COMMIT 成功"的事务必然已落盘；进程被强杀不会丢已确认的修改。

**恢复**：启动时顺序读取 WAL，逐行校验 CRC 与 JSON 完整性，只应用完整且校验通过的
记录；遇到撕裂尾部立即把文件截断到最后一个有效字节再追加。重放是幂等的，
在恢复途中再次被终止后重新重放结果不变。未提交事务从不写入 WAL，正常关闭
（`shutdown`）也会丢弃它，所以不会出现部分生效。

**事务与语句原子性**：显式事务用 overlay（`new_tables` + `changes` + `ops`）保存
未提交修改；查询按 `base rows → overlay` 合成可见集合，`view()` 直接返回行指针，
不对整表做拷贝。每条语句先在局部 `stmt_ops` 中完成全部校验（主键重复、维度、
长度、类型），校验通过才并入事务或提交；失败语句不留任何部分修改，
事务中此前成功的语句仍可继续提交或回滚。

**向量检索**：朴素精确扫描，`ann` 模式复用同一路径（召回率 1.0）。查询分两阶段：
先对过滤后的行只计算轻量排序键（距离/列值），排序并按 LIMIT 截断，再只对
TopK 行构造 JSON 投影，避免为全表分配结果对象。`CREATE VECTOR INDEX` 只记录
索引存在性；由于每次检索都基于当前可见记录，索引建立后的增删改自动反映到结果中，
不存在索引与事务状态不同步的问题。

**已实现**：基础 SQL、持久化、重启一致、事务、语句级原子性、崩溃恢复/重复恢复、
非法向量拒绝、Unicode 长度、BIGINT 精度、精确与 ANN TopK、索引维护。

**已知限制**：全量行数据常驻内存（主键→行 map），依靠 OS 而不是自建缓冲池；
10,000 条 × 128 维约 5 MB，规模继续增大时内存占用线性增长。ANN 未做近似结构，
用精确扫描换取 100% 召回与最简单的一致性保证。WAL 不做 checkpoint，长时间大量
写入后日志会持续增长。

**本地验证记录**（Windows g++ 15.1 静态编译，2026-11-17）：

| 项目 | 结果 |
| --- | --- |
| 启动握手 + shutdown（官方 `STDIO_PROTOCOL.md` 第 7 节等价） | PASS |
| 20 个公开 mysqltest 用例（95 条 SQL，含 `--restart` / `--mode ann`） | **20/20 PASS** |
| 自造边界 + 崩溃恢复（`tools/extra_check.py`） | **33/33 PASS** |
| 10,000 条 × 128 维批量写入 | 1.9~2.0 s（含驱动侧生成，本地有波动） |
| 200 次带过滤 TopK 查询 | 0.13~0.21 s（约 0.6~1.0 ms/次） |
| 50,000 条 × 128 维：写入 / 恢复 / WAL | 16.4 s / 3.0 s / 73 MiB |
| 硬杀进程后重启、撕裂 WAL 尾部、恢复途中再次终止 | 全部 PASS |

开发过程中发现并修复的真实缺陷（实验与复测记录见 `agent/conversation.json`）：
① 列名比较只把左侧转大写，导致所有列查找失败 → 公开用例由 0/20 变为 20/20；
② WAL 用 `%.9g` 把 `2.0` 写成 `2`，恢复时被解析成整数、向量全变 0 → 由 18/20
回到 20/20，修复方式是输出强制带小数点并在恢复端兼容两种 JSON 数值类型；
③ 每次提交深拷贝整张表来取列定义，写入退化成 O(N²) → 5 万条写入 28.8 s
降到 16.4 s，同时降低峰值内存；改为只拷贝列定义；
④ 首轮正式评测 0 分后补修：无 `AS` 别名的 `L2_DISTANCE(...)` 原先返回占位串
`L2_DISTANCE(v, ?)`，现按 MySQL 语义返回 SQL 原文中的表达式文本；
⑤ Dockerfile 由"多阶段 + 全静态链接 + 构建期自检"改为单阶段动态链接，
移除构建期自检（自检失败会直接毁掉整个镜像构建），apt 加 3 次重试，
参数全部内置于 `ENTRYPOINT`，并对齐题包官方示例与公开通过提交的形态。

**首轮评测复盘（如实记录）**：官方记录为 `taskStatus=2 / failMsg=0`、
`successCount=0 / failCount=1`，`message`/`performance`/`prompt` 三个字段被平台
字符混淆，无法反解出具体失败用例。这一形态与"编译失败"型记录
（`taskStatus=3 / failMsg=3 / testCaseResult=[]`）不同，据此判断镜像构建与协议
握手已成功、是 MySQL 正式用例未全部通过；但该推断未被直接证实。
因此修复同时覆盖两侧：源码侧补表达式列名，构建侧消除 3 个独有风险。

## 5. 最终验证清单（提交前逐项核对）

- [x] `2026-competition-round2` 分支存在，且为本次唯一提交
- [x] `src/minivectordb.cpp`、`Dockerfile`、`agent/conversation.json` 在**同一次 commit**
- [x] Commit SHA 为完整 40 位，提测表单与分支名一致
- [x] Dockerfile 从源码编译，不依赖本地预编译二进制；运行期禁止联网仍可启动
- [x] 启动主动输出 `{"type":"ready"}`；每条响应一行、带换行、立即 flush
- [x] `id` 原样回传、`ok` 为布尔、stdout 不含日志/空行/提示符
- [x] `shutdown` 返回 `{"id":N,"ok":true}` 后 0 退出且不再输出
- [x] 20 个公开用例全通过；额外边界与崩溃恢复用例全通过
- [x] 提交持久化先 `fsync` 后响应；撕裂尾部可截断；重复恢复幂等
- [x] `conversation.json` 为 UTF-8 合法 JSON、顶层对象、≤128 KiB、无重复键/NaN/令牌
- [x] 仓库中不含访问密钥、令牌或其他敏感信息
- [x] 未改动初赛其它赛题的分支与配置

## 6. 提交前检查项（防错）

1. 确认 `git rev-parse HEAD` 输出与提测表单里的 40 位 SHA 完全一致（勿用短 SHA）。
2. 确认表单分支填 `2026-competition-round2`，仓库 URL 为平台可获取的 HTTPS 地址。
3. 确认 `agent/conversation.json` 已纳入该 commit，而不是本地未提交文件。
4. 确认 Dockerfile 的 `COPY src/minivectordb.cpp` 与仓库实际路径一致。
5. 确认 `--read-only` 容器下只写 `/data`，启动不依赖 `/tmp` 或当前工作目录写权限。
6. 确认 `protocol_check` 的 `--restart` 复用同一数据目录，未提交事务在关闭时回滚。
7. 确认本题**只提测一次**；提交前不再修改分支内容，避免锁定版本与实际不一致。
