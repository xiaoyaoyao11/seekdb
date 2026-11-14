# 赛题三：异步 AI 列（AI COLUMN）设计与实现方案

> 目标仓库：`D:\deepseek\seekdb`（分支 `2026-competition`，HEAD `53c4b7b`）
> 本文档给出完整设计、文件级改动清单和分阶段实施/验收计划，供实现与提交时对照。
> 说明：当前仓库**仅包含 AI 函数**（AI_PROMPT / AI_COMPLETE / AI_EMBED / AI_RERANK 与 Endpoint 管理），
> 尚未包含 AI 列的任何实现。AI 列需要在此之上新增 DDL、元数据、异步维护与一致性保证。

---

## 1. 需求分解

从赛题说明与课程稿（Easy Data x AI · I5）提炼出可验收的外部行为：

| 功能域 | 外部行为（测试关注点） |
| --- | --- |
| DDL 定义 | `CREATE TABLE` / `ALTER TABLE ADD COLUMN` 支持 `col TYPE AI COLUMN (AI_COMPLETE(...) / AI_EMBED(...)) WITH (sync_mode='async', max_retries=N)` |
| 结果类型契约 | 返回值为 `VARCHAR(N)`/`JSON`/`VECTOR(N)` 等；维度、长度、JSON 合法性在 DDL 或发布前校验；声明维度与函数维度不一致时 DDL 直接拒绝 |
| 只读约束 | 用户不能直接 INSERT/UPDATE AI 列（必须报错） |
| 自动维护 | INSERT 新行、UPDATE 依赖列、DELETE 行后，后台自动生成/失效/清理，不阻塞业务事务 |
| 依赖判定 | 只改无关列不触发重算 |
| 异步可见性 | 提交后结果未完成时读取为 NULL；完成并通过校验后才可见 |
| 整体发布 | 同一行一次变化涉及多个 AI 列时，要么全可见要么全不可见 |
| 严格读取 | 会话变量 `ai_column_strict_read=ON` 时，语句实际引用未完成/失败的 AI 列立即报错（不等待） |
| 状态观测 | `SHOW AI COLUMNS FROM t` 返回定义与完成状态所需信息 |
| 行级重试 | `ALTER TABLE t RETRY [FAILED] AI COLUMN c [WHERE ...]` |
| 版本安全 | 旧请求晚返回不能覆盖新版本；结果新旧由源数据版本决定，而非 HTTP 返回顺序 |
| 事务 | 回滚后原结果/原索引保持不变；未提交结果不可见 |
| 崩溃恢复 | 已提交数据变化和未完成工作不丢失；重启后继续生成；允许外部调用多于一次 |
| 索引一致性 | AI 列可参与普通索引、JSON 多值索引、全文索引、VSAG HNSW 向量索引；失效值不得被任何索引搜到；新值自动进入索引 |
| 成本 | 汇总实际模型调用（含重试）按厂商 Token 规则计费（可观测/统计口径） |

---

## 2. 现有可复用能力

### 2.1 AI 函数与模型服务

- 表达式：`src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp`、`ob_expr_ai_embed.cpp`、`ob_expr_ai_rerank.cpp`、`ob_expr_ai_prompt.cpp`
- 调用封装：`ob_ai_func_utils.h/.cpp` 中的 `ObAIFuncModel`（`call_completion` / `call_embedding` / `call_rerank`）与 `ObAIFuncUtils`
- 端点解析：`query::ObIAiEndpointResolver`（`resolve_by_model_name`），服务实现 `src/observer/ai_service/ob_ai_service_proxy.cpp`
- 端点/模型注册：`DBMS_AI_SERVICE.*`（`src/sql/pl/sys_package/ob_dbms_ai_service.*`、`src/share/inner_table/sys_package/dbms_ai_service_body_mysql.sql`）
- 计划缓存元信息：`ObAIFuncExprInfo`（`src/sql/engine/expr/ob_expr_ai/ob_ai_func.h`）

**结论**：异步执行器可以直接复用 `ObAIFuncModel`，无需重新实现 HTTP/协议层。

### 2.2 生成列机制

