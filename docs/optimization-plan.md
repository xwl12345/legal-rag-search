# 法律 RAG 检索引擎 · 优化方案

> 依据：2026-10-06 全项目体检报告。体检结论：构建干净、71/71 单测通过、UI E2E 与评测工具全部可跑通；
> 共排查出 26 项缺陷（3 项崩溃级、5 项数据损坏级）；解耦水平「编译期 8 分 / 运行期 4 分」。
> 本方案将修复与重构组织为 **P0 → P1 → P2 → P3 四期**，每期独立可交付、可回滚、有验收线。
>
> **状态：v1.1 已定稿（2026-10-06），五个决策点已拍板（见第 9 节），尚未开工。**

---

## 0. 总原则

1. **答辩安全优先**：先消灭「换台机器就打不开」「演示现场会崩」的问题，再做架构优化。
2. **现有测试是底线**：`run_tests`（71 例）、`test_utf8_location_regression`、`ui_smoke --e2e`、
   `eval_retrieval` 是每一期的统一验收线，任何一期结束时必须全绿。
3. **不动检索算法本身**：所有可能改变 BM25 / 融合结果的行为变更，必须先跑 `eval_retrieval`
   留档前后指标对照（论文数字要同步），劣化则回退该变更。
4. **不引入新第三方库**：全部用 Qt / 标准库自带能力解决问题。

---

## 1. 四期总览

| 期 | 目标 | 预估工作量 | 风险 | 依赖 |
|----|------|-----------|------|------|
| **P0 止血包** | 修复 8 个必修 bug，保证演示与换机部署安全 | 0.5–1 天 | 低 | 无 |
| **P1 线程模型重构** | UI 线程零阻塞，结构性消灭重入 | 2.5–3.5 天 | 中（核心） | P0-2 的忙碌状态机 |
| **P2 接口抽象与业务下沉** | 网络可注入、业务规则归位引擎层、可脱离网络测试 | 2 天 | 中低 | P1 稳定后 |
| **P3 配置合一与工程卫生** | 消灭双配置漂移、CMake 收敛、文档对齐现实 | 1–1.5 天 | 低 | 无（可与 P2 穿插） |

合计约 **6.5–8 个工作日**（含每期测试与留档）。执行顺序建议 P0 → P1 → P2 → P3；
P3 不依赖前序，若中途需要切回论文写作，P3 的零散项可拆开穿插进行。

### 1.1 与功能进度（任务卡 T5-T12）的穿插顺序（建议，2026-10-06 提出，待拍板）

进度台账：T4 已完成（2026-10-05），下一张任务卡是 T5（段落角色标注）。
建议**不做**「全部修完再开发」，也**不做**「全部开发完再一起修」，而是按「与功能链路的耦合度」拆开修：

```
现在 ──► P0 止血包（0.5–1 天）
     ──► T5 段落角色标注（约 1.5 周，任务卡照常推进）
     ──► P1 线程模型重构（2.5–3.5 天）
     ──► T9 → T10 → T11 → T12 页面潮（按原计划波次）
     ──► P2 / P3 按原方案时机穿插
```

理由：
- **P0 先于 T5**：P0-5（fullText 重叠重复）、P0-6（GBK）、P0-3（metadata 下溢）与 T5 改的是
  同一条导入-解析-持久化链路（`retriever.cpp` / `parser.cpp` / `index_store.cpp`）——
  先修 = T5 在干净地基上开发，测试与落盘格式不用二次返工；P0-1（词典炸弹）关系到换机部署，早拆早安心。
- **P0-5 必须先于 T10**：全文阅读页（T10）的数据源就是 `StoredDocument.fullText`
  （T1 验收注明「满足 T10 全文阅读页数据源要求」），现状每 512 字节重复 50 字节——
  先建 T10 后修 = 明知有缺陷还交付演示功能，修的时候还要连带改 T10 的显示与测试。
- **P1 先于 T9-T12 页面潮**：线程重构的迁移成本随页面数增长（后面还有法条/全文/收藏/报告
  4-5 个新页面）。「做完再一起修」会让 P1 变成涉及全部页面的大迁移；先立异步骨架，
  新页面从第一天就建在正确模型上。
- **P2/P3 不阻塞任何功能卡**：维持原方案时机（P2 可放 T5 之后；P3 卫生项随时穿插）。

