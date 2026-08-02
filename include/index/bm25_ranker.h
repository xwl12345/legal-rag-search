#pragma once
#include "inverted_index.h"
#include <vector>
#include <string>

namespace search_index {

/// BM25 排序结果
struct RankedResult {
    std::string docId;
    int chunkIndex = 0;
    double score = 0.0;
    std::string content;   // 文本块内容（用于后续展示）
};

/// BM25 排序器
class BM25Ranker {
public:
    /// k1: 词频饱和参数 (默认 1.5)
    /// b:  文档长度归一化参数 (默认 0.75)
    BM25Ranker(double k1 = 1.5, double b = 0.75);

    /// 计算查询与所有文档的 BM25 得分，返回 TopK
    std::vector<RankedResult> search(const std::vector<std::string>& queryTerms,
                                     const InvertedIndex& index,
                                     int topK = 5);

    /// 计算单个文档的 BM25 分数
    double score(const std::vector<std::string>& queryTerms,
                 const std::string& docId,
                 int chunkIndex,
                 const InvertedIndex& index) const;

private:
    double k1_;
    double b_;
};

} // namespace index
