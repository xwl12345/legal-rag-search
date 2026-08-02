# 开发工作流：实现 → 测试 → 汇报 → Git → 下一任务

本 skill 定义了 RAG 法律检索工具项目的标准开发流程。每完成一个任务，必须严格按此流程执行。

## 流程

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ 1. 实现   │ → │ 2. 编译   │ → │ 3. 测试   │ → │ 4. 汇报   │ → │ 5. Git   │
│           │    │           │    │           │    │           │    │           │
└──────────┘    └──────────┘    └──────────┘    └──────────┘    └──────────┘
                                                      │
                                                用户确认通过？
                                                 │no      │yes
                                                 ▼        ▼
                                            修复问题   继续下一任务
```

## 每步具体操作

### Step 1: 实现

- 先读需要修改的现有文件，理解当前状态
- 写代码，控制改动范围，不做无关重构
- 新增文件记得更新 CMakeLists.txt

### Step 2: 编译

```bash
cd build
cmake .. -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="D:/Qt/6.10.2/mingw_64" \
  -DCMAKE_CXX_COMPILER="D:/Qt/Tools/mingw1310_64/bin/g++.exe" \
  -DCMAKE_MAKE_PROGRAM="D:/Qt/Tools/mingw1310_64/bin/mingw32-make.exe"
mingw32-make -j4
```

- 必须零错误零警告
- 如果有关联的测试代码，也要编译通过

### Step 3: 测试

- **单元测试**：运行 `build/run_tests.exe`，确保全部通过
  ```bash
  cmd //c "cd /d D:\graduation_project\rag-search-engine && build\run_tests.exe"
  ```
- **功能验证**：如有 GUI 改动，启动应用手动验证
  ```bash
  cmd //c "cd /d D:\graduation_project\rag-search-engine\build && set DEEPSEEK_API_KEY=sk-xxx && start rag_search_engine.exe"
  ```
- 如果测试不通过，回到 Step 1 修复

### Step 4: 汇报

向用户汇报的内容格式：

```
## ✅ 任务 X 完成：<任务名称>

### 改动
| 文件 | 操作 | 说明 |
|------|------|------|

### 测试结果
- 编译：✅ 无错误
- 单元测试：✅ N/N 通过
- 功能验证：✅ <具体验证内容>

### 下一步
任务 X+1：<下一任务名称>
```

### Step 5: Git

用户确认后，执行 git 操作：

```bash
cd D:/graduation_project/rag-search-engine
git add -A
git status                    # 让用户看到改动清单
git commit -m "<type>: <简短描述>"
```

commit message 规范：
- `feat: 添加 PDF 导入支持`
- `fix: 修复查询大小写匹配问题`
- `refactor: 重构分词器大小写归一化`

## 当前任务清单

| # | 任务 | 状态 | 依赖 |
|---|------|------|------|
| 86 | Git 初始化 + .gitignore | pending | — |
| 87 | PDF 导入支持 | pending | #86 |
| 88 | 法律分词词典 | pending | #87 |
| 89 | 元数据提取 | pending | #86 |
| 90 | UI 筛选栏 | pending | #89 |
| 91 | 法律 Prompt 模板 | pending | #88 |
| 92 | Demo 数据集 + 端到端测试 | pending | #90, #91 |

## 环境

- **编译器**: D:/Qt/Tools/mingw1310_64/bin/g++.exe (GCC 13.1.0)
- **CMake**: D:/Qt/Tools/CMake_64/bin/cmake.exe
- **Qt**: D:/Qt/6.10.2/mingw_64
- **API Key**: 已写入 run.bat，通过 set DEEPSEEK_API_KEY 传入
- **启动**: 双击 run.bat（所有 DLL 已通过 windeployqt 部署到 build/）