时间上几乎无代价：P0 + P1 合计约 4 天，T5 结束后 10 月下旬即可进入 T9-T12，
仍大幅领先原计划的第四波（2026.12 底-2027.1）。

---

## 2. P0 止血包（必修，0.5–1 天）——✅ 完成（2026-10-06）

**验收证据（实测）**：
- 构建 5 个 target 零错误零警告；
- `run_tests` **77/77**（原 71 + P0 新增 6：下溢守卫 / fullText 原文 / GBK 转码 / BOM 剥离 / 二进制拒绝 / 向量缓存观测口）；
- `ui_smoke --e2e` 1x 与 1.5x 两档**全部通过**（88 项 OK），新增 P0-2 八项断言
  （忙碌态 / 按钮禁用 / 忙碌期导入被拒 / 关窗拦截 / 任务继续 / 完成后解除恢复）
  与 P0-7 九项断言（同路径静默放行 / 覆盖·跳过·取消三路确认 / 内容逐字一致）；
- `eval_retrieval` 四路指标与体检基线**完全一致**（BM25 0.200 / 0.957 / 0.957 / 0.913）
  ——P0 未触碰检索算法，指标零漂移；
- 词典随构建部署到 `build/dict/`，运行期按 环境变量 → exe 目录 → 编译期路径 → 源码树相对 顺序探测。

### P0-1 词典路径改为运行期解析 ⚠️ 启动即崩
- **问题**：`tokenizer.cpp:29-36` 把 `CPPJIEBA_DICT_PATH`（编译期本机绝对路径，CMakeLists.txt:132）
  写死进二进制；词典缺失时 cppjieba `XCHECK` 直接 `abort()`，无法捕获。exe 拷到别的机器即启动崩溃。
- **修法**：
  1. 构建期路径改为**相对路径**定义（仅作搜索候选之一）；
  2. `Tokenizer::Impl` 构造前按顺序探测词典目录：环境变量 `LEGAL_RAG_DICT_DIR` →
     exe 所在目录 `/dict`（部署拷贝）→ 源码树相对路径（开发用）；
  3. 全部找不到时抛带明确指引的异常，`main.cpp` 捕获后弹对话框说明
     （「词典目录缺失，请将 third_party/cppjieba/dict 拷贝至程序目录 /dict」），不再裸 abort。
- **验收**：把工程目录改名后运行能启动或得到明确报错；单测新增「词典缺失路径返回错误而非崩溃」
  （通过注入假目录探测函数实现，不真 abort）。

### P0-2 UI 忙碌状态机（操作互斥）⚠️ 重入是当前最大的运行期风险源
- **问题**：生成回答期间（嵌套事件循环泵输入）导入/清空/对比按钮全部可点；
  `quality_page.cpp` 的对比按钮从未禁用；生成中关窗口会析构正在执行 `generate()` 的对象（UB）。
- **修法**（P1 完成前先止血）：
  1. `SearchPage`/`QualityPage` 引入 `busy_` 标志：检索+生成期间禁用
     导入、清空、检索、对比、批量评测（按钮 `setEnabled(false)` + 状态栏提示「生成中…」）；
  2. `MainWindow::closeEvent`：忙碌时弹确认「正在生成/检索，仍要退出吗？」，确认则先尽力中断；
  3. 生成中断按钮（P1 前先做占位：P0 阶段至少保证退出路径不再 UB —— 退出前置 `answerInterrupted_`
     并等待当前嵌套循环自然返回）。
- **验收**：ui_smoke 新增 E2E：触发生成（FakeKey 环境）后立即点导入 → 断言按钮不可点。

### P0-3 `extractCourt` 回扫下溢守卫
- **问题**：`metadata.cpp:145-159` `text.substr(nameStart - 3, 3)` 无 `nameStart >= 3` 守卫，
  「人民法院」前是截断多字节字符时 `size_t` 下溢 → `std::out_of_range`。
- **修法**：循环条件改为 `nameStart > start + 3`（或回退前先判断 `nameStart - start >= 3`）。
- **验收**：单测构造 `\x80` 开头 + 紧跟「人民法院」的畸形输入，不抛异常且提取合理。

### P0-4 换 Embedding 端点/模型时失效向量缓存
- **问题**：重建条件只有 `similarity_.size() != totalDocs`（retriever.cpp:248），
  `setEmbeddingEndpoint`/`setApiKey`（retriever.cpp:52,70）不清缓存——换模型后旧向量静默复用，
  与 `retriever.cpp:251` 注释及质量页宣称「自动按新配置重算」相反。