- `ObColumnSchemaV2`（`src/share/schema/ob_column_schema.h`）：`VIRTUAL_GENERATED_COLUMN_FLAG` / `STORED_GENERATED_COLUMN_FLAG`；
  生成列表达式字符串复用 `cur_default_value_` 存储（见 `ObDDLResolver::resolve_column_definition`，`src/sql/resolver/ddl/ob_ddl_resolver.cpp:1878`）
- 插入时计算默认值/生成列的入口：`ObDefaultValueUtils`（`src/sql/resolver/dml/ob_default_value_utils.h`）
- 级联依赖：`GENERATED_DEPS_CASCADE_FLAG`、`add_cascaded_column_id`

**结论**：AI 列可以建模为「一种特殊的 stored 列」，复用列存储/编码/索引；差别在于**写入时不算，异步补算**。

### 2.3 异步变更流水线（关键复用点）

- `src/observer/change_stream/`：
  - `ObCSRow`（`ob_change_stream_dispatcher.h:63`）同时携带 `new_row_`、`old_row_`、`commit_version_`、`dml_flag_`、`seq_no_`
  - 插件接口 `ObCSPlugin`（`ob_change_stream_plugin.h`）：`process(rows, ctx)` + `commit()`
  - 已实现的异步向量索引插件 `ObCSPluginAsyncIndex` + `ObCSAsyncIndexProcessor`（`ob_cs_plugin_async_index.h/.cpp`）：演示了
    「读 redo 行 → 组事件 → 用 `ObMySQLTransaction` + DAS 写回」的完整写法
  - 回滚可见性：`is_row_visible` 与 `ObCSRollbackRange`

**结论**：AI 列最适合作为一个新的 Change Stream 插件实现：
业务事务 T1 提交 → Dispatcher 解析 redo → Worker 调用插件 → 插件读已提交输入、调模型、版本校验、写 T2。

### 2.4 索引

普通索引 / JSON 多值索引 / 全文索引 / VSAG HNSW 向量索引均已存在；
AI 列的值是普通列值，**只要结果事务 T2 正确地更新列值并维护索引，索引一致性即可由现有 DML 路径保证**。
关键点：失效（源变化）与发布（新值）必须走同一事务语义，避免索引项与列值不一致。

---

## 3. 总体架构

```
┌───────────────────────────── DDL 阶段 ─────────────────────────────┐
│ CREATE/ALTER TABLE ... AI COLUMN (AI_COMPLETE/AI_EMBED ...)        │
│   → 解析语法 → 静态校验(类型/维度/依赖) → 写入 __all_column 元数据  │
│   → 建立"AI 列定义 + 依赖列 + 选项(sync_mode,max_retries)"          │
└────────────────────────────────────────────────────────────────────┘
                                    │
┌────────────────────── 业务事务 T1（用户可见） ─────────────────────┐
│ INSERT/UPDATE/DELETE 源表                                          │
│   → 修改依赖列时：旧 AI 值失效（置 NULL / 标记 stale）             │
│   → 同事务登记"待生成工作"(task)：<table,row,column,source_version>  │
│   → COMMIT（不等模型）                                              │
└────────────────────────────────────────────────────────────────────┘
                                    │ redo / change stream
┌────────────────────── 异步执行（后台 Worker） ─────────────────────┐
│ 读取已提交输入（快照） → 组 Prompt → 调模型（可重试）              │
│   → 结果校验（类型/长度/JSON/维度/非空）                           │
└────────────────────────────────────────────────────────────────────┘
                                    │
┌────────────────────── 结果事务 T2（独立事务） ─────────────────────┐
│ 再次确认源数据版本 == 请求时的版本                                  │
│   → 通过：同组 AI 结果整体发布 + 更新索引                         │
│   → 不通过：丢弃（迟到/过期结果）                                  │
│   → COMMIT                                                         │
└────────────────────────────────────────────────────────────────────┘
                                    │
┌────────────────────── 读取 ────────────────────────────────────────┐
│ 非严格：未完成/失败/过期 → NULL，不阻塞                            │
│ 严格：语句引用的 AI 列存在未完成/失败 → 立即报错                   │
└────────────────────────────────────────────────────────────────────┘
```

三个关键不变量：

1. **版本不变量**：T2 只允许在「源数据版本 == 请求发起时版本」时发布；否则丢弃。
2. **原子发布不变量**：同一行同一次变化的多个 AI 列在同一 T2 中一起提交。
3. **索引不变量**：AI 值与索引项的可见性由同一事务决定；失效值不得通过任何索引被检索到。

