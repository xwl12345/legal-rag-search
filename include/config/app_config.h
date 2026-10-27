#pragma once
#include <string>

namespace config {

// ── 仍被引用的常量（P3 清理：9 个无引用死常量已删除，检索参数以
//    AppSettings::defaults() 为唯一事实来源，见 include/config/app_settings.h）──

/// LLM API Key 的环境变量名（检索页启动加载用）
constexpr const char* AI_API_KEY_ENV = "DEEPSEEK_API_KEY";

// ── 数据文件名（P3 起存放于 **exe 所在目录**，经 dataFilePath() 解析）──

/// 索引落盘文件（T1）：关闭程序时写出，启动时自动恢复
constexpr const char* INDEX_FILE = "rag_index.dat";

/// 问答历史 SQLite 库（T2）：运行期产物，不入库
constexpr const char* HISTORY_DB = "rag_history.db";

/// 案例收藏 SQLite 库（T11）：运行期产物，不入库。
/// 独立文件不并入 rag_history.db：收藏是用户长期资产（类案集），
/// 与问答历史（会话流水）生命周期不同，清理历史不应误伤收藏。
constexpr const char* FAVORITE_DB = "favorites.db";

/// 检索参数配置（T3）：JSON 可手工编辑；因可含 API Key，绝不入库
constexpr const char* SETTINGS_FILE = "rag_settings.json";

/// 同义词映射（T7）：口语→规范术语映射表，设置页可编辑；运行副本不入库
constexpr const char* SYNONYMS_FILE = "synonyms.json";

/// 法条库 SQLite（T9）：由 tools/build_law_db.py 从 data/law_corpus/*.json
/// 构建后随程序分发；运行期只读，不入 .gitignore
constexpr const char* LAW_DB = "law_corpus.db";

/// 请求超时（秒）：embedding 1x，LLM 2x（流式）
constexpr int HTTP_TIMEOUT = 30;

/// 数据文件路径解析（P3 决策 ③：exe 所在目录）。
/// 整个程序目录拷走即带走全部数据，答辩/换机演示不依赖工作目录。
/// 首次解析时若 exe 目录缺该文件而**工作目录**存在旧文件，自动搬移（一次性迁移）。
std::string dataFilePath(const char* fileName);

} // namespace config