- **修法**：`setEmbeddingEndpoint` 与 `setApiKey` 中清空 `similarity_` / `vectorIndexMap_`；
  修正 251 行注释；质量页提示文案保持与实现一致。
- **验收**：单测「改 endpoint 后下次检索触发重建」；实测：改模型后检索耗时可观测（重建批次）。

### P0-5 `fullText` 改用解析原文，消灭重叠重复
- **问题**：`retriever.cpp:137-141` 逐块 `+=` 拼接带 50 字节 overlap 的 chunk，
  全文每 512 字节重复 50 字节，污染全文阅读、元数据提取与 byteSize。
- **修法**：`addDocument` 用 `ParseResult` 里的原文 `content` 直接赋值（与 `addText` 路径对齐）；
  持久化格式若已存 fullText，`loadIndex` 后按新语义兼容（旧落盘文件照旧展示，说明于 CHANGELOG）。
- **验收**：单测断言 `getFullText` 内容 = 原文（无 50 字节周期性重复）。

### P0-6 文本编码检测（GBK → UTF-8）
- **问题**：`parser.cpp:68-73` 原始字节读入，中文 Windows「ANSI」（GBK）文件全链路乱码且无报错。
- **修法**（✅ 已拍板：自动转码）：读入后做 UTF-8 合法性校验（手写状态机或 `QStringConverter`）；
  非法 UTF-8 → 尝试按 GB18030 解码转 UTF-8（Qt `QStringDecoder`，QtCore 自带）→ 仍失败则导入报错。
- **验收**：单测导入 GBK 编码样例 → 检索命中、显示正常；二进制文件（重命名为 .txt）导入被拒。

### P0-7 同名不同目录文件导入确认
- **问题**：docId=文件名（parser.cpp:35），同名先 `removeDocument`（retriever.cpp:127），
  跨目录同名文件静默覆盖 = 无感知丢数据。
- **修法**：`importPaths` 检测目标 docId 已存在且 `sourcePath` 不同 → `QMessageBox` 三选
  （覆盖 / 跳过 / 取消剩余）；同路径重导入（更新场景）保持静默覆盖。
- **验收**：E2E 覆盖同名导入两条路径。

### P0-8 杂项止血
- `generator.cpp:99` SSE `readyRead` connect 补 context 对象（`&loop`）；
- `main_window.cpp:182` 状态圆点接 `statusDotOff`（跟随 embeddingReady / apiReady）；
- `quality_page.cpp:204` 对比前禁用 `compareBtn_`（与 P0-2 同一批改动）。

**P0 验收线**：构建 0 警告 → 71 单测 + 新增用例全绿 → ui_smoke --e2e 全过 → eval_retrieval 指标与体检日一致。

---

## 3. P1 线程模型重构（核心，2.5–3.5 天）

### 3.1 目标与非目标
- **目标**：UI 线程零阻塞（慢网络/大语料/OCR 期间界面流畅可交互）；结构性消灭嵌套事件循环重入；
  生成可中断；引擎代码**保持同步风格**（71 个单测零改动）。
- **非目标**：不改检索算法；不做多线程并行建索引；不重构持久化。

### 3.2 方案选型

| 方案 | 说明 | 结论 |
|------|------|------|
| A. 全异步信号式 | Generator/EmbeddingService 改 QNetworkAccessManager 原生异步，彻底去 QEventLoop；Retriever::search 拆成 BM25（同步）+ 向量（异步）状态机 | **弃**。API 大改、单测难写、四路检索逻辑碎片化，风险收益比最差；留作远期方向 |
| B. QtConcurrent::run 每操作一线程 | 简单但线程池线程无事件循环：OCR 的 QProcess 事件泵、生成流式回调、取消（invokeMethod abort）都别扭 | **弃** |
| **C. 单引擎工作线程 + 粗粒度锁（选定，✅ 已拍板）** | `EngineWorker`(QObject) `moveToThread` 专线程，重操作队列化到 worker；轻只读保留 UI 直调 + Retriever 一把 `std::mutex`（**短临界区**：锁在每次触碰内部数据的动作上加，重操作在文档间/批次间放锁，UI 轻只读最多等毫秒级） | ✅ thread-confinement 免复杂锁；引擎同步代码原样跑在 worker；测试完全不动 |