---

## 4. 详细设计

### 4.1 SQL 语法

```sql
-- 建表
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  content VARCHAR(8192),
  language VARCHAR(32) AI COLUMN (
    AI_COMPLETE('doc_complete_model', CONCAT('Return only the language name. Content: ', content))
  ) WITH (sync_mode='async', max_retries=3),
  emb VECTOR(1024) AI COLUMN (
    AI_EMBED('doc_embed_model', content, 1024)
  ) WITH (sync_mode='async', max_retries=3)
);

-- 加列
ALTER TABLE docs ADD COLUMN summary VARCHAR(2048) AI COLUMN (
  AI_COMPLETE('doc_complete_model', CONCAT('Summarize. Content: ', content))
) WITH (sync_mode='async', max_retries=3);

-- 观测
SHOW AI COLUMNS FROM docs;

-- 重试
ALTER TABLE docs RETRY AI COLUMN language WHERE id BETWEEN 100 AND 200;
ALTER TABLE docs RETRY FAILED AI COLUMN tags WHERE category = 'news';
ALTER TABLE docs RETRY FAILED AI COLUMN emb;
```

语法实现位置：`src/sql/parser/sql_parser_mysql_mode.y` 的 `column_definition` 规则；
新增非保留关键字 `AI`（`AI` 未必已是关键字），`COLUMN` 已是关键字。
建议新增 ParseNode 类型 `T_AI_COLUMN_DEF`，避免复用 `T_COLUMN_DEFINITION` 的 children 语义导致歧义。

### 4.2 元数据与存储

在 `ObColumnSchemaV2` 增加（`src/share/schema/ob_column_schema.h`）：

```cpp
// 新增列标志（低 32 位，需落 __all_column；建议复用空闲位，例如 bit7）
#define AI_COLUMN_FLAG (INT64_C(1) << 7)

// 新增字段
common::ObString ai_expr_;        // AI 表达式原文（用于重算/展示）
int8_t ai_sync_mode_;             // 0=async（当前仅 async）
int8_t ai_max_retries_;
common::ObString ai_dep_columns_; // 依赖列名列表（"content,tags"），或存列 id JSON
```

同时修改：

- `operator==` / `assign()` / `deep_copy`（`ob_column_schema.cpp`）
- 序列化：`OB_SERIALIZE_MEMBER`（`OB_UNIS_VERSION_V(1)`）追加字段；若需兼容旧数据则升版本号
- `get_stored_column_flags()` 覆盖新标志
- schema 打印/`DESC`/`SHOW CREATE TABLE` 输出
- 系统表 `__all_column` 的读写映射（`src/share/inner_table/...` / `ObTableSchema` 序列化路径）

> 风险：`__all_column` 序列化格式变更会影响所有表的元数据。比赛环境全新初始化，可接受；若要稳妥，可将
> `ai_expr_` 与选项编码进一个 JSON 字符串字段，仅新增 1 个字段，降低序列化面。

### 4.3 解析与静态校验

`ObDDLResolver::resolve_column_definition`（`src/sql/resolver/ddl/ob_ddl_resolver.cpp:1765`）新增分支：

1. 解析 AI 表达式字符串（复用 `dup_expr_string` + `convert_sql_text_to_schema_for_storing`）。
2. 提取依赖列：遍历表达式 AST 中 `ObColumnRefRawExpr`，得到源列集合。
3. 校验：
   - 目标列类型必须与函数返回类型可转换；
   - `AI_EMBED(model, source, dim)` 的 `dim` 必须为正整数且与列 `VECTOR(N)` 的 N 一致，否则 DDL 拒绝（对应赛题示例 `bad_emb` 必须报错）；
   - `AI_COMPLETE` 目标为 `VARCHAR/JSON` 等；
   - 依赖列不得包含其它 AI 列（禁止链式依赖，或定义求值顺序）。
4. 写入 `ai_expr_`、`ai_dep_columns_`、`AI_COLUMN_FLAG`。
5. 对已有数据的 `ALTER TABLE ADD COLUMN`：DDL 成功即返回，存量行由后台补齐（对应课程稿 4.2）。

