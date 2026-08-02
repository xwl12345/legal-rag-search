# ⚖️ 法律 RAG 智能检索引擎

基于 **BM25 + 向量语义** 混合检索的法律文档智能搜索引擎，集成 **DeepSeek 大模型** 实现 RAG（Retrieval-Augmented Generation）智能问答。支持 PDF/TXT/MD 等多格式文档导入，自动提取案号、法院、裁判日期等元数据，提供结构化法律 Prompt 模板。

## 核心特性

- 🔍 **混合检索** — BM25 关键词匹配 + 余弦相似度向量语义检索，加权融合排序（BM25 0.4 / 向量 0.6）
- 📄 **PDF 文本提取** — 轻量级 PDF 解析引擎，支持 FlateDecode / ASCII85Decode 压缩流，零外部 PDF 库依赖
- ⚖️ **法律分词词典** — 250+ 法律专业术语自定义词典（涵盖刑法、民法、商法、知识产权法等 10 大类别），精准识别裁判文书用语
- 🏷️ **元数据自动提取** — 从裁判文书中自动提取案号、审理法院、裁判日期、案件类型、当事人、审判程序
- 🔎 **UI 筛选栏** — 按案件类型（民事/刑事/行政/知识产权/商事）、法院级别（最高/高级/中级/基层）、年份快速过滤检索结果
- 📋 **法律 Prompt 模板** — 自动检测法律上下文，切换结构化四段式法律问答格式（案件概述 → 法律分析 → 结论 → 参考来源）
- 🤖 **流式 AI 回答** — 基于 DeepSeek Chat API 的 SSE 流式生成，实时逐字展示
- 🇨🇳 **中文分词** — 集成 cppjieba + 法律自定义词典，精准的中文分词和关键词提取
- 🎨 **现代桌面 UI** — 基于 Qt6，左右分栏（检索结果 + AI 回答），支持 API Key 界面配置
- 🧪 **完整测试** — 43 个单元测试 + 端到端测试，21 篇模拟裁判文书 Demo 数据集

## 技术架构

```
┌──────────────────────────────────────────────────────────┐
│                      Qt6 Desktop UI                       │
│    (搜索栏 / 筛选下拉框 / API Key 配置 / 结果列表 / AI 回答)   │
└────────────────────────┬─────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────┐
│                    RAG 检索引擎 (Retriever)                 │
│       ┌──────────────┐  ┌──────────────┐                  │
│       │  BM25 排序器   │  │ 向量相似度引擎 │                  │
│       │  (关键词匹配)  │  │  (语义检索)   │                  │
│       └──────┬───────┘  └──────┬───────┘                  │
│              │                 │                           │
│       ┌──────▼───────┐  ┌──────▼───────┐                  │
│       │  倒排索引      │  │ Embedding API │                 │
│       └──────┬───────┘  └──────┬───────┘                  │
│              │                 │                           │
│       ┌──────▼───────────────▼──────────┐                  │
│       │  文档元数据提取 (案号/法院/日期等)  │                  │
│       └──────────────┬─────────────────┘                  │
│                      │                                    │
│       ┌──────────────▼──────────────┐                     │
│       │  中文分词 (cppjieba + 法律词典) │                    │
│       └─────────────────────────────┘                     │
└──────────────────────────────────────────────────────────┘
                         │
┌────────────────────────▼─────────────────────────────────┐
│                AI 生成器 (Generator)                       │
│   法律 Prompt 模板 / SSE 流式解析 / DeepSeek Chat API       │
└──────────────────────────────────────────────────────────┘
```

### 检索流程

1. **文档导入** → 格式识别（TXT/MD/PDF）+ 文本分块（512 字符/块，50 字符重叠）
2. **元数据提取** → 案号、法院、日期、案件类型、当事人、审判程序
3. **分词** → cppjieba + 250+ 法律术语词典 → 去停用词
4. **建索引** → 倒排索引 + Embedding 向量
5. **搜索** → BM25 关键词检索 + 余弦相似度向量检索 → 加权融合排序 → 超过阈值（BM25 0.15 + 向量 0.25）
6. **筛选** → 客户端本地按案件类型 / 法院级别 / 年份过滤
7. **生成** → 自动检测法律上下文 → 拼接元数据摘要 + 检索上下文 → 法律/通用双 Prompt 模板 → DeepSeek 流式生成