### 3.3 目标架构

```
UI 线程                                引擎线程（QThread + exec()）
──────────────                        ─────────────────────────────
MainWindow / 五个页面
   │  轻只读（μs 级）: documentInfos / getMetadata / count…
   └──────────── std::mutex 粗锁 ────────────────┐
   │                                             ▼
   │  重操作（队列化）                    Retriever / Generator / OcrClient
   ├── invokeMethod(worker, "search",…) ──► 执行（内部 QEventLoop 只阻塞引擎线程）
   │        ▲                                   │
   │        └──── queued signal 回传结果 ────────┘
   └── 流式 delta: worker → invokeMethod(page, QueuedConnection)  ← 现有写法直接跨线程成立
```

- **归属变更**：`Retriever` 的所有权从 `MainWindow` 移到 `EngineWorker`（`unique_ptr`）；
  页面仍收 `Retriever*`（轻只读直调），重操作一律经 worker。
- **重操作清单**：`search / searchWithMode / generate / importPaths 的 addDocument 循环 /
  批量评测 / saveIndex / loadIndex / OCR Python 首次探测`。
- **轻只读清单**：`documentInfos / getDocumentInfo / getFullText / getChunk / getMetadata /
  allDocIds / documentCount / chunkCount / embeddingReady` 等微秒级访问。
- **取消机制**：导入沿用 `cancelledQuery` 回调（worker 内轮询 atomic 标志，UI 置位）；
  生成中断 = `QMetaObject::invokeMethod(reply, &QNetworkReply::abort)`（引擎线程事件循环会处理），
  UI 加「■ 停止」按钮；中断后走既有 `answerInterrupted_` 落库语义。
- **退出安全**：`closeEvent` → 取消令牌置位 → `thread->quit(); thread->wait(3s)` → 落盘索引。

### 3.4 文件级改动清单
| 文件 | 改动 |
|------|------|
| `include/ui/engine_worker.h` + `src/ui/engine_worker.cpp` | **新增**：命令槽（search/generate/import/eval/save…）+ 对应结果信号 |
| `include/rag/retriever.h` / `src/rag/retriever.cpp` | 全公共方法加 `std::mutex` + `std::lock_guard`（头文件注明线程模型约定） |
| `src/rag/generator.cpp` | `cancel()` 支持；readyRead context（P0-8 已做）；错误体解析留在 P2 |
| `src/ui/search_page.cpp` | `onSearch`/`importPaths`/生成段改为「入队 → 信号回传渲染」；忙碌状态机接 worker |
| `src/ui/quality_page.cpp` | `onCompare`/`onBatchEval` 入队化，逐条进度信号驱动 |
| `src/ui/main_window.cpp` | 创建 thread+worker；`restoreIndexOnStartup` 入队化；closeEvent 收尾 |
| `src/ui/library_page.cpp` | 删除/清空操作入队化（结果刷新经信号） |
| `test/*` | **零改动**（测试直接同步构造 Retriever，不经 worker） |

### 3.5 风险与对策
- **信号时序错乱**（改一页坏一页）：每页改完立即跑 `ui_smoke --e2e`，不攒批。
- **队列堆积**（用户狂点）：P0-2 的忙碌状态机保留——worker 忙碌时 UI 直接拒绝入队并提示。
- **锁粒度**：毕设规模（千块级）一把大锁足够；头文件注释写明「勿在持锁回调里再调引擎」。
- **回滚**：P1 独立分支，出问题 revert 后 P0 成果仍在。

### 3.6 P1 验收线
1. 既有四项测试全绿（零改动通过 = 兼容性证明）；
2. 新增 E2E：检索+生成期间界面可交互（切页/缩放不卡）、点导入被拒绝且提示、
   生成中关窗不崩、生成中点「停止」→ 残卷落库带 interrupted 标记；
3. 慢网络场景（FakeTransport 限速，P2 提前小范围引入亦可）UI 响应 < 100ms。

---

## 4. P2 接口抽象与业务下沉（2 天）

### 4.1 网络传输抽象 `IHttpTransport`
- **接口**（QtCore-only，头文件放 `include/net/transport.h`）：
  ```cpp
  struct HttpResponse { int status = 0; QByteArray body; std::string error; };
  class IHttpTransport {
  public:
      virtual ~IHttpTransport() = default;
      virtual void post(const QUrl& url, const QList<QPair<QByteArray,QByteArray>>& headers,
                        const QByteArray& body, int timeoutMs,
                        std::function<void(HttpResponse)> onDone) = 0;
  };
  ```
