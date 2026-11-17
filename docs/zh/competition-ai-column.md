# 赛题三：异步 AI 列——实现与验证记录

## 当前状态

第三题本地实现、真实编译、回归及独立审查收尾已完成，第一题、第二题实现均保留。本文记录本地验收证据，不代表正式平台得分；提交和推送信息以当前分支 Git 历史及远端 SHA 为准。

下文前部为历史开发记录；当前验收结论以末尾第 9 轮及最终复核为准。

已读取题面和 `tools/deploy/mysql_test/test_suite/ai_column/` 下五个原始测试及预期结果，未修改公开测试。

已完成的原生解析器切片：

- `AI COLUMN (expr)` 作为普通列的独立属性，保留用户可见结果列，不冒充同步生成列。
- 可选 `WITH (sync_mode='async', max_retries=...)`，保存表达式原文和原始选项 AST。
- 专用 `T_AI_COLUMN = 4930`，不改变已有枚举值；属性节点两子节点为表达式、可选 T_EXPR_LIST。
- 普通列定义仍为原来的四子节点结构，已有生成列语法不变。
- 原生回归覆盖 CREATE、ALTER ADD、缺省配置、多 AI 列、普通生成列及 `ai` 作为普通标识符。

验证证据：

1. 未修改的已编译服务器执行真实建表用例，返回 SQL 1064，确定原先不支持此语法。测试实例正常关闭。
2. 原始缓存解析器运行新增原生用例：四个 AI 语法失败、两个普通 SQL 成功。
3. 新语法经 Bison `-Werror` 生成并编译，六个原生用例全部通过，同时校验 AI 节点、原始表达式和选项结构。
4. 第二题原生解析器回归使用同一新生成解析器执行成功。

**这些是解析器层结果，不代表 CREATE TABLE 已能持久化 AI 定义，更不代表 DML、任务调度或后台模型推理已实现。** SQL 集成首个用例目前仍是待实现的 RED。

## 必须实现的语义

- 通过原生 Schema、SQL、DML 和后台任务机制实现，禁止应用层代理、响应硬编码或仅内存队列替代持久化任务。
- CREATE/ALTER 保存 AI 表达式、依赖、配置和定义版本；每个 AI 结果列有三个隐藏状态/请求/结果规格列。
- AI_EMBED 返回维度与 VECTOR 类型一致；拒绝 AI 列之间依赖；结果列禁止业务 SQL 直接赋值。
- 插入、依赖列变更及主键变更，同事务执行结果失效与任务创建。无关列更新不触发。回滚恢复旧值、旧索引、任务状态；删除清理任务。
- 模型请求只能在业务事务提交后发起；后台领取、恢复、有限重试和终态失败使用持久化任务。
- 写回核对请求 ID、定义版本、行归属，拒绝迟到或已删除行的结果。不得通过写回重新创建已删除行。
- 同一行同次变更触发的一组 AI 列，在各任务达到终态后一次事务发布；成功为 READY、失败为 FAILED，不能部分提前可见。
- NULL 结果不能代替状态；未完成和失败默认读 NULL，但成功生成 NULL 仍可能为 READY。
- 严格读取按被引用的 AI 列在整张表的完整状态检查；查询 WHERE 只命中 READY 行也不能忽略该列其他未就绪行。SELECT/UPDATE/DELETE 子查询与谓词均纳入引用检查。
- RETRY 只选择指定 AI 列和 SQL 谓词命中的行；未引用的其他 AI 列不受影响。未加反引号的 status 是状态伪列，反引号 status 是业务列。
- AI 向量失效、普通表数据和 VSAG/HNSW 索引维护使用原生事务路径；生成后更新索引，回滚恢复可见性。

## 原始公开测试范围