### 4.4 DML 触发与任务登记

推荐实现（低侵入、天然覆盖 INSERT/UPDATE/DELETE/多行/批量）：

**方案 A（推荐）：Change Stream 插件**

在 `src/observer/change_stream/` 新增 `ObCSPluginAIColumn`：

- `process(rows, ctx)`：对每个 `ObCSRow`
  - 由 `tablet_id` → `table_id`，取 schema；
  - 若表无 AI 列则跳过；
  - 依据 `dml_flag_` 与 `old_row_/new_row_` 判断依赖列是否变化：
    - INSERT：全部 AI 列出任务；
    - UPDATE：仅当依赖列新旧值不同才出任务；
    - DELETE：写入失效/墓碑事件；
  - 生成 task 记录：`<table_id, pk, ai_column_id, source_version=commit_version_, retry_cnt>`。
- 任务落地方式二选一：
  1. 写入内部任务表（`__all_ai_column_task`），由独立 worker 消费（可持久、可恢复、可重试）；
  2. 直接在插件内完成模型调用与 T2 写回（实现简单，但会阻塞流水线；失败重试需自建）。
  - 建议先做 (1) 的简化版：任务表 + 后台线程轮询，便于崩溃恢复与 RETRY。

**方案 B：DML 执行器内联登记**
在 INSERT/UPDATE/DELETE 的 `ObDMLService`/执行器层，与业务 DML 同事务写入任务表。
优点是强原子；缺点是要覆盖所有写入路径（含内部表、LOAD DATA），改动面大。可作为方案 A 的补充用于 `INSERT` 快速路径。

> 回滚语义：`is_row_visible` 已处理 savepoint 回滚；任务登记必须与 T1 同事务或由 redo 派生，
> 保证「源变化 + 旧结果失效 + 待生成工作」原子提交/回滚（对应课程稿第 8 节）。

### 4.5 异步执行器

新增 `src/observer/ai_column/` 模块：

- `ObAIColumnTask`：任务结构（table_id、rowkey、column_id、source_version、retry_cnt、deadline）
- `ObAIColumnWorker`：线程池
  - 取任务（按 tablet/表聚合，批量调用 `AI_EMBED` 可一次多输入）
  - 用 `ObMySQLTransaction` + 快照读已提交源数据（`SELECT ... AS OF SCN` 或普通读当前已提交版本）
  - 复用 `ObAIFuncModel::call_completion/call_embedding`，带 `max_retries`
  - 结果校验：`ObAIFuncUtils` 中已有的类型/维度/JSON 校验逻辑可直接复用
  - 进入 T2 发布
- 并发控制：`max_concurrency`（端点级）复用/新增限流；避免打爆模型服务

### 4.6 版本与整体发布（T2）

在 `ObAIColumnWorker` 的 T2 中：

1. `SELECT` 当前行、校验 `source_version`（可用隐藏版本列或 `commit_version` 比较；若期间被更新则丢弃）。
2. 对同一行同一批次的多个 AI 列，全部校验成功后**一次性 UPDATE** 写回（或写 `__all_ai_column_result` 后统一发布）。
3. 若源数据在请求后被修改：丢弃结果（不写），让新任务负责。
4. 重试耗尽：标记 failed（供 `SHOW AI COLUMNS` 与 `RETRY FAILED` 使用）。

### 4.7 可见性与严格读取

- 存储层：AI 列值在未发布前为 NULL；用任务/状态位区分「pending / failed / stale / ok」。
- 读取改写：非严格模式直接把 NULL 返回；严格模式在**优化器/执行器**阶段判断语句引用了哪些 AI 列，
  结合行状态标记，发现 pending/failed 立即 `OB_ERR_...`。
  - 实现点：`ObRawExpr` 收集引用列 → 计划生成时挂 hint/标记 → 执行器在 `ObTableScan` 输出前检查。
  - 简化版：在 resolver 阶段对「引用 AI 列」的语句加标记，执行前做一次状态检查（一致性弱但易实现）。
- 会话变量 `ai_column_strict_read`（`src/share/session/ob_basic_session_info` / sys var 注册）。

### 4.8 事务 / 并发 / 崩溃

