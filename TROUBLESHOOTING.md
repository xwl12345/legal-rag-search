# 踩坑记录 & 解决方案

本文档记录了 RAG 智能检索引擎开发过程中遇到的所有问题、根因分析和解决方案。

---

## 编译问题

### 1. `std::unique_ptr` 未定义 → `tokenizer.h` 编译失败

**现象**：
```
error: 'unique_ptr' in namespace 'std' does not name a template type
```

**根因**：`tokenizer.h` 使用了 `std::unique_ptr`（Pimpl 模式），但只 include 了 `<string>`、`<vector>`、`<unordered_set>`，没有 include `<memory>`。

**修复**：在 `include/document/tokenizer.h` 中添加 `#include <memory>`。

---

### 2. 中文字符多字节常量警告 → `parser.cpp`

**现象**：
```
warning: multi-character character constant [-Wmultichar]
    if (c == '\n' || c == '。' || c == '！' || c == '？' || ...
```

**根因**：UTF-8 编码下，中文字符 `'。'` 占 3 个字节，不能放在单引号 `''` 中（单引号只能放单字节字符）。GCC 将其解释为多字节常量，行为不可预期。

**修复**：将中文标点判断移除，仅保留 ASCII 断点字符（`\n`、`\r`、`.`、`!`、`?`）。

---

### 3. `namespace index` 与 GCC built-in 冲突

**现象**：
```
warning: built-in function 'index' declared as non-function
```

**根因**：GCC/glibc 有一个历史遗留的 built-in 函数叫 `index`（等价于 `strchr`）。当声明 `namespace index` 时，编译器认为你在试图覆盖这个 built-in。

**修复**：将 `namespace index` 重命名为 `namespace search_index`。影响范围：
- `include/index/inverted_index.h`
- `include/index/bm25_ranker.h`
- `src/index/inverted_index.cpp`
- `src/index/bm25_ranker.cpp`
- `include/rag/retriever.h`（`index::` → `search_index::`）

---

### 4. `std::setprecision` 未定义 → `retriever.cpp` 编译失败

**现象**：
```
error: 'setprecision' is not a member of 'std'
```

**根因**：`retriever.cpp` 使用了 `std::fixed` 和 `std::setprecision`，但没有 include `<iomanip>`。

**修复**：在 `src/rag/retriever.cpp` 中添加 `#include <iomanip>`。

---

### 5. `nlohmann/json.hpp` 找不到 → CMake include 路径错误

**现象**：
```
fatal error: nlohmann/json.hpp: No such file or directory
```

**根因**：nlohmann/json 仓库克隆后，`json.hpp` 实际路径是 `single_include/nlohmann/json.hpp`，但 CMakeLists.txt 中设置的 include 路径是 `third_party/nlohmann`，导致 `#include "nlohmann/json.hpp"` 解析为 `third_party/nlohmann/nlohmann/json.hpp`（实际不存在）。

**修复**：将 CMake 中的 `JSON_INCLUDE_DIR` 改为 `third_party/nlohmann/single_include`。

> **教训**：头文件库 clone 后要确认实际目录结构，不要在 CMake 中假设路径。

---

### 6. `cpp-httplib` 不兼容 GCC 13 + 网络下载失败 → 替换为 Qt6::Network

**现象**：
```
httplib.h:1250: error: unterminated #ifdef
error: 'char_traits' does not name a type
...数百行级联错误...
```

**根因**：
1. GitHub raw 在国内网络不稳定，`curl` 下载 `httplib.h` 被截断（只下了 36KB，实际约 200KB+）
2. 即使完整下载，cpp-httplib 旧版本在内部定义了 `namespace std`（阴影了 `::std`），与 GCC 13 不兼容

**修复**：放弃 cpp-httplib，改用 Qt6 自带的 `QNetworkAccessManager` + `QNetworkReply`。改动文件：
- `src/vector/embedding.cpp`：HTTP POST → DeepSeek Embedding API，使用 `QEventLoop` 同步等待
- `src/rag/generator.cpp`：HTTP POST + SSE 流式解析，使用 `readyRead` 信号处理流式数据
- `CMakeLists.txt`：移除 `cpp-httplib` 相关配置

> **教训**：能用框架自带方案就不要另加依赖。Qt 本身就有完善的 HTTP 客户端。

---

## 运行时问题

### 7. DLL 入口点错误 #1：libstdc++/libgcc 版本冲突

**现象**：
```
无法定位程序输入点 _ZNSt7__cxx11... 于动态链接库 libstdc++-6.dll
```

