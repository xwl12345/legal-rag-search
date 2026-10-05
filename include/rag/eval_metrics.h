#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace rag {

/// 检索质量指标计算（T4）——纯函数，质量分析页 / eval_retrieval / 单测共用。
///
/// 检索结果是块级（一个文档多个块），指标按文档级计算：
/// 同一文档的多个块只保留最高名次（去重保序）。

/// 四项核心指标（对单条查询）
struct QueryMetrics {
    double p5 = 0.0;     // P@5    = Top-5 文档中相关文档数 / 5
    double hit5 = 0.0;   // Hit@5  = Success@5，Top-5 内至少命中一个相关文档（0/1）
    double r10 = 0.0;    // R@10   = Top-10 文档命中的相关文档数 / 相关文档总数
    double mrr = 0.0;    // MRR    = 1 / 首个相关文档名次（未命中为 0）
    int firstRank = 0;   // 首个相关文档名次（1 起；0 = 未命中）
};

/// 块级结果 → 文档级排名（去重保序：同文档多块取最高名次）
inline std::vector<std::string> docLevelRanking(const std::vector<std::string>& chunkDocIds) {
    std::vector<std::string> docRank;
    for (const auto& d : chunkDocIds) {
        if (std::find(docRank.begin(), docRank.end(), d) == docRank.end()) {
            docRank.push_back(d);
        }
    }
    return docRank;
}

/// 对单条查询计算指标
/// @param docRank  文档级排名（docLevelRanking 的输出）
/// @param relevant 相关文档 docId 集合
inline QueryMetrics computeQueryMetrics(const std::vector<std::string>& docRank,
                                        const std::vector<std::string>& relevant) {
    QueryMetrics m;
    const int relN = static_cast<int>(relevant.size());
    if (relN <= 0) return m;

    int hits5 = 0, hits10 = 0;
    for (int i = 0; i < static_cast<int>(docRank.size()); ++i) {
        const bool rel = std::find(relevant.begin(), relevant.end(), docRank[i]) != relevant.end();
        if (!rel) continue;
        if (i < 5)  ++hits5;
        if (i < 10) ++hits10;
        if (m.firstRank == 0) m.firstRank = i + 1;
    }

    m.p5 = static_cast<double>(hits5) / 5.0;
    m.hit5 = hits5 > 0 ? 1.0 : 0.0;
    m.r10 = static_cast<double>(hits10) / relN;
    m.mrr = m.firstRank > 0 ? 1.0 / m.firstRank : 0.0;
    return m;
}

/// 多条查询求宏平均（各查询指标直接平均）
inline QueryMetrics averageMetrics(const std::vector<QueryMetrics>& rows) {
    QueryMetrics avg;
    if (rows.empty()) return avg;
    for (const auto& r : rows) {
        avg.p5 += r.p5;
        avg.hit5 += r.hit5;
        avg.r10 += r.r10;
        avg.mrr += r.mrr;
    }
    const double n = static_cast<double>(rows.size());
    avg.p5 /= n; avg.hit5 /= n; avg.r10 /= n; avg.mrr /= n;
    return avg;
}

} // namespace rag
