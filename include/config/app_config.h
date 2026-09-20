#pragma once
#include <string>
#include <cstdint>

namespace config {

// ── AI API 配置 ──
constexpr const char* AI_API_BASE_URL = "https://api.deepseek.com";
constexpr const char* AI_API_KEY_ENV = "DEEPSEEK_API_KEY";
constexpr const char* CHAT_MODEL = "deepseek-chat";
constexpr const char* EMBEDDING_MODEL = "text-embedding-3-small";  // or use DeepSeek embedding

// ── 检索配置 ──
constexpr int DEFAULT_TOP_K = 5;
constexpr int MAX_CHUNK_SIZE = 512;      // 每个文本块最大字符数
constexpr int CHUNK_OVERLAP = 50;        // 文本块重叠字符数
constexpr double BM25_WEIGHT = 0.4;      // BM25 权重
constexpr double VECTOR_WEIGHT = 0.6;    // 向量检索权重

// ── 数据库配置 ──
constexpr const char* DB_PATH = "rag_index.db";

// ── 索引持久化（T1）──
// 落盘文件默认位于程序工作目录；关闭程序时写出，启动时自动恢复，
// 免去每次重开都要重新导入语料。
constexpr const char* INDEX_FILE = "rag_index.dat";

// ── 请求超时 (秒) ──
constexpr int HTTP_TIMEOUT = 30;

} // namespace config