**根因**：系统上有 3 个不同版本的 `libstdc++-6.dll`：
- `D:\Qt\6.10.2\mingw_64\bin\`（Qt 自带）
- `D:\Qt\Tools\mingw1310_64\bin\`（编译器工具链）
- Git Bash 的 `/mingw64/bin/`

exe 用 GCC 13.1.0 编译，链接了 MinGW 13.1.0 的 libstdc++。但运行时 PATH 中先找到了 Qt 自带的老版本 libstdc++，入口点不匹配。

**修复**：在 CMake 中添加 `-static-libstdc++ -static-libgcc`，将这两个库静态编译进 exe。

```cmake
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE -static-libstdc++ -static-libgcc)
endif()
```

---

### 8. DLL 入口点错误 #2：libwinpthread 遗漏

**现象**：修完 #7 后仍然弹窗"无法定位程序输入点"，这次是 `libwinpthread-1.dll`。

**根因**：`-static-libstdc++ -static-libgcc` 只静态链接了 libstdc++ 和 libgcc，**遗漏了 libwinpthread**。这个 DLL 同样存在于多个 MinGW 中，版本不一致导致入口点错误。

**修复**：将链接标志从 `-static-libstdc++ -static-libgcc` 改为 `-static`，一次性静态链接所有 MinGW 运行时。

```cmake
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE -static)
endif()
```

`-static` 会让链接器优先使用静态库（`.a`），对于 MinGW 运行时（libstdc++、libgcc、libwinpthread、libmingw32 等）全部静态链接进 exe；对于 Qt 库，因为没有静态版本（`.a` 文件不存在），链接器自动回退到动态链接。

**验证方法**：
```bash
objdump -p build/rag_search_engine.exe | grep "DLL Name"
# 输出应只包含 Qt6*.dll、KERNEL32.dll、msvcrt.dll
# 不应包含 libstdc++、libgcc、libwinpthread
```

---

### 9. `setx` 设置环境变量后程序读不到

**现象**：用 `setx DEEPSEEK_API_KEY "sk-xxx"` 设置后，启动程序仍显示"未配置 API Key"。

**根因**：`setx` 将变量写入注册表（`HKEY_CURRENT_USER\Environment`），但**只对全新启动的进程生效**。从当前终端或 IDE 启动的程序继承的是旧环境。

**修复**：
1. 临时方案：当前终端中 `set DEEPSEEK_API_KEY=sk-xxx`（仅当前会话）
2. 永久方案：写入启动脚本 `run.bat`，每次启动自动设置
3. 用户体验方案：在 UI 中添加 API Key 输入框（见 #12）

> **教训**：`setx` ≠ `export`。Windows 上程序读取环境变量的时机是进程启动时，不会动态刷新。

---

### 10. windeployqt 不会部署 MinGW 运行时 DLL

**现象**：运行 `windeployqt` 后，build 目录有 Qt DLL 但没有 MinGW 运行时 DLL。

**根因**：`windeployqt` 只负责 Qt 自己的 DLL 和插件（platforms、styles、imageformats 等），不管编译器运行时。

**解决方案**：
- 短期：用 `-static` 消除 MinGW 运行时 DLL 依赖（推荐，已采用）
- 中期：手动复制 `libwinpthread-1.dll` 等文件
- 长期：用 CPack 或 NSIS 制作安装包时一并打包

---

## 逻辑问题

### 11. 文档导入时重复分块

**现象**：`addDocument()` 和 `addText()` 都对文本做了一次分块，导致一个文本块被切成更小的子块。

**根因**：
```cpp
// addDocument 调 parser.parse() 分了块
auto chunks = parser.parse(filePath);
// 然后对每个 chunk 调 addText，addText 又调 parser.parseText() 再分一次
for (const auto& chunk : chunks) {
    addText(chunk.content, chunk.docId);  // 重复分块！
}
```

**修复**：重构 `addDocument()`，直接读取原始文本，交给 `addText()` 一次性处理分块+索引。`addDocument` 只负责文件 I/O，`addText` 负责分块+索引。

---

### 12. 大小写不匹配：`rag` vs `RAG`

**现象**：用户输入 `rag`（小写）搜不到文档中的 `RAG`（大写）。

**根因**：cppjieba 不分词不做大小写归一化，`rag` 和 `RAG` 被视为不同的词，倒排索引中存的是 `RAG`，查询用的是 `rag`，BM25 匹配不上。

**修复**：在 `Tokenizer::cutForIndex()` 中，过滤停用词之前，对每个 token 做 ASCII 小写转换。

```cpp
for (char& c : w) {
    if (static_cast<unsigned char>(c) < 128) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}