- `ai_column_ddl`：DDL、系统元数据、隐藏列、类型提示、维度和依赖约束。
- `ai_column_t1_dml`：提交前任务可见、模型调用须提交后、依赖/无关列/主键更新、批量与回滚删除。
- `ai_column_worker`：类型化 system prompt、分组发布、有限重试、终态任务清理、新请求胜出。
- `ai_column_retry_strict`：严格列级读取、业务条件与状态条件重试、引用范围、错误码、非法 ALTER 混用。
- `ai_column_vector_index`：Embedding、向量索引、失效删除旧项、写回加入新项、回滚一致性。

## 本地新增测试

- `tests/competition/test_ai_column_parser_native.cpp`：真实原生解析器测试。
- `tests/competition/test_ai_column_integration.py`：真实 SQL DDL/元数据用例（当前仍 RED）。

当前未提交、未推送；完整服务器重编译和全部第三题公开测试尚未完成。

## 联合集成进度（继续开发）

工作区还保留了 Schema 编解码、DDL 定义持久化、同事务 DML 入队、后台 worker 的部分实现；这些源码不能等同于已完成第三题。本轮按实际代码接续，不覆盖第二题改动。

- `ai_column_dml.cpp` 已加入 SQL 原生 standalone source inventory。
- 完整 DML、DML service、codegen、DML resolver、ctdef、session 等 7 个翻译单元以实际构建 flags 做语法检查，全部通过。证据：`ai-column-dml-parent-syntax.log`。
- 对缓存服务器运行 7 项 DML SQL 测试，均在 `CREATE TABLE ... AI COLUMN` 阶段返回 1064。该证据只说明缓存二进制尚未集成语法，不能声称事务/DML 语义已经验证。证据：`ai-column-dml-parent-baseline.log`，测试实例正常关闭。
- DDL/持久化系统表、worker 生命周期、严格读取与 RETRY 的源码已联合接入。worker 与 strict/retry CPP 均已加入对应 source inventory；RETRY 变更指定 request helper 会触发该列重新入队。

## 第一轮真实服务器结果

- 首次完整服务器编译、链接成功；编译期间 canonical resolver/错误码修复发生变化，快照检查正确要求再次构建。该第一份二进制 SHA256：`786b80a2d46cc5012b629a680f702b25b10dc123d0193fe5527218000b11a85b`，不可冒称最新源码版本。
- 6 项真实 DDL/元数据测试全部通过；原始公开 `ai_column_ddl` mysqltest 通过（SQL/result 未修改）。日志：`ai-column-first-ddl.log`、`ai-column-first-public-ddl.log`。
- 7 项 DML 用例均在实际 INSERT 处返回 SQL 4016，worker SQL 中 12 项也被该 INSERT 错误阻塞，已进入原生 DML 排错。不是连接失败，也不再是语法 1064。日志：`ai-column-first-dml.log`、`ai-column-first-worker.log`。
- 第一题 60 项 AI SQL 和两个原始公开测试仍通过；第二题 20 项文件 SQL 仍通过，日志：`ai-column-first-ai-regression.log`、`ai-column-first-file-regression.log`。
- strict-read 已按公开测试契约使用原生 11120，带 table_id/column_id 的消息；普通不支持的 RETRY 仍为 1235。RETRY 的 SQLUtils、resolver、prepare 分类入口已全部修补，父任务重跑 8 项原生分类回归通过。日志：`ai-retry-parent-applied-green.log`。
- 最新源码第二轮全量构建运行中；第三题整体尚未完成，无正式平台分数，未提交/推送。

## DML 事务路径接续验证

