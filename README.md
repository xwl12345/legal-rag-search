# ⚖️ Legal RAG Search — 法律文档智能检索引擎

基于 **BM25 + 向量语义** 混合检索的法律文档智能检索引擎（C++17 / Qt 5.15.2 桌面应用），集成大模型流式问答实现 RAG（Retrieval-Augmented Generation）。支持 PDF/TXT/MD 文档导入（含扫描件 OCR）、元数据自动提取、索引持久化、会话历史落库与检索质量对比分析。

> 核心检索链路（分词、倒排索引、BM25、向量相似度、RRF 融合、PDF 解析）全部手写实现，零重型第三方检索依赖。

## 核心特性

- 🔍 **混合检索** — BM25 关键词匹配 + 余弦相似度向量语义检索，加权融合排序（权重可配置）
- 🧪 **四路对比** — BM25 单路 / 向量单路 / 加权融合 / RRF（倒数排名）融合四种检索模式并列，支撑检索质量分析
- 💾 **索引持久化** — `rag_index.dat` 落盘（magic + version + CRC32 校验，QSaveFile 原子写），重启自动恢复，毫秒级
- 📚 **文档库管理** — 文档级列表、四维筛选、单篇删除（倒排/块/元数据/全文同步清理）
- 🕘 **问答历史** — Qt SQLite 落库（含命中来源、中断状态标记），关键词搜索、删除、导出 Markdown
- 📈 **检索质量分析** — 23 条标注查询批量跑四路，输出 Hit@5 / R@10 / MRR 对比指标（维护者可靠性看板）
- ⚙️ **检索参数配置中心** — k1/b、融合权重、TopK、分块参数、生成温度、Embedding/生成服务地址与模型，全部界面可配、运行时热更新、范围钳制
- 📄 **PDF 文本提取 + 扫描件 OCR** — 轻量 PDF 解析引擎（FlateDecode / ASCII85Decode），文本型 PDF 直接提取，扫描件调起本地 OCR
- ⚖️ **法律分词词典** — 250+ 法律专业术语自定义词典，精准识别裁判文书用语
- 🏷️ **元数据自动提取** — 案号、审理法院、裁判日期、案件类型、当事人、审判程序、裁判结果倾向
- 🤖 **流式 AI 回答** — SSE 流式生成，法律结构化四段式 Prompt（案件概述 → 法律分析 → 结论 → 参考来源）
- 🎨 **现代桌面 UI** — 藏青 + 铜金主题，五页导航全部实现（检索问答 / 文档库 / 问答历史 / 检索质量分析 / 设置），Ctrl+滚轮整体缩放

## 技术架构

```
┌──────────────────────────────────────────────────────────┐
│                     Qt 5.15.2 Desktop UI                  │
│   检索问答 / 文档库 / 问答历史 / 检索质量分析 / 设置 五页      │
└────────────────────────┬─────────────────────────────────┘
                         │ 信号槽（跨页联动经 MainWindow 中转）
┌────────────────────────▼─────────────────────────────────┐
│              检索引擎 Worker 线程 (EngineWorker)             │
│                    RAG 检索器 (Retriever)                   │
│       ┌──────────────┐  ┌──────────────┐                  │
│       │  BM25 排序器   │  │ 向量相似度引擎 │                  │
│       └──────┬───────┘  └──────┬───────┘                  │
│       ┌──────▼───────┐  ┌──────▼─────────┐                │
│       │  倒排索引      │  │ Embedding 服务  │──┐             │
│       └──────────────┘  └────────────────┘  │             │
│       ┌─────────────────────────────┐       │             │
│       │ 中文分词 (cppjieba + 法律词典) │       │             │
│       └─────────────────────────────┘       │             │
└─────────────────────────────────────────────┼─────────────┘
                                              │ IHttpTransport 抽象
                    ┌─────────────────────────▼────────────┐
                    │  Embedding：硅基流动 (BAAI/bge-large)  │
                    │  生成：DeepSeek Chat（端点均可配置）     │
                    └──────────────────────────────────────┘
```