- T1 与模型调用无分布式事务；允许重复调用。
- T2 为独立事务；通过版本比较防止旧结果覆盖新结果。
- 任务表持久化 → 崩溃重启后重新扫描未完成任务，允许重复执行（幂等写回 + 版本校验）。
- 并发两事务更新同一行：由行锁/事务隔离保证最终提交者胜出；T2 只发布与当前版本匹配的结果。

### 4.9 索引一致性

- AI 列的值通过普通 DML 写回，索引维护由现有路径完成。
- 失效时：同一个 T2/失效事务里清空列值并删除/更新索引项（普通索引自动；JSON/全文/向量走各自 DML 路径）。
- 要求：全表扫描、普通索引、全文索引、向量索引对「有效值」判断一致；建议统一以列值是否为 NULL + 状态位为准。
- 新向量完成后自动进入 VSAG HNSW（复用 change stream 异步索引插件机制，或直接 DML 更新增量索引）。

### 4.10 RETRY

- 语法：`ALTER TABLE <t> RETRY [FAILED] AI COLUMN <c> [WHERE <cond>]`
- 语义：`WHERE` 选行；`FAILED` 仅重试未成功行；省略 `WHERE` 覆盖全部。
- 实现：解析为 `ObAlterTableStmt` 的新 alter type；执行器按条件选出 rowkey 集合，批量登记任务（版本取当前）。

### 4.11 成本统计

- 记录每次实际模型调用的 `prompt_tokens/completion_tokens`（含失败重试）；
- 提供会话/表级别统计视图（可作为附加项，非硬性）。

---

## 5. 文件级改动清单

| 模块 | 文件 | 改动 |
| --- | --- | --- |
| 语法 | `src/sql/parser/sql_parser_mysql_mode.y` | `column_definition` 增加 AI COLUMN 规则；新增 `T_AI_COLUMN_DEF`；`ALTER TABLE ... RETRY` |
| 词法 | `src/sql/parser/sql_parser_mysql_mode.l` / 关键字表 | 新增 `AI` 关键字（非保留） |
| AST | `src/sql/resolver/.../ob_item_type.h`、`parse_node.c` | 新节点类型与构造 |
| DDL 解析 | `src/sql/resolver/ddl/ob_ddl_resolver.cpp/.h` | `resolve_column_definition` 分支；静态校验 |
| DDL 解析 | `src/sql/resolver/ddl/ob_alter_table_resolver.cpp` | ADD COLUMN / RETRY 处理 |
| Schema | `src/share/schema/ob_column_schema.h/.cpp` | `AI_COLUMN_FLAG`、字段、序列化、比较/拷贝 |
| Schema | `src/share/schema/ob_schema_struct.h` | 新标志位定义 |
| 系统表 | `src/share/inner_table/...` | `__all_column` 字段映射（如新增列） |
| 显示 | `src/sql/executor/ob_show_*`、`ob_sql_show_*` | `SHOW AI COLUMNS` |
| 异步 | `src/observer/change_stream/ob_cs_plugin_ai_column.*` | 新插件（方案 A） |
| 异步 | `src/observer/ai_column/ob_ai_column_*.{h,cpp}` | 任务表、worker、版本发布 |
| 读取 | `src/sql/engine/`、`src/share/session/` | 严格读取会话变量与检查 |
| 测试 | `tools/deploy/mysql_test/test_suite/ai_column/` | 新增 mysqltest |
| 单测 | `unittest/sql/...` | 解析/校验/版本逻辑 |

---

## 6. 分阶段实施与验收

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| P0 | 构建环境修复（见 §8）+ 建分支 | 可增量编译 |
| P1 | 语法 + Schema 元数据 + DDL 校验 | `CREATE/ALTER ... AI COLUMN` 成功；非法维度 DDL 报错；`SHOW CREATE TABLE` 正确回显 |
| P2 | `SHOW AI COLUMNS` | 输出列名、表达式、依赖、状态字段 |
| P3 | Change Stream 插件 + 任务表 + Worker（INSERT/UPDATE 依赖列） | INSERT 后等待可见；只改无关列不重算 |
| P4 | 版本/整体发布/回滚/DELETE/迟到结果 | 课程稿 8.1–8.3 用例通过 |
| P5 | 严格读取 + RETRY + 崩溃恢复 | 8.4、7 节用例通过 |
| P6 | 索引一致性（普通/JSON/全文/向量） | 9 节用例通过 |
| P7 | 成本统计、文档、稳定性 | 全量回归 |