- **实现**：`QtTransport`（QNetworkAccessManager，生产）+ `FakeTransport`（单测/冒烟脚本化响应，
  支持 SSE 分片重放、超时注入、乱序 embed 返回）。
- **注入点**：`Generator` / `EmbeddingService` 构造注入（默认 QtTransport）；
  `Retriever` 提供 `setTransport()` 转发；`ui_smoke` 注入 Fake 后可测「有 Key 全链路」。
- **顺带修复的 bug**：
  - HTTP 200 + 错误体 → 解析 body `error.message` 抛出并在 UI 展示（替代现在的静默空白）；
  - `embedBatch` 读响应 `index` 字段重排，不再假设服务端保序（embedding.cpp:96）；
  - `Generator` 硬编码 URL/模型改为成员（为 P3 配置化铺路），删除死成员 `apiBaseUrl_` 或真正用起来。
- **删除 `simulateAnswer` 测试后门**（✅ 已拍板：删除）：E2E 全面改注入 FakeTransport（离屏进程注入简单），
  删除 `SearchPage::simulateAnswer` 与 ui_smoke 对应调用点。

### 4.2 业务规则下沉引擎层
| 现状（UI 层） | 下沉后 | 测试 |
|------|------|------|
| 聚合检测 15 短语表（search_page.cpp:401） | `Retriever::isAggregateQuery(query)` | 单测逐短语 + 反例 |
| per-doc 去重（search_page.cpp:418） | `Retriever::searchAggregate(query, width, perDocLimit)` | 单测限流断言 |
| 法院四级字符串匹配（search_page.cpp:769） | `document::courtLevelOf(DocMetadata)` → 枚举 | 单测覆盖四级边界（含「基层」反例） |
| 元数据摘要两份重复拼装（search_page.cpp:434 与 :856） | `Retriever::metadataSummary(docIds)` 一份 | 单测往返 |
| SearchPage 自建引擎分支（search_page.cpp:57） | 构造必须注入（测试方自持实例注入） | 编译期消除分支 |

- **验收**：grep 证明 UI 层无业务常量表/短语表；eval_retrieval 四路指标与 P1 后一致（算法未动）。

### 4.3 单测新增（FakeTransport 驱动，可离线跑）
SSE 解析（CRLF / 无尾换行 / 分块跨行 / `[DONE]` / error 事件）、embedBatch 乱序重排、
200+错误体报错、超时路径、法律/通用 Prompt 切换。

---

## 5. P3 配置合一与工程卫生（1–1.5 天，可穿插）

### 5.1 配置系统合一
- `AppSettings::defaults()` 成为**唯一事实来源**；`app_config.h` 仅保留仍被引用的
  `HTTP_TIMEOUT / INDEX_FILE / HISTORY_DB / SETTINGS_FILE` 四项（其余 9 个死常量删除）。
- `Generator` 端点/模型可配置：设置页新增「生成服务」卡片（模型名默认 deepseek-chat），
  与 Embedding 卡片并列——**Key 分家之后把服务分家补齐**。
- `AppSettings::load` 数值范围校验：`k1>0`、`0≤b≤1`、两权重非负且重归一化、`topK≥1`、
  `chunkOverlap<chunkSize`、`0≤temperature≤2`——load 路径与 UI 保存路径同一套钳制
  （修 main_window.cpp:273 直传不钳制的不一致）。
- **数据文件位置**（✅ 已拍板：exe 所在目录）：CWD 相对路径 → **exe 所在目录**
  （`QCoreApplication::applicationDirPath()`，便携演示友好，数据随文件夹一起拷走）；
  首启动检测旧路径有数据文件则自动迁移并提示。`run.bat` 同步更新。
- 同一 JSON 双 load 收敛：SettingsPage 表单回填改为接收 MainWindow 注入的 `AppSettings`。

### 5.2 工程卫生
- **CMake**：`SRC_ENGINE/SRC_STORE/SRC_CONFIG` 三个变量在全部 5 个 target 统一复用
  （消灭 5 份手抄列表）；`HEADERS` 补入 `metadata.h / pdf_extractor.h / eval_metrics.h / golden_queries.h`。
