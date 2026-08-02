#pragma once
#include <vector>
#include <string>
#include <utility>

namespace vector_engine {

/// 余弦相似度搜索结果
struct VectorSearchResult {
    int index = 0;              // 向量库中的索引
    double similarity = 0.0;    // 余弦相似度
};

/// 向量相似度计算
class SimilarityEngine {
public:
    /// 添加文档向量到向量库
    void addVector(int docIndex, const std::vector<double>& vec);

    /// 查询与目标向量最相似的 TopK 个文档
    std::vector<VectorSearchResult> search(const std::vector<double>& queryVec,
                                           int topK = 5);

    /// 计算两个向量的余弦相似度
    static double cosineSimilarity(const std::vector<double>& a,
                                   const std::vector<double>& b);

    /// 向量库大小
    size_t size() const { return vectors_.size(); }

    /// 清空
    void clear() { vectors_.clear(); indices_.clear(); }

private:
    // index → vector
    std::vector<std::vector<double>> vectors_;
    // vector库中位置 → 原始文档索引
    std::vector<int> indices_;
};

} // namespace vector_engine
