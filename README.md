# 🚀 RAG 智能检索引擎

基于 **BM25 + 向量检索** 的本地混合检索引擎，结合 **DeepSeek AI** 实现 RAG（Retrieval-Augmented Generation）智能问答。

## 核心特性

- 🔍 **混合检索** — BM25 关键词匹配 + 向量语义检索，加权融合排序
- 🤖 **AI 智能回答** — 基于检索到的文档上下文，由 DeepSeek 大模型流式生成答案
- 📂 **多格式支持** — 支持 TXT、MD 等文本格式文档导入
- 🇨🇳 **中文分词** — 集成 cppjieba，精准的中文分词和关键词提取
- 🎨 **现代 UI** — 基于 Qt6 的桌面界面，流式实时展示 AI 回答
- ⚡ **纯本地运行** — 除 AI API 调用外，检索引擎完全本地运行

## 技术架构

```
┌─────────────────────────────────────────────────┐
│                   Qt6 Desktop UI                  │
│              (搜索栏 / 结果列表 / AI 回答)          │
└─────────────────────┬───────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│                  RAG 检索器 (Retriever)            │
│         ┌──────────────┐  ┌──────────────┐       │
│         │  BM25 排序器  │  │ 向量相似度引擎 │       │
│         │  (关键词匹配) │  │  (语义检索)   │       │
│         └──────┬───────┘  └──────┬───────┘       │
│                │                 │                │
│         ┌──────▼───────┐  ┌──────▼───────┐       │
│         │  倒排索引     │  │ Embedding API│       │
│         └──────┬───────┘  └──────┬───────┘       │
│                │                 │                │
│         ┌──────▼───────┐  ┌──────▼───────┐       │
│         │  中文分词     │  │  DeepSeek    │       │
│         │  (cppjieba)  │  │  API (云端)  │       │
│         └──────────────┘  └──────────────┘       │
└─────────────────────────────────────────────────┘
```

### 检索流程

1. **文档导入** → 文本分块（512 字符/块，50 字符重叠）
2. **分词** → cppjieba 中文分词 + 去停用词
3. **建索引** → 倒排索引 + 向量 Embedding
4. **搜索** → BM25 关键词检索 + 余弦相似度向量检索 → 加权融合排序
5. **生成** → 拼接检索结果作为上下文 → DeepSeek 流式生成答案

## 项目结构

```
rag-search-engine/
├── include/
│   ├── config/
│   │   └── app_config.h          # 全局配置（检索参数、API 配置）
│   ├── document/
│   │   ├── parser.h              # 文档解析 + 文本分块
│   │   └── tokenizer.h           # 中文分词器（封装 cppjieba）
│   ├── index/
│   │   ├── inverted_index.h      # 倒排索引数据结构
│   │   └── bm25_ranker.h         # BM25 排序算法
│   ├── vector/
│   │   ├── embedding.h           # 向量嵌入服务（调用 AI API）
│   │   └── similarity.h          # 余弦相似度计算
│   ├── rag/
│   │   ├── retriever.h           # 混合检索器（BM25 + 向量融合）
│   │   └── generator.h           # AI 答案生成器（SSE 流式）
│   └── ui/
│       └── main_window.h         # Qt 主窗口
├── src/                          # 实现文件（与 include/ 一一对应）
│   ├── main.cpp                  # 程序入口
│   ├── document/
│   ├── index/
│   ├── vector/
│   ├── rag/
│   └── ui/
├── third_party/                  # 第三方库
│   ├── cppjieba/                 # 中文分词（MIT 协议）
│   ├── limonp/                   # cppjieba 依赖
│   └── nlohmann/                 # JSON 库（备用）
├── CMakeLists.txt                # CMake 构建配置
├── run.bat                       # Windows 启动脚本
└── test/                         # 测试目录
```

## 环境要求

| 依赖 | 版本 | 说明 |
|------|------|------|
| **Qt** | 6.x (MinGW 或 MSVC) | GUI + Network 模块 |
| **CMake** | ≥ 3.16 | 构建工具 |
| **编译器** | GCC 13+ / MSVC 2022 | 需支持 C++17 |
| **操作系统** | Windows 10/11 | 当前仅 Win32 构建 |
| **DeepSeek API Key** | — | 用于向量检索和 AI 回答 |

## 快速开始

### 1. 安装 Qt6