- **UI 工具函数收敛**：`setButtonRole`（5 份重复）→ `app_theme.h`；`makeCard`（2 份近似）→ 同处。
- **BM25 性能**：`avgDocLength()` 提出内层循环（bm25_ranker.cpp:35，O(命中×文档) → O(命中)）。
- **`buildContext` UTF-8 安全截断**（retriever.cpp:457）：回退到合法 UTF-8 边界再截。
- **tokenizer 单字过滤**（✅ 已拍板：只改注释）：当前注释与实现不符（单汉字 3 字节绕过 `size()<3` 过滤）。
  本期**只把注释改为如实描述现状，不改行为**——检索指标与论文数字零风险。
  论文如需「停用词优化」叙事，作为远期独立任务另评（需重跑 eval_retrieval 对照）。
- `chunk.startPos` 语义修正或删除该字段（当前无消费者，埋雷字段）。
- `test/data/_quarantine/` 处置：确认后删除（防 21 篇断言被副本文件破坏的事故复发，
  ui_smoke 的语料断言改为「≥21 且含副本检测警告」）。

### 5.3 文档对齐现实
README 大修：Qt 5.15.2 实际构建环境、71 个测试、五页功能现状、
项目结构图更新（config/history/net 目录）、删除失效引用（PITFALLS.md 等）；
TROUBLESHOOTING.md 补「词典缺失」「GBK 导入」「数据文件位置」三节。

---

## 6. 回归防线（贯穿各期，每期结束必跑）

```
1. mingw32-make -j8                     # 0 警告 0 错误
2. build\run_tests.exe                  # 71 + 新增全绿
3. build\test_utf8_location_regression.exe
4. build\ui_smoke.exe -platform offscreen out\smoke_<期> 1x --e2e test/data/legal_cases
5. build\eval_retrieval.exe test\data\legal_cases > docs\eval_results_<期>.txt   # 指标对照留档
```

- 每期一个独立分支（或至少一批原子 commit），可独立 revert；
- eval 指标任何一期出现非预期漂移 → 定位到具体变更并二选一：回退变更 / 论文更新数字。

---

## 7. 里程碑与工作量

| 里程碑 | 内容 | 预估 | 累计 |
|--------|------|------|------|
| M1 | P0 止血包 + 验收 | 0.5–1 天 | 1 天 |
| M2 | P1 线程模型（worker + 锁 + 取消 + 退出安全） | 2.5–3.5 天 | 4 天 |
| M3 | P2 transport 注入 + 业务下沉 + 后门移除 | 2 天 | 6 天 |
| M4 | P3 配置合一 + 卫生 + README | 1–1.5 天 | 7.5 天 |

（兼职/穿插节奏按 1.5 倍估：约 10–11 个自然日；远早于答辩窗口。）

---

## 8. 明确不做（Non-goals）

- 不重写 PDF 提取引擎、不引入商业 OCR SDK（Python 脚本方案维持，仅把探测移线程）；
- 不引入 gtest/QTest 等测试框架（自写断言维持一致性）；
- 不做多进程/分布式/持久化向量库（Faiss 等）；
- 不改 UI 视觉与 QSS 主题（只接线 statusDotOff 死代码）；
- 不做 PDF 扫描件批量 OCR 队列（单文件交互式维持）。

---

## 9. 决策记录（✅ 已全部拍板，2026-10-06）

| # | 决策 | 结论 | 备注 |
|---|------|------|------|
| ① | GBK 文件处理 | **自动转码**（GB18030 → UTF-8） | 仍失败才拒绝导入并报错 |
| ② | tokenizer 单字过滤 | **只改注释，不改行为** | 检索指标与论文数字零风险；真过滤留作远期独立任务 |
| ③ | 数据文件位置 | **exe 所在目录** | 首启动自动迁移旧 CWD 下的数据文件并提示 |
| ④ | simulateAnswer 后门 | **删除** | P2 落地 FakeTransport 后，E2E 全面走真实生成路径 |
| ⑤ | P1 线程方案 | **单引擎工作线程 + 短临界区粗锁**（本方案第 3 节） | 细节：锁不整段持有，UI 轻只读最多等毫秒级 |

**当前状态：方案定稿，未开工。** 开工时按 P0 → P1 → P2 → P3 顺序执行，每期结束回报验收线结果
（构建 0 警告 → 71+ 单测 → ui_smoke --e2e → eval_retrieval 指标对照留档）。