## 项目结构

```
rag-search-engine/
├── include/
│   ├── config/
│   │   └── app_config.h              # 全局配置（检索参数、API 配置）
│   ├── document/
│   │   ├── parser.h                  # 文档解析 + 文本分块
│   │   ├── tokenizer.h               # 中文分词器（Pimpl 封装 cppjieba）
│   │   ├── pdf_extractor.h           # PDF 文本提取器
│   │   └── metadata.h                # 法律文档元数据提取器
│   ├── index/
│   │   ├── inverted_index.h          # 倒排索引数据结构
│   │   └── bm25_ranker.h             # BM25 排序算法
│   ├── vector/
│   │   ├── embedding.h               # 向量嵌入服务（DeepSeek API）
│   │   └── similarity.h              # 余弦相似度计算
│   ├── rag/
│   │   ├── retriever.h               # 混合检索器（BM25 + 向量融合）
│   │   └── generator.h               # AI 答案生成器（SSE 流式 + Prompt 模板）
│   └── ui/
│       └── main_window.h             # Qt 主窗口（含筛选栏）
├── src/                              # 实现文件（与 include/ 一一对应）
│   ├── main.cpp                      # 程序入口
│   ├── document/
│   │   ├── parser.cpp                # TXT/MD 文本解析
│   │   ├── tokenizer.cpp             # 分词器 + 法律词典加载
│   │   ├── pdf_extractor.cpp         # PDF FlateDecode/ASCII85 解码
│   │   └── metadata.cpp              # 案号/法院/日期等提取
│   ├── index/
│   ├── vector/
│   ├── rag/
│   │   ├── retriever.cpp             # 混合检索 + 元数据管理
│   │   └── generator.cpp             # Prompt 模板 + SSE 解析
│   └── ui/
│       └── main_window.cpp           # 主窗口 UI + 筛选逻辑
├── third_party/
│   ├── cppjieba/                     # 中文分词（MIT）含法律自定义词典
│   │   └── dict/
│   │       ├── legal_dict.utf8       # 250+ 法律专业术语
│   │       └── ...                   # 其他标准词典
│   ├── limonp/                       # cppjieba 依赖（MIT）
│   └── nlohmann/                     # JSON 库（MIT）
├── test/
│   ├── test_main.cpp                 # 43 个测试用例
│   └── data/
│       └── legal_cases/              # 21 篇模拟裁判文书（5 种案件类型）
├── CMakeLists.txt                    # CMake 构建配置（C++17 / Qt6）
├── run.bat                           # Windows 一键启动脚本
└── PITFALLS.md                       # 开发踩坑记录
```

## 环境要求

| 依赖 | 版本 | 说明 |
|------|------|------|
| **Qt** | 6.x (MinGW) | Widgets + Core + Network 模块 |
| **CMake** | ≥ 3.16 | 构建工具 |
| **编译器** | GCC 13+ / MSVC 2022 | 需支持 C++17 |
| **操作系统** | Windows 10/11 | 当前仅 Win32 构建 |
| **DeepSeek API Key** | — | 向量检索 + AI 回答（可在界面配置） |

## 快速开始

### 1. 安装 Qt6

