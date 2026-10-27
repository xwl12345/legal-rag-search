#pragma once
#include <string>
#include <utility>
#include <vector>

namespace rag {
namespace synonym {

/// 口语 → 规范术语映射（T7 同义词归一化）
///
/// 存储形态：独立 JSON 文件（默认 synonyms.json，exe 目录，运行副本不入库）。
/// 选独立文件而非塞进 rag_settings.json：映射是数据表（行数不定），
/// 与标量参数职责不同；结构风格沿用 T3（QJsonDocument + 容错回落）。
///
/// 容错约定（沿用 T3）：文件不存在 / JSON 损坏 / 字段类型不对 —— 一律回落
/// 内置默认映射，绝不崩溃、绝不静默丢扩展能力。
struct SynonymMap {
    /// spoken = 口语/俗称（查询侧子串匹配）；terms = 追加的规范术语（词项级）。
    /// 用 vector 而非 map：保序（设置页按行展示编辑），重复口语词后行覆盖前行。
    std::vector<std::pair<std::string, std::vector<std::string>>> entries;

    /// 内置默认映射（首次运行生成文件的种子）。
    /// 「电信诈骗」条对应 qrels 唯一 BM25 失败查询（语料用词为「诈骗/骗取」，
    /// 无「电信」——见任务卡 T7 失败查询现状核实）。
    static SynonymMap defaults();

    /// 查找口语词对应术语表；不存在返回 nullptr。
    const std::vector<std::string>* find(const std::string& spoken) const;
};

/// BM25 查询扩展（纯函数，可独立单测）：
/// query 中包含某口语词（子串匹配，中文无词边界）→ 把对应术语追加进 terms。
/// 去重：terms 中已有、或本轮已追加的词项不重复加入；
/// 术语与查询原有词项相同也不加（扩展语义 = 补充，不放大权重）。
/// 返回实际追加的词项数（便于日志与测试断言）。
int expandTerms(const std::string& query, std::vector<std::string>& terms,
                const SynonymMap& map);

/// JSON 持久化（UTF-8）。
/// load：文件不存在 / 解析失败返回 false（out 已置 defaults()）；
///       单条映射字段缺失或类型不对时跳过该条，其余条目照常生效。
/// save：失败返回 false 并填 diagnostic。
bool load(const std::string& filePath, SynonymMap& out);
bool save(const std::string& filePath, const SynonymMap& data,
          std::string* diagnostic = nullptr);

} // namespace synonym
} // namespace rag