### 检索流程

1. **文档导入** → 格式识别（TXT/MD/PDF，扫描件走 OCR）+ 文本分块（512 字符/块，50 字符重叠，可配置）→ 元数据提取 → 建倒排索引。**导入不调用任何网络 API**
2. **向量懒计算** → 首次带 Key 的检索时，按当前配置批量嵌入文本块并缓存向量库（改 Key/模型自动重算）
3. **检索** → 按所选模式跑 BM25 / 向量 / 加权融合 / RRF 融合，阈值过滤后排序
4. **筛选** → 客户端本地按案件类型 / 法院级别 / 年份过滤
5. **生成** → 拼接检索上下文（UTF-8 安全截断）→ 法律/通用双 Prompt → SSE 流式生成；无 LLM Key 时检索结果照常展示
6. **落库** → 回答完成自动写入问答历史（含命中来源与中断状态）

## 项目结构

```
legal-rag-search/
├── include/                          # 头文件（与 src/ 一一对应）
│   ├── config/
│   │   ├── app_config.h              # 少量常量 + dataFilePath()（数据文件 → exe 目录）
│   │   └── app_settings.h            # AppSettings：defaults() 唯一事实源 + 范围钳制
│   │                                 # （实现在 src/config/app_settings.cpp、data_paths.cpp）
│   ├── document/                     # parser / tokenizer / pdf_extractor / ocr_client / metadata
│   ├── history/
│   │   ├── history_record.h          # 问答记录结构（含命中来源、中断标记）
│   │   └── history_store.h           # Qt SQLite 会话历史存储
│   ├── index/                        # inverted_index / bm25_ranker / index_store（持久化）
│   ├── net/
│   │   └── http_transport.h          # IHttpTransport 网络传输抽象（可注入 Fake 离线测试）
│   ├── rag/                          # retriever（四路混合检索）/ generator（SSE 生成）/ eval_metrics / golden_queries
│   ├── ui/                           # main_window / navigation_bar / search_page / library_page /
│   │                                 #   history_page / quality_page / settings_page / app_theme
│   └── vector/                       # embedding / similarity
├── src/                              # 实现文件（模块结构与 include/ 相同）
│   ├── config/data_paths.cpp         # 数据路径解析 + 一次性迁移（互斥保护）
│   └── ui/engine_worker.cpp          # 引擎线程封装（P1 线程模型：导入/检索异步、可取消、退出安全）
├── third_party/                      # cppjieba + limonp + nlohmann/json（MIT，随仓库分发）
├── test/
│   ├── test_main.cpp                 # 93 个单元/集成测试用例
│   ├── ui_smoke.cpp                  # UI 冒烟 + 五组 E2E（离屏渲染，可无 Key 运行）
│   └── data/legal_cases/             # 21 篇模拟裁判文书（5 大案件类型）
├── docs/                             # optimization-plan.md（四期优化方案）、screenshots/、defense/
├── CMakeLists.txt                    # C++17 / Qt 5.15.2 / MinGW
└── run.bat.example                   # 启动脚本模板（复制为 run.bat 使用）
```

## 环境要求

| 依赖 | 版本 | 说明 |
|------|------|------|
| **Qt** | 5.15.2 (MinGW 8.1.0) | Widgets + Core + Network + Sql 模块 |
| **CMake** | ≥ 3.16 | 构建工具 |
| **编译器** | MinGW GCC 8.1.0 | C++17 |
| **操作系统** | Windows 10/11 | 当前仅 Win32 构建 |
| **Embedding Key** | 硅基流动等 OpenAI 兼容服务 | 向量语义检索（可选，无 Key 自动降级纯 BM25） |
| **LLM Key** | DeepSeek 等兼容服务 | AI 流式回答（可选） |

## 快速开始

### 1. 安装 Qt 5.15.2

