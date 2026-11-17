# 赛题二：文件即数据库（本地验证记录）

## 原生执行路径

`read_csv/read_json/read_ndjson/read_parquet/glob` 经原生解析器、Resolver、FUNCTION_TABLE 逻辑计划和物理扫描执行；过滤、表达式、聚合、排序、LIMIT、INNER/LEFT JOIN 使用 seekdb 算子。没有使用外部查询引擎代执行，也不创建业务数据表或把文件全部装入内存。

CSV/JSONL 采用完整流式扫描推断 Schema，`sample_size` 仅支持 `-1`。所有文件表函数（包括 `glob`）在推断、查询权限收集、执行打开三个阶段使用原生 FILE 权限校验；只有开关和安全目录并不足以授权读取。受限账号测试覆盖读取、DESCRIBE/EXPLAIN、COPY、授权与撤权，以及不存在路径须先拒绝权限而不泄露推断错误。计划中仅保留 Schema、选项和文件指纹，执行时重新打开并校验；文件查询不进入普通计划缓存。自动参数化后的字符串/数字仅从 ParamStore 绑定，不执行任意表达式。

`DESCRIBE SELECT` 只解析子查询并读取其列元数据，生成 Schema 大小的原生常量关系，不执行子查询扫描；普通 `EXPLAIN SELECT` 保持执行计划行为。

## COPY 导出

支持 `COPY (SELECT ...) TO 'path' (...)` 和 `COPY table TO 'path' (...)`。查询仍由 seekdb 执行，原生 SELECT INTO 算子逐行/批处理输出 CSV。

- 选项：FORMAT CSV、HEADER、DELIMITER/DELIM、QUOTE、ESCAPE、NULLSTR。
- 使用目标目录内的匿名临时 inode，只有成功完成后才通过保持打开的文件描述符发布目标，原子操作禁止覆盖已有目标。没有可替换、碰撞或被误删的临时文件名。
- 目标已存在则失败；查询异常时清理临时文件，不留下半成品目标。
- CSV 区分引号内的空字符串/NULL 文本与未加引号的 SQL NULL，保证导出读回不丢值。
- 开关和 secure_file_priv 同时约束读取和 COPY；不追踪路径中的符号链接。

## 本地测试结果

完整修改后的服务器编译成功，并实际运行：

| 层级 | 结果 |
| --- | --- |
| 统一读取器原生检查 | 203 项检查通过 |
| Parquet 独立样本/投影测试 | 23 次通过 |
| 原生解析器测试 | 23 次通过 |
| 文件 SQL 集成 | 20 个测试通过 |
| COPY 原生资源与发布回归 | 8 项通过（移除全部 Linux capabilities 后运行） |
| 公开 file_sql_basic SQL 与原始预期结果 | 通过 |
| 第一题 AI SQL 回归 | 60 个测试通过 |
| 第一题两个公开 mysqltest | 通过 |

公开 File SQL 测试的 SQL 和 `.result` 文件直接取自 Git HEAD，未改断言和预期结果。官方 include 使用 SSH/SCP 和 admin TCP 连接部署测试数据；本地 runner **只将这两个部署/清理 include 适配为本地传输**，仍使用原始数据和真实 mysqltest。这是本地验证，不是正式平台得分。

Parquet 测试中的 PyArrow 仅用于生成独立测试文件和期望值；生产代码不依赖 PyArrow，也不调用它执行查询。

## 执行测试

在与服务器相同的 Linux 网络和文件命名空间中，连接一个专用、socket-only 的测试实例，安装测试所需 PyMySQL 后：

```sh
export SEEKDB_TEST_SOCKET=/private/test-instance/run/sql.sock
python tests/competition/test_file_sql_integration.py -v
bash tests/competition/run_file_reader_native.sh /path/to/canonical/seekdb
bash tests/competition/run_file_copy_native.sh /path/to/canonical/seekdb
python tests/competition/run_file_parquet_native.py
python tests/competition/run_file_sql_public_mysqltest.py \
  --source-root /path/to/canonical/seekdb \
  --socket "$SEEKDB_TEST_SOCKET" \
  --mysqltest /path/to/seekdb/deps/3rd/u01/obclient/bin/mysqltest \
  --output-dir /private/new-test-results
```

集成测试会短暂修改专用实例的全局 secure_file_priv，并在结束时恢复；不要对共享或生产数据库运行。公开测试 runner 的输出目录必须尚不存在。

## 范围与限制

- 单进程本地文件，不提供远程文件、分布式扫描或多文件自动合并。
- COPY 的安全发布要求 Linux、支持 `O_TMPFILE` 的目标文件系统，以及真实 procfs 的 `/proc/self/fd`；环境不支持时明确失败，不降级到可被替换的临时路径。
- CSV/JSONL 全量推断；JSONL 仅扁平对象，拒绝嵌套数组/对象。
- Parquet 支持扁平基础类型、DATE/TIMESTAMP、PLAIN/字典/RLE、Data Page V1/V2、未压缩和 Snappy；其他编码、复杂类型、加密文件明确拒绝。
- 单条记录、Schema、列数量、Parquet 页和 COPY 输出均有资源限制。
- 尚不宣称隐藏测试全部通过或获得满分。提交/推送与平台提测仍需单独授权。
