#include "index/bm25_ranker.h"
#include <cmath>
#include <algorithm>
#include <unordered_set>

namespace search_index {

BM25Ranker::BM25Ranker(double k1, double b)
    : k1_(k1), b_(b)
{
}

std::vector<RankedResult> BM25Ranker::search(
    const std::vector<std::string>& queryTerms,
    const InvertedIndex& index,
    int topK)
{
    if (queryTerms.empty()) return {};

    // 计算所有相关文档的 BM25 分数
    std::unordered_map<std::string, double> scores;  // docKey → score

    for (const auto& term : queryTerms) {
        const auto* postings = index.getPostings(term);
        if (!postings) continue;

        double idf = std::log(
            (index.totalDocs() - postings->size() + 0.5) /
            (postings->size() + 0.5) + 1.0
        );

        for (const auto& posting : *postings) {
            std::string docKey = posting.docId + ":" + std::to_string(posting.chunkIndex);
            int dl = index.docLength(posting.docId, posting.chunkIndex);
            double avgdl = index.avgDocLength();

            double tfComponent =
                (posting.termFreq * (k1_ + 1.0)) /
                (posting.termFreq + k1_ * (1.0 - b_ + b_ * dl / std::max(avgdl, 1.0)));

            scores[docKey] += idf * tfComponent;
        }
    }

    // 排序取 TopK
    std::vector<std::pair<std::string, double>> sorted;
    for (const auto& [key, score] : scores) {
        sorted.emplace_back(key, score);
    }

    std::partial_sort(
        sorted.begin(),
        sorted.begin() + std::min(topK, static_cast<int>(sorted.size())),
        sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; }
    );

    std::vector<RankedResult> results;
    for (int i = 0; i < std::min(topK, static_cast<int>(sorted.size())); ++i) {
        const auto& [key, score] = sorted[i];
        auto colonPos = key.rfind(':');
        RankedResult r;
        r.docId = key.substr(0, colonPos);
        r.chunkIndex = std::stoi(key.substr(colonPos + 1));
        r.score = score;
        results.push_back(r);
    }

    return results;
}

double BM25Ranker::score(const std::vector<std::string>& queryTerms,
                          const std::string& docId,
                          int chunkIndex,
                          const InvertedIndex& index) const
{
    double total = 0.0;
    int dl = index.docLength(docId, chunkIndex);
    double avgdl = index.avgDocLength();

    for (const auto& term : queryTerms) {
        const auto* postings = index.getPostings(term);
        if (!postings) continue;

        double idf = std::log(
            (index.totalDocs() - postings->size() + 0.5) /
            (postings->size() + 0.5) + 1.0
        );

        // 找到该文档的词频
        for (const auto& p : *postings) {
            if (p.docId == docId && p.chunkIndex == chunkIndex) {
                double tfComponent =
                    (p.termFreq * (k1_ + 1.0)) /
                    (p.termFreq + k1_ * (1.0 - b_ + b_ * dl / std::max(avgdl, 1.0)));
                total += idf * tfComponent;
                break;
            }
        }
    }

    return total;
}

} // namespace index