```

> **注意**：只转 ASCII 字符（`< 128`），不影响 UTF-8 中文。

---

## UI 问题

### 13. 搜索结果为空时 AI 回答区永远卡在"正在检索..."

**现象**：有 API Key 但搜索结果为 0 条时，右侧 AI 回答区停留在"正在检索..."，不会更新为任何提示。

**根因**：`onSearch()` 中的条件判断有漏洞：
```cpp
if (generator_->isReady() && !results.empty()) {
    // AI 生成回答
} else if (!generator_->isReady()) {
    // 提示设置 API Key
}
// 当 isReady()==true 且 results.empty() 时，两个分支都不进！
```

**修复**：添加第三个分支处理 `results.empty()` 的情况：
```cpp
} else if (results.empty()) {
    // 显示"未找到相关文档" + 建议
} else if (!generator_->isReady()) {
    // 提示设置 API Key
}
```

---

### 14. 文档导入失败静默吞异常

**现象**：导入文件失败时没有任何提示，用户不知道出了问题。

**根因**：
```cpp
try {
    retriever_->addDocument(files[i].toStdString());
    imported++;
} catch (const std::exception& e) {
    // 跳过无法解析的文件  ← 什么都不提示！
}
```

**修复**：收集错误信息，导入完成后在状态栏显示：
```cpp
QStringList errors;
// ...
errors.append(QString::fromStdString(e.what()));
// ...
if (!errors.isEmpty()) {
    statusLabel_->setText(QString("⚠️ %1 成功，%2 失败：%3")
                          .arg(imported).arg(errors.size()).arg(errors.first()));
}
```

---

## Git 问题

### 15. 第三方库的 `.git` 目录导致嵌套仓库

**现象**：`git add third_party/` 时，cppjieba/limonp/nlohmann 被识别为 submodule 而非普通目录。

**根因**：这些目录是通过 `git clone` 下载的，各自包含 `.git` 目录（独立仓库）。

**修复**：删除 `third_party/*/\.git`，将它们作为普通文件提交到主仓库。

```bash
rm -rf third_party/cppjieba/.git
rm -rf third_party/limonp/.git
rm -rf third_party/nlohmann/.git
```

---

### 16. 编译产物混入 Git 提交

**现象**：`git add -A` 时把截图 `.png`、可执行文件等也加了进去。

**修复**：完善 `.gitignore`：
```gitignore
build/
*.exe
*.dll
*.obj
*.o
*.png
*.jpg
moc_*
*_autogen/
run.bat         # 含 API Key
```

已误提交的文件用 `git rm --cached` 移除。

---

## 真实文档实测问题（2026-08 用真实裁判文书验证时发现）

### 17. 真实判决书 PDF（WPS/CID 字体）文本层提取为空 → 导入失败

**现象**：导入真实下载的判决书 PDF，弹「未找到可运行 PyMuPDF（fitz）的 Python 解释器」。

**根因**：两连击。① 自研 PDF 解析器只支持简单文本编码，而真实文书（WPS 导出）使用 CID 子集字体 + ToUnicode CMap，正确逐字提取需要解析 CMap，超出轻量解析器能力 → 文本层为空 → 触发 OCR 回退；② 本机没有 Python 环境 → OCR 也失败 → 导入整体失败。

**修复**：安装 Python 3.12 + PyMuPDF + rapidocr-onnxruntime（覆盖级联第 1、2 级）后，该 PDF 由 PyMuPDF 文本层直接完整提取（9 页）。

> **教训**：处理真实 PDF 时 OCR 环境是实际前置条件，诊断弹窗给出的提示就是安装指引；自研解析器的「零依赖」能力边界要如实标注（CID/ToUnicode 解析为后续增强方向）。

---

### 18. PATH 上的商店占位 `python.exe` 遮蔽真实 Python

**现象**：装好 Python 后，OCR 仍报「未找到可运行 PyMuPDF 的 Python 解释器」。

**根因**：Windows 商店的 App Execution Alias 在 PATH 上放了一个假的 `python.exe`（运行只提示"未找到"并退出）。`findPython` 探测 PATH 命中这个占位程序、验证失败后就返回空，真实安装在 `AppData\Local\Programs\Python\Python312` 的解释器永远轮不到。

**修复**：`findPython` 在 PATH 探测**之前**先扫描标准安装位置（`%LOCALAPPDATA%\Programs\Python\Python3xx`、`C:\Program Files\Python\Python3xx`，版本号倒序），每个候选都用 `python -c "import fitz"` 实测验证。

> **教训**：Windows 的商店占位程序会"顶替"真实解释器；任何依赖 PATH 探测运行时的逻辑都要对占位程序免疫。

---

### 19. 扫描件 OCR 期间程序"未响应"（假死约 3 分钟）

**现象**：导入 20 页扫描件 PDF（15.8 MB），窗口灰掉拖不动，约 3 分钟后恢复；观感等同死机。

**根因**：RapidOCR 逐页约 8 秒 × 20 页 ≈ 3 分钟，`QProcess::waitForFinished` 在 UI 线程同步阻塞，期间不派发绘制事件 → Windows 判定无响应。且旧超时上限 5 分钟，35 页以上的扫描件会被中途误杀。

**修复**：等待改为 `QEventLoop` + `QEventLoop::ExcludeUserInputEvents`（持续泵绘制事件，窗口保持响应；屏蔽输入防止等待期间误操作）；默认超时 5→10 分钟；导入状态栏提示「扫描件 OCR 逐页识别，可能需要数分钟」。

> **教训**：凡在 UI 线程同步等待长耗时子进程/网络，必须泵事件或整体移出 UI 线程，否则界面必然假死。

---

### 20. AI 生成报「TLS initialization failed」

**现象**：配置 API Key 后，AI 回答报「LLM API request failed: TLS initialization failed」。

**根因**：Qt 的 HTTPS 依赖 OpenSSL 1.1 动态库（`libssl-1_1-x64.dll`、`libcrypto-1_1-x64.dll`）。本 Qt 5.15.2 安装变体未附带，手工部署运行时时也漏了它们 → Qt 加载不到 OpenSSL，所有 HTTPS 请求直接失败。注意 Git 自带的 OpenSSL 3（libssl-3-x64.dll）**不能**给 Qt 5.15 用，版本必须匹配 1.1。

**修复**：安装 slproweb「Win64OpenSSL Light 1.1.1w」（MSI），把 `libssl-1_1-x64.dll`、`libcrypto-1_1-x64.dll` 拷到 exe 目录。

> **教训**：Qt 网络应用部署清单 = Qt5*.dll + platforms 插件 + MinGW 运行时 + **OpenSSL 1.1**，缺一即 HTTPS 全挂；修改部署清单后必须重启应用重新加载。

---

### 21. AI 回答流式输出完毕后报「substr: __pos > size」

**现象**：AI 回答已完整逐字输出（含四段式与参考来源），结尾却弹「❌ AI 生成失败：basic_string::substr: __pos (1409) > this->size() (1408)」。

**根因**：SSE 解析用 `std::getline` 逐行切分并累计 `processed` 计数，再 `sseBuffer.substr(processed.size())` 保留未完成尾行。当流末尾以**无换行符的半行**结束时，`getline` 依然会取出尾行，代码给它补了一个缓冲区中并不存在的 `'\n'` 计入 `processed` → `processed.size()` 比缓冲区真实长度大 1 → `substr` 越界抛异常。

**修复**：改为标准写法——`find('\n')` 只消费完整行（顺带兼容 CRLF），不完整尾行留在缓冲区；`finished` 后再单独处理残留的最后一行。

> **教训**：手写流式协议解析时，"已消费字节数"必须以真实分隔符为界记账，严禁给补全出来的内容记账；这类差一错误在流刚好断在行中间时才触发，测试很难覆盖到。

---

## 快速排查清单

遇到问题时按顺序检查：

| # | 检查项 | 命令/方法 |
|---|--------|-----------|
| 1 | exe 缺哪些 DLL | `objdump -p build/legal_rag_search.exe \| grep "DLL Name"` |
| 2 | build 目录有这些 DLL 吗 | `ls build/*.dll build/platforms/*.dll` |
| 3 | HTTPS 报 TLS 错误 | build 下有 `libssl-1_1-x64.dll`、`libcrypto-1_1-x64.dll` 吗 |
| 4 | tests 还能过吗 | `build\run_tests.exe` |
| 5 | CMake 配置对了吗 | 重新 `cmake -B build -DCMAKE_PREFIX_PATH=...` |
| 6 | Qt 插件目录在吗 | `ls build/platforms/qwindows.dll` |
| 7 | API Key 设了吗 | 看 UI 顶部 API Key 状态指示 |
| 8 | 文档导入了吗 | 看状态栏导入计数 |
| 9 | 导入扫描件卡住？ | OCR 正常耗时（约 8 秒/页），窗口应保持响应 |

---

## 环境速查

| 组件 | 路径 |
|------|------|
| Qt 5.15.2 (MinGW 8.1) | `D:\QT5.15.2\5.15.2\mingw81_64` |
| 编译器 (GCC 8.1.0) | `D:\QT5.15.2\Tools\mingw810_64\bin\g++.exe` |
| CMake 4.x | `C:\Program Files\CMake\bin\cmake.exe` |
| OpenSSL 1.1.1w（TLS 必需） | `C:\Program Files\OpenSSL-Win64`（DLL 需拷到 exe 目录） |
| Python 3.12 + PyMuPDF + RapidOCR（OCR 级联） | `%LOCALAPPDATA%\Programs\Python\Python312` |
| cppjieba 词典 | `third_party/cppjieba/dict/` |
