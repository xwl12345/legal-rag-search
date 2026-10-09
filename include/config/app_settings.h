#pragma once
#include <string>

#include <QMetaType>

namespace config {

/// 检索参数配置（T3 配置中心）
///
/// 存储形态：JSON 文件（默认 rag_settings.json，位于工作目录）。
/// 选 JSON 而非 QSettings 的理由：
///   1. QSettings 在 Windows 上写注册表，用户看不见、难备份、难手工核对；
///   2. JSON 可读可手工编辑，容错测试容易构造（本文件含损坏用例）；
///   3. 只依赖 QtCore（QJsonDocument），不引入新模块，与项目零重型依赖一致。
///
/// 容错约定（任务卡 T3）：
///   文件不存在 / 字段缺失 / 字段类型不对 / JSON 被改坏 —— 一律回落默认值，
///   绝不崩溃、绝不读到 0 或空串冒充配置。load() 返回是否成功读到文件内容。
struct AppSettings {
    // ── BM25 参数（查询期，改后立即生效）──
    double k1 = 1.5;              // 词频饱和参数
    double b = 0.75;              // 文档长度归一化参数

    // ── 混合融合权重（查询期，改后立即生效）──
    double bm25Weight = 0.4;
    double vectorWeight = 0.6;

    // ── 检索条数（查询期）──
    // 语义：检索问答页每次检索返回的候选文本块数。
    // 默认 20 与改造前检索页的实际行为一致（search(20)，文档去重宽检索 50），
    // 刻意不用引擎内部的 DEFAULT_TOP_K=5 —— 配置默认值必须对齐真实行为，
    // 不留"配置写 5、界面实际 20"的静默不一致。
    int topK = 20;

    // ── 分块参数（建索引期固定）──
    // ⚠️ 语义：仅对**之后导入**的文档生效。已建索引的块边界在建块时已固定，
    // 改小不会让现有文本块重新切（设置页有同样的标注）。
    int chunkSize = 512;          // 每个文本块最大字符数
    int chunkOverlap = 50;        // 相邻文本块重叠字符数

    // ── AI 生成参数（查询期）──
    double temperature = 0.3;

    // ── 生成（LLM）服务（P3：Key 分家后服务分家补齐；Key 归检索页管）──
    std::string chatBaseUrl = "https://api.deepseek.com";
    std::string chatModel = "deepseek-chat";

    // ── Embedding 服务（T4 消费）──
    std::string embeddingBaseUrl = "https://api.deepseek.com";
    std::string embeddingModel = "text-embedding-3-small";
    // 留空 = 沿用检索页 / DEEPSEEK_API_KEY 环境变量的既有流程；非空时覆盖。
    std::string embeddingApiKey;

    /// 默认值实例（P3 起是全部检索/服务参数的唯一事实来源）
    static AppSettings defaults();

    /// 字段钳制（P3）：数值越界/NaN 回落默认，融合权重非负且重归一化，
    /// chunkOverlap < chunkSize，服务地址/模型空串回落默认。
    /// load() 与 save() 内部统一调用——两条路径一套钳制，别处不再各写一遍。
    void sanitize();

    /// 从 JSON 文件加载。文件不存在 / 解析失败返回 false（out 保持默认值）；
    /// 单个字段缺失或类型不对时该字段回落默认，其余字段照常生效。
    static bool load(const std::string& filePath, AppSettings& out);

    /// 保存为 JSON 文件（UTF-8，紧凑格式）。失败返回 false 并填 diagnostic。
    static bool save(const std::string& filePath, const AppSettings& data,
                     std::string* diagnostic = nullptr);
};

} // namespace config

Q_DECLARE_METATYPE(config::AppSettings)