- 第二轮构建在链接 `ObSQLUtils::get_sql_item_type(const ParseNode&)` 时失败；重新编译精确的陈旧 unity 对象后，服务器链接成功。未修改依赖或公开测试。
- 依据现有 GDB 跟踪，第一条内部任务 DELETE 的 close 误走外层 `set_end_stmt`，返回 -4016。`ObSQLUtils::is_nested_sql` 现识别具有非空父执行上下文、且持有私有 AI COLUMN internal-write capability 的嵌套 SQL；不冒用 FK/统计任务标记，不公开该权限。
- 修改后实际二进制 SHA256：`62429053c9f032cee0c2f9f2317cb9991215b58b73d219fdc089831a56485f57`。9 项 DML 集成测试全部通过，包含语句级失败回滚、后续语句可用、事务内任务与结果失效、批量/主键/删除/UPSERT。原始公开 `ai_column_t1_dml` 通过。
- 验证日志：`ai-column-resume-dml-5-green.log`、`ai-column-resume-public-t1.log`。测试实例均正常关闭。
- 构建时 worker 源码仍在并行修复，快照身份检查正确要求重新构建；上述二进制不冒充最新全部 canonical 源码。CHAR/TEXT 结果和超大任务输入隔离仍在修复，第三题整体尚未验收。
- 同一快照实际运行原始公开 `ai_column_worker` 和 `ai_column_vector_index`，两项均通过；`ai_column_retry_strict` 在第 63 行读取已就绪的 label 列时返回 4016，严格读取内部查询的生命周期仍待修复。日志：`ai-column-resume-public-runtime.log`；不能宣称五个公开用例全部通过。

## Worker 联合构建与公开测试接续

- 已在旧二进制上实际复现 CHAR/TEXT 持续排队和 5 MiB 输入阻塞：`ai-column-worker-parent-semantic-red-2.log`，6 个语义断言失败；另一个 latin1 用例因基线不支持该字符集而不能作为语义 RED。
- 新 worker 支持 DDL 接受的字符串类型，区分字符数与目标编码字节限制；输入元数据批次不再 HEX 原始 LONGTEXT，单任务输入超过 4 MiB 走显式失败/重试/发布流程，不截断调用模型。
- 本地测试改用基线实际支持的 UTF8MB4/BINARY，不额外启用 latin1。隐藏辅助列不向普通 SQL 暴露，测试使用行锁内的公开 RETRY/status 谓词并回滚观察状态；录制服务只对整数请求注入坏整数，避免意外同时破坏 JSON 成功列。官方 SQL/result 未修改。
- 最新完整构建验证 canonical 源码一致，日志 `ai-column-worker-parent-build-6.log`：`AI_COLUMN_CANONICAL_BUILD_VERIFIED`。二进制 SHA256：`7929d8657c4984f3ec8bc31cb2b29f70f1bb74d335e2c5e7d3a3e6835bc5f114`。
- `ai-column-worker-parent-sql-6c.log`：17 项测试全部通过，其中含 15 项 worker SQL 场景和两个原生值/SQL 表达式辅助用例。
- `ai-column-parent-public-6.log`：五个原始公开 AI COLUMN mysqltest 全部通过，包括 DDL、DML、worker、strict/retry 和 vector index。只隔离 Mock 服务的状态目录，SQL、断言、结果和模型协议均保持原始内容；仍非正式平台分数。
- 第三题完整回归、前两题回归和最终独立审查尚待完成；未提交、未推送。

## 三题最终回归（第 7 轮）

- 构建源码身份检查无漂移；全部验证使用上述 `7929d865...` 二进制。
- DDL：6 项；DML：9 项；strict/retry：9 项，全部通过。加上 worker/辅助 17 项，第三题共 41 项本地测试通过，其中辅助测试不冒称 SQL 集成。
- 第一题 60 项 SQL 与两个公开 mysqltest 仍通过；第二题 20 项 SQL 与公开 `file_sql_basic` 仍通过。
- 第三题原始五个公开 mysqltest 的通过结果保持；任何本地通过均不等于正式平台得分。
- 证据：`ai-column-final-7-report.json`、`ai-column-final-7-{ddl,dml,strict,ai,file,file-public}.log`。
- 所有自有测试实例/协议服务已关闭；未提交、未推送。最终三路只读审查进行中，不能提前宣称审查完成。

