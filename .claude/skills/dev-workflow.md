# 开发工作流：实现 → 编译 → 测试 → 汇报 → 用户确认 → Git → 下一任务

本 skill 定义法律 RAG 检索引擎（legal-rag-search）的标准开发流程。每完成一个任务/修复，必须严格按此流程执行。

**两条铁律**（用户明确要求）：
1. **每完成一项，先自动编译 + 跑全部测试自检，再请用户检查，用户确认无误后才 git 提交**——绝不未经确认就 commit；
2. **每修复一个 bug，必须同步更新 `TROUBLESHOOTING.md` 踩坑记录**（现象/根因/修复/教训，沿用 #编号 递增格式）。

## 流程

```
┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐
│ 1.实现  │→ │ 2.编译  │→ │ 3.测试  │→ │ 4.汇报  │→ │ 用户确认 │→ │ 5.Git  │
└────────┘  └────────┘  └────────┘  └────────┘  └────────┘  └────────┘
                                        │              │no      │yes
                                        ▼              ▼        ▼
                                  更新踩坑记录      修复问题   继续下一任务
```

## 每步具体操作

### Step 1: 实现

- 先读需要修改的现有文件，理解当前状态；控制改动范围，不做无关重构
- 新增源文件/测试记得更新 `CMakeLists.txt`
- **Qt5/Qt6 双版本兼容**：项目同时运行在本机 Qt 5.15.2 (MinGW 8.1) 与另一台电脑 Qt 6.x 上。注意：弃用属性用 `#if QT_VERSION < QT_VERSION_CHECK(6,0,0)` 守卫；`capturedStart/End` 等返回 `qsizetype`；禁用 Qt6 已移除的 API（QTextCodec/QRegExp/单字符串 QProcess::start 等）

### Step 2: 编译

```bash
# 本机（Qt 5.15.2 MinGW 8.1）；首次配置后日常只需 make
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="D:/QT5.15.2/5.15.2/mingw81_64" \
  -DCMAKE_CXX_COMPILER="D:/QT5.15.2/Tools/mingw810_64/bin/g++.exe" \
  -DCMAKE_C_COMPILER="D:/QT5.15.2/Tools/mingw810_64/bin/gcc.exe" \
  -DCMAKE_MAKE_PROGRAM="D:/QT5.15.2/Tools/mingw810_64/bin/mingw32-make.exe" \
  -DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON

D:/QT5.15.2/Tools/mingw810_64/bin/mingw32-make.exe -C build -j4
```

- 必须零错误；若链接报 "Permission denied/cannot open"，先关掉正在运行的 `legal_rag_search.exe` 再重链
- **Qt6 机器**：仓库拉取后直接用已有 build 目录重建即可；CMake 自动探测 Qt6 并回退 Qt5

### Step 3: 测试

```bash
# 从项目根目录运行（测试有路径检查），45 项，无需 API Key
./build/run_tests.exe
./build/test_utf8_location_regression.exe
./build/eval_retrieval.exe     # 检索质量评测（Hit@5/R@10/MRR），改了检索逻辑后必跑
```

- 任何失败先修根因，不带失败项进入下一任务
- GUI 改动需启动应用实机验证：
  ```bash
  powershell -NoProfile -Command "Start-Process -FilePath 'D:\VS_Project\legal-rag-search\build\legal_rag_search.exe' -WorkingDirectory 'D:\VS_Project\legal-rag-search\build'"
  ```

### Step 4: 汇报

向用户汇报的格式：

```
## ✅ 任务完成：<名称>

### 改动
| 文件 | 说明 |
|------|------|

### 自检结果
- 编译：✅ 零错误
- 测试：✅ 45/45 通过（+回归测试）
- 实机验证：✅ <具体内容>

请检查，确认后我提交 git。
```

### Step 5: Git（用户确认后才执行）

```bash
git add <明确列出的文件>          # 不用 git add -A，避免误带无关文件
git commit -m "<type>: <描述>"
```

commit message 规范（沿用仓库现状）：`feat:` / `fix:` / `test:` / `docs:` / `chore:` + 中文描述；修复类必须写清根因。

## 运行环境与部署清单

| 组件 | 位置 | 说明 |
|------|------|------|
| exe | `build/legal_rag_search.exe` | 注意旧名 rag_search_engine.exe 已弃用 |
| Qt5 DLL | `build/Qt5Core/Gui/Widgets/Network.dll` | 从 `D:\QT5.15.2\5.15.2\mingw81_64\bin` 拷贝 |
| 平台插件 | `build/platforms/qwindows.dll` | 缺失无法启动 |
| MinGW 运行时 | `build/libgcc_s_seh-1.dll` 等 3 个 | 从 `D:\QT5.15.2\Tools\mingw810_64\bin` 拷贝 |
| **OpenSSL 1.1** | `build/libssl-1_1-x64.dll`、`libcrypto-1_1-x64.dll` | **HTTPS 必需**，缺了报 TLS initialization failed；来源 slproweb Win64OpenSSL Light 1.1.1w；OpenSSL 3 不能用于 Qt5 |
| OCR 环境 | Python 3.12 + PyMuPDF + rapidocr-onnxruntime（`%LOCALAPPDATA%\Programs\Python\Python312`） | 真实 PDF 的实际前置条件；级联 = 文本层→RapidOCR→Tesseract→PaddleOCR |
| API Key | 界面 Key 行填写（内存态，重启需重填）；或 `run.bat`（已 gitignore） | DeepSeek 平台获取 |

## 当前任务清单

| # | 任务 | 状态 |
|---|------|------|
| 86-92 | 初始功能开发（Git 初始化/PDF 导入/法律词典/元数据/筛选/Prompt/E2E） | ✅ 全部完成 |
| 93 | 构建基线迁移至 Qt 5.15.2/MinGW 8.1 + 45 项测试全绿 | ✅ 完成（commit b28dc6c） |
| 94 | GUI 端到端验证 + 截图 + 验证清单 | ✅ 完成（commit 1b14c8a） |
| 95 | 量化评测（23 条标注查询，Hit@5/R@10/MRR = 0.957/0.957/0.913） | ✅ 完成（commit fc78cb8） |
| 96 | 真实文书实测 5 问题修复（OCR 链路/假死/TLS/SSE 越界，踩坑 #17-21） | ✅ 完成（commit 8a2f378） |
| 97 | 界面高分屏自适应 + Ctrl+滚轮缩放 + 传输超时 | ✅ 完成（commit 4131edf） |
| 98 | 答辩报告 defense_report.md（含真实文书实测章节） | 🔄 待用户审阅后提交 |
| 99 | 答辩流程 defense_process.md（时间轴 + 演示脚本 + 预设问答） | pending |
| 100 | 答辩 PPT（13 页，judge 视觉验收通过，构建脚本 docs/defense/build_ppt.js 可复用改版） | ✅ 完成 |

## 后续迭代候选（已列入答辩报告「不足与展望」，未经用户确认不要擅自动工）

- 索引持久化（重启免重导/免重 OCR）——用户已明确暂缓
- embedding 提供方可配置化（当前 DeepSeek 无 embeddings 端点，向量路实际降级为 BM25）
- 重排序（rerank）、法律同义词归一化（可消解评测中的词汇失配失败案例）