从 [Qt 官网](https://www.qt.io/download-qt-installer) 下载安装 Qt 6.x，选择 MinGW 64-bit 组件。

### 2. 克隆项目

```bash
git clone <repo-url> rag-search-engine
cd rag-search-engine
```

第三方库已随项目分发（cppjieba + limonp + nlohmann），无需额外下载。

### 3. 编译

```bash
# 创建构建目录
mkdir build && cd build

# 配置 CMake（替换为你的 Qt 路径）
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="D:/Qt/6.10.2/mingw_64" \
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe"

# 编译
mingw32-make -j4
```

编译完成后，可执行文件为 `build/rag_search_engine.exe`。

### 4. 设置 API Key

程序通过环境变量读取 DeepSeek API Key：

```bash
# 当前终端有效
set DEEPSEEK_API_KEY=sk-xxxxxxxxxxxxxxxx

# 或永久设置（系统环境变量）
setx DEEPSEEK_API_KEY "sk-xxxxxxxxxxxxxxxx"
```

> 📌 获取 API Key：访问 [platform.deepseek.com](https://platform.deepseek.com) 注册并创建 API Key。

### 5. 启动

```bash
# 方式一：使用启动脚本
run.bat

# 方式二：手动启动
set PATH=D:\Qt\6.10.2\mingw_64\bin;%PATH%
build\rag_search_engine.exe
```

## 使用指南

### 导入文档

1. 点击 **"📂 导入文档"** 按钮
2. 选择一个或多个 `.txt` / `.md` 文件
3. 状态栏会显示导入进度和已索引文本块数量

### 搜索问答

1. 在搜索框输入问题（例如："这份文档中提到的核心技术是什么？"）
2. 点击 **"🔍 搜索"** 或按 Enter
3. **左侧**展示检索结果列表，包含相关度分数（BM25 + 向量）
4. **右侧**实时展示 AI 生成的回答（流式输出）

### 检索结果分数说明

| 字段 | 含义 |
|------|------|
| `finalScore` | 加权融合分数（BM25 × 0.4 + 向量 × 0.6） |
| `bm25Score` | 纯关键词匹配分数 |
| `vectorScore` | 语义相似度分数 |

> 分数越高，表示该文本块与你的问题越相关。

### 清空索引

点击 **"🗑 清空索引"** → 确认后清除所有已导入的文档数据。

## 配置说明

编辑 `include/config/app_config.h` 可调整以下参数：

```cpp
// ── 检索配置 ──
constexpr int DEFAULT_TOP_K = 5;         // 默认返回结果数
constexpr int MAX_CHUNK_SIZE = 512;       // 文本块最大字符数
constexpr int CHUNK_OVERLAP = 50;         // 文本块重叠字符数
constexpr double BM25_WEIGHT = 0.4;       // BM25 权重
constexpr double VECTOR_WEIGHT = 0.6;     // 向量检索权重

// ── AI 配置 ──
constexpr const char* CHAT_MODEL = "deepseek-chat";
constexpr const char* EMBEDDING_MODEL = "text-embedding-3-small";
constexpr int HTTP_TIMEOUT = 30;          // API 请求超时（秒）
```

修改后需重新编译。

## 降级模式

即使没有设置 `DEEPSEEK_API_KEY`，程序仍可使用：

| 功能 | 有 API Key | 无 API Key |
|------|-----------|-----------|
| 文档导入 + 分词 | ✅ | ✅ |
| BM25 关键词搜索 | ✅ | ✅ |
| 向量语义检索 | ✅ | ❌ |
| AI 智能回答 | ✅ | ❌ |

无 API Key 时，搜索框输入问题后仅展示 BM25 关键词检索结果列表。

## 开发调试

### 查看编译详情

```bash
# 详细编译输出
mingw32-make VERBOSE=1
```

### 静态分析

项目代码使用 C++17 标准库为主，辅以 Qt6 框架。核心数据结构：

- **倒排索引** — `std::unordered_map<std::string, std::vector<Posting>>`
- **文本块存储** — `std::unordered_map<docKey, content>`
- **向量存储** — 内存 `std::vector<std::vector<double>>`

### 目录说明

| 目录 | 用途 |
|------|------|
| `include/` | 头文件（公开接口） |
| `src/` | 实现文件 |
| `third_party/` | 第三方库（不参与编译） |
| `build/` | CMake 构建输出（自动生成，不提交） |
| `test/` | 单元测试 |

## 常见问题

### Q: 启动时提示"找不到 Qt6Widgets.dll"？

A: 需要将 Qt 的 `bin` 目录加入 `PATH`，参考 `run.bat` 中的设置。

### Q: 分词功能异常，程序崩溃？

A: 确认 `third_party/cppjieba/dict/` 目录存在且包含以下文件：
- `jieba.dict.utf8`
- `hmm_model.utf8`
- `user.dict.utf8`
- `idf.utf8`
- `stop_words.utf8`

### Q: 点击搜索后 AI 回答区显示"未配置 AI API"？

A: 检查是否设置了 `DEEPSEEK_API_KEY` 环境变量，设置后需重启程序。

### Q: AI 回答很慢或超时？

A: 
1. 检查网络是否能访问 `api.deepseek.com`
2. 增大 `HTTP_TIMEOUT` 配置值
3. 减少导入文档数量以缩短检索时间

### Q: 如何添加更多文档格式支持？

A: 编辑 `src/document/parser.cpp` 中的 `parse()` 方法，添加对应格式的解析逻辑。

## 依赖许可

| 库 | 许可 | 用途 |
|----|------|------|
| [Qt 6](https://www.qt.io) | LGPLv3 / GPLv3 / Commercial | GUI + 网络 |
| [cppjieba](https://github.com/yanyiwu/cppjieba) | MIT | 中文分词 |
| [limonp](https://github.com/yanyiwu/limonp) | MIT | cppjieba 依赖 |
| [DeepSeek API](https://platform.deepseek.com) | 商业 API | Embedding + LLM |

## License

本项目仅供学习和研究使用。