## 最终审查：未通过，继续修复

三路审查返回 5 个 Major：MODIFY/CHANGE 静默丢失 AI 属性、NO_BACKSLASH_ESCAPES 提示词/模型名往返、READ COMMITTED 严格读取快照窗口、AI_PROMPT 原生 JSON 类型丢失、多个大 staged_result 污染元数据批次。

- 父任务已用 `7929d865...` 二进制实际复现前三类中的 ALTER/字面量和 AI_PROMPT 问题：`ai-column-final-review-reproductions-red.log`，4 个测试、4 处断言失败；普通 VARCHAR 中 envelope 形状 JSON 保持字面文本的对照通过。
- 大结果组实际复现：`ai-column-large-staged-group-semantic-red.log`，五个 LONGTEXT 目标的各 4 MiB 结果未在期限内联合发布。该轮记录 6 次 HTTP，不能据此断言无关小行也完全停止推理。
- READ COMMITTED 快照窗口目前为源码追踪结论，尚待可控双会话交错验证，不冒称已实际复现。
- 已并行修复三组代码；旧公开测试通过不能覆盖这些缺口，第三题不视为最终完成。未提交、未推送。

## 第 8 轮：Major 专项修复验证

- 上次三个修复子任务均因额度错误中断，留下部分修改。父任务已检查实际源码、补齐上下文字段、快照接入和逐值发布，不将中断任务当作完成报告。
- 完整构建成功，`ai-column-review-fixes-build-8.log` 含 `AI_COLUMN_CANONICAL_BUILD_VERIFIED`；二进制 SHA256：`1ccfee6eef381c01c062d28c01670b72d0b0bcc62746a35dc7c6a5f4fe16f58d`。
- `test_ai_column_review_reproductions.py` 的 5 项真实 SQL/HTTP 最小复现全部通过：MODIFY/CHANGE 拒绝、NO_BACKSLASH_ESCAPES 字面量、原生 JSON prompt 渲染、普通 VARCHAR envelope 对照、大结果组及无关小行发布。日志 `ai-column-review-fixes-semantic-8.log`。
- READ COMMITTED 不再仅是源码结论：外部 GDB/MI non-stop 调试器只暂停指定连接的外层 SQL 线程，另一连接真实提交依赖更新；旧 `7929d865...` 服务器随后 SELECT 成功返回 NULL，没有 11120，形成确定性语义 RED。日志 `ai-column-snapshot-parent-red-3.log`。
- 相同外部屏障下，新二进制的 SELECT、UPDATE、DELETE 子查询窗口以及“快照建立后提交仍读取旧 READY 值”的对照共 4 项通过，无跳过。日志 `ai-column-snapshot-parent-green-8b.log`。调试器不是生产测试开关，不改业务 SQL 或存储。
- 严格检查在原生外层 statement snapshot 建立后、扫描前执行；同会话私有 nested SQL 继承该快照。收集显式引用时不再做 resolver-time 数据探测；带 AI 引用的 INSERT SELECT 不走 none-read 快照优化。
- 原生 JSON 输入的类型来源保存在任务规格中；只对真正 JSON 恢复 AI_PROMPT renderer，VARCHAR 外形相同的 JSON 不猜测、不渲染。分组元数据不装 staged_result；每次加载一份有界结果，在同一行锁/事务内逐列原生 UPDATE，单次提交保证原子可见，不提高通用字段/聚合限额。
- 扩展 DDL/SHOW CREATE 跨模式往返 6 项、prompt/大组/已 staged 恢复 4 项通过：`ai-column-ddl-edges-8b.log`、`ai-column-prompt-large-8b.log`。测试修正：在清理特殊模型名之前恢复 SQL mode；提示词组合用已支持的 COALESCE 和标量参数，未把不支持的 CASE 或整数组参数的不同语义算成产品回归。
- 第 8 轮联合回归结束：DDL 6、strict/retry 9、worker/辅助 17、第三题原始公开 5 个、第一题 60 SQL/2 公开、第二题 20 SQL/1 公开通过。DML 首轮仅因自有测试仍断言旧 result_spec version=1 而失败；按照新增类型来源规格改为 version=2 并断言 input_kind=text 后，9 项全部通过，日志 `ai-column-final-8-dml-v2-spec.log`。原始失败报告保留，不篡改成全绿。
- 两路独立复审结束：prompt 测试 CASE/整数组参数问题已经在本轮实际修正并跑通；另确认严格检查报 11120 后跳过外层 end_stmt，导致语句 scope/快照保留未清理，需继续修复。
- 外部调试器已直接读取真实原生会话字段，在 11120 返回且任何后续 SQL 执行前，autocommit 与显式事务均实际复现 nested_count=0、保留快照仍有效，应为 -1/无效。日志 `ai-column-rejected-scope-red-8c.log`，不是只依据源码推测。
- 清理修复已派给独立实现任务，只允许修改 `ob_sql_trans_control.cpp`，保留先前父语句/事务和原始 11120；父任务随后重编译并检查失败清理、先前未提交写入和全部回归。当前仍未最终验收、未 commit/push/平台提测。自有服务器、调试器和 fixture 进程已核实为空。

