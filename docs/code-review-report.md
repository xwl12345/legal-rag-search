# legal-rag-search 全面代码审查报告

- **审查对象**：`D:\VS_Project\legal-rag-search`（C++17 + Qt 5.15.2 Widgets，MinGW 8.1 构建）
- **审查日期**：2026-10-13
- **审查方式**：全量源码阅读（src/ 29 个 cpp、include/ 32 个 h）+ 构建/回归/E2E/评测实测
- **问题分级**：P0 阻断（崩溃/数据损坏/功能不可用）→ P1 严重（功能错误或资源泄漏，有明确触发路径）→ P2 中等（边界异常、维护性风险）→ P3 轻微（代码卫生）

---

## 一、功能与端到端验证结论

| 验证项 | 命令/方式 | 结果 |
|---|---|---|
| 增量构建 | CMake + MinGW 全 target | ✅ 通过 |
| 单元/回归测试 | `./build/run_tests.exe` | ✅ **100/100 全绿** |
| UI 冒烟 + E2E | `./build/ui_smoke.exe -platform offscreen docs/screenshots 1x --e2e test/data/legal_cases` | ✅ 七组全过（检索/文档库/持久化/历史/设置/质量分析/法条速查） |
| UTF-8 分块边界回归 | `./build/test_utf8_location_regression.exe` | ✅ 通过 |
| 检索质量评测 | `./build/eval_retrieval.exe`（无 Embedding Key） | ✅ 跑通，自动降级纯 BM25：P@5=0.200 / Hit@5=0.957 / R@10=0.957 / MRR=0.913，与基线一致 |

**结论：端到端流程通畅。** 唯一外部依赖为 Embedding/LLM API Key；无 Key 时降级行为符合设计语义（VectorOnly 返回空不伪装结果，加权/RRF 融合降级为纯 BM25）。

---

## 二、问题清单（按严重程度）

### P0（阻断性）：无

未发现崩溃、数据损坏或功能阻断级缺陷。索引 CRC32 校验、QSaveFile 原子写、配置 sanitize 兜底、LIKE 通配符转义等关键防护均在位且有效。

### P1（严重）：2 项——均为 LawPage 结果列表 payload 生命周期管理遗漏

**P1-1 结果模式下再次搜索泄漏 LawArticle 对象**

- **位置**：`src/ui/law_page.cpp:368`（`LawPage::onSearch` 有命中路径）
- **触发条件**：法条速查页输入关键词搜索并命中后，**不退出结果模式**，直接输入新关键词再次搜索。`resultList_->clear()`（368 行）销毁 QListWidgetItem 时，不会删除 item 的 `Qt::UserRole` 上挂接的 `new LawArticle` 堆对象。
- **证据**：382 行注释自证意图——「释放结果列表各 item 挂载的条文对象（**onSearch 重建前** / 退出结果模式时）」，但 `clearResultPayloads` 实际仅在 `exitResultMode()`（468 行）内调用；无命中路径（324/335 行）会先 `exitResultMode()` 故安全，**有命中路径漏掉了这一步**。
- **影响**：每次重复搜索泄漏 ≤20 个 LawArticle（上限为检索宽度）。单对象几百字节，增长缓慢、不崩溃（进程退出由 OS 回收），但属运行期可持续累积的真实泄漏，非设计局限。
- **修复建议**：368 行 `resultList_->clear();` 之前插入一行 `clearResultPayloads(resultList_);`。

**P1-2 LawPage 析构不释放结果 payload**

- **位置**：`src/ui/law_page.cpp`（无自定义析构函数，`clearResultPayloads` 只在 468 行被调用）
- **触发条件**：`resultMode_` 为真时 LawPage 对象被销毁。当前应用中页面由 `MainWindow::buildPages()` 创建并持有整个进程生命周期，实际仅在进程退出时触发，**当前无实际内存损失**；但属析构不完整。
- **影响**：当前架构下无感；一旦将来页面改为可销毁/重建（或单测反复构造 LawPage），泄漏即显现。
- **修复建议**：添加 `~LawPage() { clearResultPayloads(resultList_); }`（与 P1-1 一并修复后两者相互兜底）。

### P2（中等）：3 项

**P2-1 PDF 文本提取不支持 PDF 1.5+ 对象流**

- **位置**：`src/document/pdf_extractor.cpp`（自研 BT/ET/Tj/TJ 文本层提取）
- **触发条件**：导入 Acrobat 6.0 以后生成的、使用 CrossRef Stream / 对象流（ObjStm）压缩的 PDF。
- **影响**：文本层可能提取为空，静默回退 OCR 通道（慢、且依赖本机 Python + OCR 环境可用）。功能降级而非失败。
- **说明**：头文件已注明该局限，属已知功能边界；此处列出以提醒答辩/文档口径中如实说明。
- **改进建议**：在 README/论文中明示支持范围；后续如需覆盖，可增加对象流解压（FlateDecode 已具备，ObjStm 解析约百余行）。

**P2-2 SearchPage 筛选用 `currentText()` 字面量匹配，与 itemData 方式不一致**

- **位置**：`src/ui/search_page.cpp:871-873`（读取）、`885-886`（与 `「全部」` 比较）
- **触发条件**：修改案件类型/法院级别/年份下拉项的显示文案（如「全部」改「不限」、后续做界面翻译）。
- **影响**：筛选条件静默失效——用户选了具体筛选项却按「全部」处理，且无任何报错。第四维结果倾向筛选（876-879 行）已正确使用 `currentData()` 语义值，同一函数内两种范式并存。
- **改进建议**：三个下拉统一改 `addItem(文案, QVariant::fromValue(枚举/哨兵值))` + `currentData()` 判断，与 tendencyFilter 对齐。

