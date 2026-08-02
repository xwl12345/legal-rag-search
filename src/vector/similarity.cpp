#include "vector/similarity.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace vector_engine {

void SimilarityEngine::addVector(int docIndex, const std::vector<double>& vec) {
    indices_.push_back(docIndex);
    vectors_.push_back(vec);
}

std::vector<VectorSearchResult> SimilarityEngine::search(
    const std::vector<double>& queryVec, int topK)
{
    if (queryVec.empty() || vectors_.empty()) return {};

    // 计算所有向量的余弦相似度
    std::vector<std::pair<int, double>> scored;  // (vectorIdx, similarity)

    for (size_t i = 0; i < vectors_.size(); ++i) {
        double sim = cosineSimilarity(queryVec, vectors_[i]);
        scored.emplace_back(static_cast<int>(i), sim);
    }

    // 部分排序取 TopK
    int k = std::min(topK, static_cast<int>(scored.size()));
    std::partial_sort(
        scored.begin(), scored.begin() + k, scored.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );

    std::vector<VectorSearchResult> results;
    for (int i = 0; i < k; ++i) {
        VectorSearchResult r;
        r.index = indices_[scored[i].first];
        r.similarity = scored[i].second;
        results.push_back(r);
    }

    return results;
}

double SimilarityEngine::cosineSimilarity(
    const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size() || a.empty()) return 0.0;

    double dot = 0.0;
    double normA = 0.0;
    double normB = 0.0;

    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    double denom = std::sqrt(normA) * std::sqrt(normB);
    if (denom < 1e-12) return 0.0;

    return dot / denom;
}

} // namespace vector_engine