## 第 9 轮：严格拒绝清理及全量重跑

- 已接入独立实现的 startup 清理补丁：记录本次 scope/savepoint/reservation 的所有权，仅在严格校验失败时使用适当的原生 end_stmt 或局部回滚；恢复被覆盖的父快照保留，不关闭父语句，保留最初的严格读取错误。
- 完整构建与 canonical 身份检查通过：`ai-column-cleanup-build-9.log`。新二进制 SHA256：`a0e9b958f2fb2b06579d46a365bde5d1a8a306dee019c62f6895c81a8ec2804e`（这是二进制哈希，不是 Git 提交 ID）。
- `ai-column-snapshot-cleanup-green-9.log`：6 项无跳过外部调试器/SQL 测试全部通过；新增 INSERT SELECT 窗口，在 SELECT/UPDATE/DELETE/INSERT SELECT 拒绝后直接检查原生状态清理，并覆盖自动提交、显式事务此前未提交写入保留和回滚。
- `ai-column-focused-all-9.log`：15 项最终审查专项全部通过，包括跨 SQL mode 的 SHOW CREATE 往返、真实 JSON prompt 与 VARCHAR 对照、5×4 MiB 结果分组及已 staged 崩溃边界恢复。
- `ai-column-final-9-report.json`：8 个联合套件全部 exit 0。第三题基础 DDL 6、DML 9、strict 9、worker/辅助 17，加上述专项共 62 项本地测试（含 2 项原生 helper），及 5 个原始公开用例均通过。第一题 60 项 SQL/2 个公开、第二题 20 项 SQL/1 个公开仍通过。
- 完成后再次确认 canonical 源码无漂移、自有 seekdb/GDB/fixture 进程为空；`git diff --check` 通过，官方测试/固定依赖/Rust 路径无改动。清理补丁新增行静态安全扫描无匹配。
- 清理补丁的最终独立只读复核已结束：在本次增量范围内无确认的 Blocker/Major。原始五项 Major 及后续失败清理 Major 均已修复并有真实测试证据；先前测试 CASE/整数组参数问题也已修正并重跑。
- 审查限制明确保留：已有父语句/父快照保留的特殊分支、清理失败注入、锁等待竞争仅作源码追踪，未独立运行专项故障注入；不将此结论扩大为所有路径均经运行验证。
- 本地第三题验收完成。未 commit/push/平台提测，HEAD 仍为 `f8b5452cc456fcc0ac17643d7224dce3a0da1855`，旧提交仅含第一题；第二、三题需用户授权后生成新提交并推送团队分支。