**P2-3 `Retriever::docCount()` 语义歧义（返回块数而非文档数）**

- **位置**：`include/rag/retriever.h:169-170`，`src/rag/retriever.cpp`
- **触发条件**：任何调用方把 `docCount()` 当作文档数使用（头文件 169 行已有注释警告，但接口名本身仍在诱导误用；`totalDocs_` 在 `InvertedIndex` 中按块累加，docCount 实际返回 118 而非 21）。
- **影响**：UI 显示「文档数」时若误用会展示块数。项目已有 `documentCount()` / `chunkCount()` 正确接口。
- **改进建议**：将 `docCount()` 标记废弃（`[[deprecated]]`）或直接删除，全项目调用点改 `documentCount()` / `chunkCount()`。

### P3（轻微）：2 项

**P3-1 死代码**

- **位置**：`src/ui/history_page.cpp:367`，`const auto pathStr = QObject::tr("导出失败");` 声明后从未使用。
- **改进建议**：删除该行。

**P3-2 `extractDate` 正则逐匹配的性能开销**

- **位置**：`src/document/metadata.cpp`（`std::regex` 对多个日期模式依次匹配，已 try/catch 包裹）
- **影响**：非阻塞；单篇文书耗时在毫秒级，仅超长文书上有可感知开销。
- **改进建议**：可先用简单字符串扫描定位日期区段再精匹配；优先级低。

---

## 三、架构评估

**整体评价：合理，符合桌面端 RAG 系统的最佳实践，无需动架构。**

| 维度 | 结论 | 依据 |
|---|---|---|
| 模块划分 | ✅ 清晰 | 九层单向依赖：ui → rag → {index, vector, document}；history/law/config/net 为独立横切模块，无反向依赖 |
| 组合根 | ✅ 唯一 | `MainWindow::buildPages()` 是 Retriever / EngineWorker / HistoryStore / LawStore / lawRetriever_ 的唯一创建点，裸指针注入各页；页面内不存在 `new Retriever`（唯一例外 SearchPage 缺省参数自建，仅测试用） |
| 线程模型 | ✅ 健全 | EngineWorker 独占 QThread，重操作队列化；Retriever 内部 `std::mutex` 短临界区 + `vectorEpoch_` 代次兜底防重入下标错位；embed/generate 的网络等待（QEventLoop）绝不持锁 |
| 持久化 | ✅ 可靠 | 自定义二进制索引 magic+version+count+records+CRC32，小端序，`kMaxStringBytes=512MB` 防巨额分配；QSaveFile 原子替换；v2/v3 版本兼容 |
| 可扩展性 | ✅ 良好 | `IHttpTransport` 可注入（测试用 FakeTransport）；`SearchMode` 四路枚举 + `searchWithMode()` 单入口；配置热更新走 setter，明确禁止销毁重建 Retriever |
| 法条子库 | ✅ 隔离彻底 | 独立 `LawStore` + 独立 `Retriever` 实例（构造保证主索引零漂移），docId = `法律名#条号` 命名空间隔离 |

**少量改进点**（非缺陷）：
- `SearchPage`（1069 行）与 `Retriever`（1004 行）单文件偏重；SearchPage 可考虑将「流式回答 + 历史落库」拆为独立 presenter/controller，进一步减 UI 职责。
- `src/ui/` 下各页体量增长后，可将 `engine_worker` 的重操作槽按域拆分（search / import / generate），当前尚可维护。

---

## 四、解耦分析

**结论：解耦程度高，无循环依赖，无职责不清。**

1. **跨页零互引**（grep 验证）：五个内容页互不 `#include`、互不持有指针，全部经 MainWindow 信号槽中转（engineStatsChanged / apiKeyStateChanged / answerFinished / libraryChanged / settingsChanged / answerRecorded / forwardEngineBusy）。符合全局约定第 8 条。
2. **单向只读调用**：页面只对 Retriever / Store 做调用，没有任何页面反向注册进引擎或存储。
3. **无循环依赖**：include 关系图检查通过，`rag` 层不依赖 `ui`，`document` 层不依赖 `rag`。
4. **存储隔离**：HistoryStore / LawStore 各自使用具名 SQLite 连接（`history_conn_%1` / `law_conn_%1`），互不串味；law 存储模块只读。
5. **配置边界清晰**：Key 覆盖分家（检索页 Key 只给 LLM，Embedding Key 归设置页），`AppSettings::sanitize()` 统一兜底，load 失败回落默认值。

---

## 五、修复优先级建议

| 顺序 | 问题 | 工作量 | 预期效果 |
|---|---|---|---|
| 1 | P1-1 + P1-2 LawPage payload 泄漏 | 2 行代码 | 消除唯一真实泄漏，与既有注释意图对齐 |
| 2 | P2-2 筛选改 itemData | 半小时 | 消除静默失效类维护陷阱 |
| 3 | P2-3 docCount 废弃 | 半小时（含调用点） | 消除命名歧义 |
| 4 | P3-1 死代码删除 | 1 分钟 | 卫生 |
| 5 | P2-1 PDF 对象流 | 百余行/后续版本 | 扩大 PDF 兼容面（可选） |

---

## 六、总结

项目功能全部跑通（构建/100 项回归/E2E 七组/UTF-8 回归/检索评测），端到端通畅；架构与解耦质量高于一般本科毕业设计水准。共发现问题 7 项：P0 无、P1 两项（同一根因：LawPage 结果列表 payload 释放遗漏，修复共 2 行）、P2 三项、P3 两项。**建议按第五节顺序处理 P1 后即可收尾。**