从 [Qt 官网](https://www.qt.io/download) 安装 Qt 5.15.2，勾选 **MinGW 8.1.0 64-bit** 与 **MinGW 8.1.0 工具链**组件。

### 2. 克隆与编译

```bash
git clone <repo-url> legal-rag-search
cd legal-rag-search
cmake -B build -G "MinGW Makefiles" ^
  -DCMAKE_PREFIX_PATH="D:/Qt/5.15.2/mingw81_64" ^
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw810_64/bin/g++.exe" ^
  -DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/mingw810_64/bin/mingw32-make.exe"
cmake --build build -j 8
```

构建脚本会自动把 Qt 运行时 DLL（含 sqldrivers/qsqlite.dll）部署到 exe 同级目录，无需手动 windeployqt。

### 3. 运行测试

```bash
build\run_tests.exe        # 预期 93/93 全绿
```

UI 冒烟 + 端到端（离屏渲染，不弹窗）：

```bash
set QT_QPA_FONTDIR=C:\Windows\Fonts
build\ui_smoke.exe -platform offscreen docs/screenshots 1x --e2e test/data/legal_cases
```

### 4. 配置 Key（两把 Key 分家）

| 服务 | 用途 | 配置入口 |
|------|------|----------|
| **Embedding** | 向量语义检索 | **设置页**「Embedding 服务」卡片（服务地址 / 模型名 / API Key） |
| **LLM** | AI 流式回答 | **检索问答页**顶部 Key 输入框，或环境变量 `DEEPSEEK_API_KEY` |

LLM 生成服务的端点与模型名也可在设置页「生成服务」卡片调整（默认 DeepSeek `deepseek-chat`）。两把 Key 相互独立：只有 Embedding Key 时纯检索可用（BM25+向量），只有 LLM Key 时退化为纯 BM25 检索 + AI 回答。

### 5. 启动

```bash
copy run.bat.example run.bat   # 首次：复制模板并填入 LLM Key
run.bat
```

## 使用指南

五页导航（Ctrl+滚轮缩放全局字号）：

| 页面 | 功能 |
|------|------|
| **🔍 检索问答** | 导入文档、混合检索、三维筛选、流式 AI 回答、LLM Key 配置 |
| **📚 文档库** | 文档列表（含裁判结果倾向列）、四维筛选、单篇删除（同步清理索引/块/全文） |
| **🕘 问答历史** | 历史列表与详情、关键词搜索、删除/清空、导出 Markdown（含命中来源表） |
| **📈 检索质量分析** | 输入查询并列跑四路（BM25/向量/加权/RRF）对比，23 条标注查询批量指标 |
| **⚙️ 设置** | 检索参数（k1/b/权重/TopK/分块）、Embedding 与生成服务配置、恢复默认 |

检索结果条目显示融合相关度与分项（BM25 / 向量），设置页修改参数**运行时热更新**，无需重启或重建索引。

## 配置说明

检索参数与 AI 服务配置在**设置页**修改，持久化到数据目录的 `rag_settings.json`：

- `AppSettings::defaults()` 是唯一事实源，配置文件缺失/字段缺失/值损坏时逐项回落默认值；
- 所有数值经范围钳制（k1>0、0≤b≤1、权重非负且自动重归一化、TopK≥1、chunkOverlap<chunkSize、0≤temperature≤2）；
- 修改**查询期参数**（k1/b/权重/TopK）即时生效；**分块参数**只对之后导入的文档生效。

## 数据文件位置

P3 起三件数据文件存放在 **exe 所在目录**（便携设计，整个文件夹拷走即带走全部数据）：

| 文件 | 内容 |
|------|------|
| `rag_index.dat` | 倒排索引 + 分块 + 元数据 + 全文（CRC32 校验，原子写） |
| `rag_history.db` | 问答历史（SQLite） |
| `rag_settings.json` | 检索参数与 AI 服务配置 |

首次启动若在工作目录检测到旧版本数据文件，会**自动迁移**到 exe 目录并打日志提示；迁移失败则沿用旧文件继续运行。上述文件均不参与版本控制。

## 功能矩阵

| 功能 | Embedding Key | 无任何 Key |
|------|:---:|:---:|
| 文档导入（PDF/TXT/MD）+ 分词 + 元数据 | ✅ | ✅ |
| BM25 关键词检索 + 筛选 | ✅ | ✅ |
| 向量语义检索 / 加权融合 | ✅ | 降级纯 BM25 |
| 四路对比 + 质量指标 | ✅ | BM25 路可用 |
| 索引持久化 / 文档库 / 问答历史 | ✅ | ✅ |
| AI 流式回答 | 需 LLM Key | ❌ |

降级是**静默且如实**的：向量路不可用时检索照常返回 BM25 结果，状态栏与结果分数明确区分。

## Demo 数据集

项目内置 21 篇模拟中国裁判文书（`test/data/legal_cases/`），覆盖 5 大案件类型：

| 类型 | 数量 | 示例 |
|------|------|------|
| 民事 | 8 篇 | 借贷纠纷、合同纠纷、侵权纠纷、离婚、继承、劳动、房产、交通事故 |
| 刑事 | 5 篇 | 诈骗、盗窃、故意伤害、职务侵占、危险驾驶 |
| 行政 | 3 篇 | 行政许可、行政处罚、行政赔偿 |
| 知识产权 | 3 篇 | 商标侵权、专利纠纷、著作权纠纷 |
| 商事 | 2 篇 | 公司纠纷、破产清算 |

每篇包含标准裁判文书要素：标题、案号、法院、当事人、案由、事实、说理、判决、日期、合议庭成员。

## 法律词典

自定义词典位于 `third_party/cppjieba/dict/legal_dict.utf8`，包含 250+ 法律专业术语，分 10 大类（诉讼程序 / 法院机关 / 实体法 / 刑法 / 合同法 / 公司法 / 知识产权 / 劳动法 / 行政法 / 证据）。

## 常见问题

### Q: 启动时提示「找不到 Qt5Widgets.dll」？

A: 使用仓库提供的 `run.bat` 启动（CMake 构建时已自动部署 Qt DLL 到 exe 目录）；手动运行请将 `Qt/5.15.2/mingw81_64/bin` 加入 PATH。

### Q: 分词异常或启动即崩？

A: 确认 `third_party/cppjieba/dict/` 词典齐全（`jieba.dict.utf8`、`hmm_model.utf8`、`user.dict.utf8`、`idf.utf8`、`stop_words.utf8`、`legal_dict.utf8`）。详见 TROUBLESHOOTING.md「词典缺失」节。

### Q: TXT 导入乱码？

A: 引擎支持 UTF-8 与 GBK 自动检测；个别编码异常文件见 TROUBLESHOOTING.md「GBK 导入」节。

### Q: 数据文件去哪了？

A: 一律在 **exe 所在目录**（通常为 `build/`）。旧版本存在工作目录的文件会在首次启动时自动迁移。详见 TROUBLESHOOTING.md「数据文件位置」节。

### Q: PDF 导入后文本为空？

A: 仅支持**文本型 PDF**，不支持扫描版/加密 PDF（扫描件可走 OCR 路径）。

## 依赖许可

| 库 | 许可 | 用途 |
|----|------|------|
| [Qt 5.15.2](https://www.qt.io) | LGPLv3 / Commercial | GUI + 网络 + SQLite |
| [cppjieba](https://github.com/yanyiwu/cppjieba) | MIT | 中文分词 |
| [limonp](https://github.com/yanyiwu/limonp) | MIT | cppjieba 依赖 |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON 解析 |
| [zlib](https://www.zlib.net/) | zlib License | PDF 流解压（MinGW 自带） |
| DeepSeek API / 硅基流动 | 商业 API | LLM 生成 / Embedding（均为可选外部服务） |

## License

本项目为毕业设计作品，仅供学习和研究使用。