从 [Qt 官网](https://www.qt.io/download-qt-installer) 下载安装 Qt 6.x，选择 MinGW 64-bit 组件。

### 2. 克隆项目

```bash
git clone <repo-url> rag-search-engine
cd rag-search-engine
```

第三方库（cppjieba + limonp + nlohmann）已随项目分发，无需额外下载。

### 3. 编译

```bash
mkdir build && cd build

# 配置 CMake（替换为你的 Qt 路径）
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="D:/Qt/6.10.2/mingw_64" \
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe"

# 编译（4 线程并行）
mingw32-make -j4
```

### 4. 运行测试

```bash
# 确保工作目录在项目根（词典路径依赖相对路径）
cd D:\graduation_project\rag-search-engine
build\run_tests.exe
```

### 5. 设置 API Key

**方式一：界面配置（推荐）**
启动程序后在顶部输入框填入 API Key，点击「设置」。

**方式二：环境变量**
```bash
set DEEPSEEK_API_KEY=sk-xxxxxxxxxxxxxxxx
```

> 📌 获取 API Key：访问 [platform.deepseek.com](https://platform.deepseek.com) 注册并创建。

### 6. 启动

```bash
# 方式一：使用启动脚本（自动设置 API Key + 启动）
run.bat

# 方式二：手动启动
set PATH=D:\Qt\6.10.2\mingw_64\bin;%PATH%
build\rag_search_engine.exe
```

## 使用指南

### 导入文档

1. 点击 **「📂 导入文档」** 按钮
2. 支持格式：`.txt` / `.md` / `.pdf` / `.csv` / `.json` / `.xml`
3. 导入时自动提取元数据（案号、法院、日期等）
4. 状态栏显示导入进度和已索引文本块数量

### 搜索问答

1. 在搜索框输入问题（例：「合同纠纷中违约金如何计算？」）
2. 点击 **「🔍 搜索」** 或按 Enter
3. **左侧** 展示检索结果列表（含文档名 + 元数据 + 相关度分数）
4. **右侧** AI 法律助手按结构化格式实时流式作答

### 筛选功能

检索后可快速过滤结果，无需重新搜索：

| 筛选项 | 选项 | 说明 |
|--------|------|------|
| **案件类型** | 全部 / 民事 / 刑事 / 行政 / 知识产权 / 商事 | 按案由分类筛选 |
| **法院级别** | 全部 / 最高人民法院 / 高级人民法院 / 中级人民法院 / 基层人民法院 | 按审理法院层级筛选 |
| **年份** | 全部 / 动态生成 | 仅显示检索结果中存在的年份 |

### 检索结果分数

| 字段 | 权重 | 说明 |
|------|------|------|
| `finalScore` | — | 加权融合最终分数 |
| `bm25Score` | 0.4 | 关键词匹配度 |
| `vectorScore` | 0.6 | 语义相似度 |

### 清空索引

点击 **「🗑 清空索引」** → 确认后清除所有已导入文档。

## 配置说明

编辑 `include/config/app_config.h` 可调整以下参数：

```cpp
// ── AI API ──
constexpr const char* CHAT_MODEL = "deepseek-chat";
constexpr const char* EMBEDDING_MODEL = "text-embedding-3-small";
constexpr int HTTP_TIMEOUT = 30;              // API 请求超时（秒）

// ── 检索 ──
constexpr int DEFAULT_TOP_K = 5;              // 默认返回结果数
constexpr int MAX_CHUNK_SIZE = 512;           // 文本块最大字符数
constexpr int CHUNK_OVERLAP = 50;             // 文本块重叠字符数
constexpr double BM25_WEIGHT = 0.4;           // BM25 权重
constexpr double VECTOR_WEIGHT = 0.6;         // 向量检索权重

// ── 数据库 ──
constexpr const char* DB_PATH = "rag_index.db";
```

修改后需重新编译。

## 功能矩阵

| 功能 | 有 API Key | 无 API Key |
|------|-----------|-----------|
| 文档导入 + 分词 | ✅ | ✅ |
| PDF 文本提取 | ✅ | ✅ |
| 元数据提取（案号/法院等） | ✅ | ✅ |
| BM25 关键词搜索 | ✅ | ✅ |
| 筛选栏（类型/级别/年份） | ✅ | ✅ |
| 向量语义检索 | ✅ | ❌ |
| AI 智能回答 | ✅ | ❌ |

无 API Key 时，仅展示 BM25 关键词检索结果列表，筛选、元数据等功能均正常可用。

## Demo 数据集

项目内置 21 篇模拟中国裁判文书（`test/data/legal_cases/`），覆盖 5 大案件类型：

| 类型 | 数量 | 示例 |
|------|------|------|
| 民事 | 8 篇 | 借贷纠纷、合同纠纷、侵权纠纷、离婚、继承、劳动、房产、交通事故 |
| 刑事 | 5 篇 | 诈骗、盗窃、故意伤害、贪污、危险驾驶 |
| 行政 | 3 篇 | 行政许可、行政处罚、行政赔偿 |
| 知识产权 | 3 篇 | 商标侵权、专利纠纷、著作权纠纷 |
| 商事 | 2 篇 | 公司纠纷、破产清算 |

每篇包含标准裁判文书要素：标题、案号、法院、当事人、案由、事实、说理、判决、日期、合议庭成员。

## 法律词典

自定义词典位于 `third_party/cppjieba/dict/legal_dict.utf8`，包含 250+ 法律专业术语，分 10 大类：

| 类别 | 示例术语 |
|------|----------|
| 诉讼程序 | 管辖权异议、诉讼时效、举证责任、强制执行 |
| 法院机关 | 最高人民法院、中级人民法院、人民法院、合议庭 |
| 实体法 | 违约责任、侵权责任、不当得利、表见代理 |
| 刑法 | 故意伤害、数额巨大、数罪并罚、自首立功 |
| 合同法/民法 | 格式条款、合同解除、善意第三人、不可抗力 |
| 公司法/商法 | 股权转让、法人代表、注册资本、破产清算 |
| 知识产权 | 商标侵权、专利无效、著作权登记、商业秘密 |
| 劳动法 | 劳动合同、经济补偿、工伤认定、竞业限制 |
| 行政法 | 行政复议、行政许可、行政处罚、强制措施 |
| 证据 | 书证物证、鉴定意见、电子数据、证人证言 |

## 开发

### 编译测试

```bash
cd build
mingw32-make run_tests
build\run_tests.exe
```

### 构建 Release 版本

```bash
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="D:/Qt/6.10.2/mingw_64" \
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe" \
  -DCMAKE_BUILD_TYPE=Release
mingw32-make -j4
```

### 技术要点

- **Pimpl 模式** — `Tokenizer` 通过 `Impl` 封装 cppjieba，避免头文件暴露第三方库依赖
- **静态链接** — MinGW 运行时（libstdc++/libgcc/libwinpthread）全部静态链接，消除 DLL 版本冲突
- **UTF-8 字节扫描** — 元数据提取中 Unicode 字符匹配使用手动 UTF-8 字节扫描，规避 GCC `<regex>` 的 Unicode 兼容问题
- **中文数字解析** — 区分位置记数法（「二〇二四」→ 2024）与叠加记数法（「十五」→ 15）
- **SSE 流式解析** — 非完整行缓冲 + `QEventLoop` 同步阻塞，确保流式输出的可靠拼接

## 常见问题

### Q: 启动时提示「找不到 Qt6Widgets.dll」？

A: 将 Qt 的 `bin` 目录加入 `PATH`，或使用 `windeployqt` 部署依赖：
```bash
windeployqt --release build/rag_search_engine.exe
```

### Q: 分词异常或程序崩溃？

A: 确认 `third_party/cppjieba/dict/` 目录包含以下文件：
- `jieba.dict.utf8`、`hmm_model.utf8`、`user.dict.utf8`
- `idf.utf8`、`stop_words.utf8`
- `legal_dict.utf8`（法律自定义词典）

### Q: AI 回答很慢或超时？

A: ① 检查网络是否能访问 `api.deepseek.com`；② 增大 `HTTP_TIMEOUT` 配置；③ 减少导入文档量

### Q: PDF 导入后文本为空或乱码？

A: 本引擎仅支持**文本型 PDF**（可选中文字的），不支持扫描版/图片型 PDF。后者需要 OCR 处理。也不支持加密 PDF。

### Q: 如何添加新的文件格式支持？

A: 在 `src/document/parser.cpp` 的 `parse()` 方法中添加对应格式扩展名判断和解析逻辑。

## 依赖许可

| 库 | 许可 | 用途 |
|----|------|------|
| [Qt 6](https://www.qt.io) | LGPLv3 / GPLv3 / Commercial | GUI + 网络 |
| [cppjieba](https://github.com/yanyiwu/cppjieba) | MIT | 中文分词 |
| [limonp](https://github.com/yanyiwu/limonp) | MIT | cppjieba 依赖 |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON 解析 |
| [zlib](https://www.zlib.net/) | zlib License | PDF 流解压（MinGW 自带） |
| [DeepSeek API](https://platform.deepseek.com) | 商业 API | Embedding + Chat |

## License

本项目为毕业设计作品，仅供学习和研究使用。