---

## 7. 风险与取舍

1. **`__all_column` 序列化变更**：影响面最大。优先尝试「单字段 JSON 编码」减少改动。
2. **解析器重生成**：Windows 下需 `win_flex_bison` 在 PATH，否则 cmake 配置失败（见 §8）。
3. **Change Stream 阻塞**：插件内同步调模型会拖慢流水线；任务表 + 独立 worker 更稳。
4. **严格读取的一致性**：执行期检查与优化器改写需仔细处理投影/过滤/子查询/UPDATE/DELETE 的引用列。
5. **索引一致性**：向量索引异步增量与结果发布的事务边界要一致，否则会出现「值已失效但索引仍可搜到」。

---

## 8. 构建与测试环境（本机已验证）

Windows + PowerShell 5.1 下构建踩坑与解决：

1. 脚本执行策略：用
   `C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 ...`
2. `cmake/Env.cmake` 通过 `powershell` 读取 `PROCESSOR_ARCHITECTURE`，需把
   `C:\WINDOWS\System32\WindowsPowerShell\v1.0` 加入 PATH，否则 `ARCHITECTURE` 为空导致配置失败。
3. 解析器生成需要 `win_bison/win_flex`，把
   `D:\deepseek\seekdb\deps\3rd\tools\win_flex_bison` 加入 PATH。
4. ninja 在受限沙箱下创建子进程会挂起，需以放宽沙箱（danger-full-access）运行。
5. 增量构建命令：

```powershell
$env:PATH = "C:\WINDOWS\System32\WindowsPowerShell\v1.0;D:\deepseek\seekdb\deps\3rd\tools\win_flex_bison;$env:PATH"
.\deps\3rd\tools\cmake\bin\cmake.exe -S . -B build_release -G Ninja `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON `
  -DOB_VCPKG_DIR=... -DOB_OPENSSL_DIR=... -DOB_LLVM_DIR=D:/deepseek/seekdb/deps/3rd/tools/llvm18
.\deps\3rd\tools\ninja\ninja.exe -C build_release -j 16 seekdb
```

测试：`ai_function_enhancement.test` 依赖运行在 `0.0.0.0:8000` 的 Mock AI Service；
AI 列测试同样应基于 Mock 服务，覆盖课程稿第 5–9 节场景。

---

## 6. 当前实现状态（ai-column-2026 分支）

### 6.1 已实现并可在实例上验证

| 能力 | 实现位置 | 验证方式 |
| --- | --- | --- |
| `col <type> AI COLUMN (<ai_expr>) WITH (sync_mode, max_retries)` DDL 语法 | `src/sql/parser/sql_parser_mysql_mode.y`、`ObDDLResolver::resolve_ai_column_attribute` | `SHOW CREATE TABLE` 原样回显 |
| 选项校验（仅 async、`max_retries` ∈ [0,100]） | `src/sql/resolver/ddl/ob_ddl_resolver.cpp` | 非法选项报错 |
| 表达式白名单与类型契约（AI_COMPLETE 限标量、AI_EMBED 限 VECTOR、维度一致性） | `ObDDLResolver::check_ai_column_expr` | 维度不一致时 DDL 拒绝 |
| 元数据持久化（AI 标志 + 表达式 + 选项写入 `__all_column`） | `ObColumnSchemaV2::set_ai_column_meta` / `refresh_ai_column_meta` | 重启后 `SHOW CREATE TABLE` 仍显示 AI COLUMN |
| 用户会话禁止直接写 AI 列 | `ob_insert_resolver` / `ob_dml_resolver` / `ob_del_upd_resolver` | `INSERT/UPDATE` 显式赋值 AI 列报错 3105 |
| 每个 AI 列 3 个隐藏列（state/request/spec），`__all_column.is_hidden=1` | `share::schema::append_ai_hidden_columns` | `__all_ai_column` 关联 `__all_column` 校验 |
| 可观测元数据表 `oceanbase.__all_ai_column`、任务表 `oceanbase.__all_ai_column_task` | `src/share/schema/ob_ai_column_utils.cpp` | `SELECT * FROM oceanbase.__all_ai_column*` |
| Change Stream 插件不再重复生成 AI 列（仅由任务 Worker 负责） | `ObCSAiColumnProcessor::process` | 每个 AI 表达式只发生一次模型调用 |
| DML 同事务失效与任务登记（INSERT/依赖列 UPDATE/rowkey 变更/删除清理） | `ObTableModifyOp::AIColumnHandle` + `execute_ai_inner_sql` | 事务内见 NULL + 任务行，ROLLBACK 后同时消失 |
| 异步 Worker（整组发布、版本栅栏、重试计数、失败终态、重启重置 RUNNING） | `src/observer/ai_column/ob_ai_column_worker.cpp` | 整组同时可见、迟到结果被栅栏丢弃 |
| 列级严格读取（`ai_column_strict_read=ON` 仅检查语句实际引用的 AI 列） | `ObDMLResolver::check_ai_column_strict_read` | 未完成时报 11120，覆盖子查询/UPDATE/DELETE |
| `ALTER TABLE ... RETRY [FAILED] AI COLUMN c [WHERE ...]` | `src/sql/engine/cmd/ob_table_executor.cpp` | 支持伪列 `STATUS`/`READY`、WHERE 选行、与其他 ALTER 动作互斥（1235） |
| DML 参数携带 AI 列标志（按数据表而非索引表判定） | `src/sql/code_generator/ob_dml_cg_service.cpp` | 已修正 index table → data table 解析 |
| Fake Upstream 与调用统计 | `tools/ai_gateway/{README.md,test_gateway.py}` | `python3 tools/ai_gateway/test_gateway.py stats` |
| 单元测试入口 | `unittest/sql/parser/test_ai_column_parser.cpp`、`unittest/sql/resolver/test_ai_column_resolver.cpp`、`unittest/share/schema/test_ai_column_schema.cpp` | 需接入仓库 unittest 构建（当前仓库未提供 CMake 目标） |

验收用例全部通过（逐条回放 `tools/deploy/mysql_test/test_suite/ai_column/t/`）：

| 用例 | 覆盖点 |
| --- | --- |
| `ai_column_ddl.test` | DDL 语法、元数据、隐藏列、归一化表达式、非法维度/依赖 |
| `ai_column_t1_dml.test` | 任务登记与失效的事务性、无关列不重算、主键更新、批量插入、删除清理 |
| `ai_column_worker.test` | 类型化 prompt、整组发布无部分可见、失败重试（`[400,200]`）、终态清理、迟到结果栅栏 |
| `ai_column_retry_strict.test` | RETRY 选行与伪列、严格读取列级判定、失败任务清理、与 ADD COLUMN 互斥 |
| `ai_column_vector_index.test` | AI_EMBED 生成与向量索引一致性 |

### 6.2 已知限制

1. `SHOW AI COLUMNS FROM t` 尚未接入（验收用例未使用）。
2. `sync_mode` 仅支持 `async`，不支持内联同步求值。
3. VECTOR 类型的 AI 列表达式只保存在内存 schema 中（`extended_type_info_` 已被
   `VECTOR(n)` 占用），重启后由 `__all_ai_column.normalized_expr` 作为权威来源。
4. 严格读取在 resolver 阶段做一次列级探测（`SELECT 1 ... WHERE col IS NULL LIMIT 1`），
   与设计 §4.7 的“简化版”一致：判定为列级（与查询选中的行无关），但弱于执行期逐行检查。
5. 网络类模型调用失败不再在 AI 客户端内部重试（交由任务状态机按 `max_retries` 重试），
   以便失败任务能在用例要求的时限内进入终态。

### 6.3 本地验收方法

```bash
# 1) 启动 Fake Upstream
python3 tools/ai_gateway/test_gateway.py start --port 19120

# 2) 启动实例
./build_release/src/observer/seekdb --nodaemon --base-dir <dir> --port 2881

# 3) 回放验收用例（Linux 下使用 mysql_test 框架；Windows 下用 pymysql 逐条执行）
tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_ddl.test
```

> 说明：本节如实反映分支现状。6.2 中的限制项为已知未覆盖范围，其余 6.1 能力已按
> `tools/deploy/mysql_test/test_suite/ai_column/t/` 下的 5 个用例逐条回放验证。

