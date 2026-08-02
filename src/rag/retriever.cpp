#include "rag/retriever.h"
#include "document/parser.h"
#include "config/app_config.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>

namespace rag {

Retriever::Retriever() = default;

void Retriever::setApiKey(const std::string& key) {
    embedding_.setApiKey(key);
}

void Retriever::addDocument(const std::string& filePath) {
    // Read raw file content
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Extract filename as docId
    std::filesystem::path path(filePath);
    std::string docId = path.filename().string();

    // addText handles chunking + indexing
    addText(content, docId);
}

void Retriever::addText(const std::string& text, const std::string& docId) {
    if (text.empty() || docId.empty()) return;

    // 使用已有的 parser 来分块
    document::DocumentParser parser;
    auto chunks = parser.parseText(text, docId);

    for (const auto& chunk : chunks) {
        // 1. 分词
        auto terms = tokenizer_.cutForIndex(chunk.content);

        // 2. 建倒排索引
        index_.addDocument(chunk.docId, chunk.chunkIndex, terms);

        // 3. 存储文本块
        std::string key = chunk.docId + ":" + std::to_string(chunk.chunkIndex);
        chunkStore_[key] = chunk.content;
    }
}

std::vector<SearchResult> Retriever::search(const std::string& query, int topK) {
    // ── Step 1: BM25 关键词检索 ──
    auto queryTerms = tokenizer_.cutForIndex(query);
    auto bm25Results = bm25_.search(queryTerms, index_, std::max(topK * 2, 10));

    // ── Step 2: 向量语义检索 ──
    std::vector<SearchResult> combined;

    if (embedding_.isReady()) {
        try {
            auto queryVec = embedding_.embed(query);

            // 确保向量库和索引同步
            if (similarity_.size() != static_cast<size_t>(index_.totalDocs())) {
                // 重建向量库（从 chunkStore 生成 embedding）
                // 注意：这里简化处理，实际应该增量更新
                similarity_.clear();
                vectorIndexMap_.clear();

                auto allDocs = index_.allDocs();
                std::vector<std::string> allTexts;
                for (const auto& [docId, chunkIdx] : allDocs) {
                    std::string key = docId + ":" + std::to_string(chunkIdx);
                    auto it = chunkStore_.find(key);
                    if (it != chunkStore_.end()) {
                        allTexts.push_back(it->second);
                        vectorIndexMap_.emplace_back(docId, chunkIdx);
                    }
                }

                if (!allTexts.empty()) {
                    // 批量获取 embeddings（每批最多 20 个）
                    for (size_t i = 0; i < allTexts.size(); i += 20) {
                        size_t batchEnd = std::min(i + 20, allTexts.size());
                        std::vector<std::string> batch(
                            allTexts.begin() + i,
                            allTexts.begin() + batchEnd
                        );
                        auto vecs = embedding_.embedBatch(batch);
                        for (size_t j = 0; j < vecs.size(); ++j) {
                            similarity_.addVector(static_cast<int>(i + j), vecs[j]);
                        }
                    }
                }
            }

            if (similarity_.size() > 0) {
                auto vectorResults = similarity_.search(queryVec, std::max(topK * 2, 10));

                // ── Step 3: 混合加权排序 ──
                // 用 map 合并两种分数
                std::unordered_map<std::string, double> bm25Scores;
                std::unordered_map<std::string, double> vectorScores;

                for (const auto& r : bm25Results) {
                    std::string key = r.docId + ":" + std::to_string(r.chunkIndex);
                    bm25Scores[key] = r.score;
                }

                for (const auto& r : vectorResults) {
                    if (r.index >= 0 && static_cast<size_t>(r.index) < vectorIndexMap_.size()) {
                        auto [docId, chunkIdx] = vectorIndexMap_[r.index];
                        std::string key = docId + ":" + std::to_string(chunkIdx);
                        vectorScores[key] = r.similarity;
                    }
                }

                // 收集所有出现过的文档
                std::unordered_set<std::string> allKeys;
                for (const auto& [k, _] : bm25Scores) allKeys.insert(k);
                for (const auto& [k, _] : vectorScores) allKeys.insert(k);

                // 归一化并加权
                // 先找最大最小值
                double bm25Max = 0.0, vecMax = 0.0;
                for (const auto& [_, s] : bm25Scores) bm25Max = std::max(bm25Max, s);
                for (const auto& [_, s] : vectorScores) vecMax = std::max(vecMax, s);

                std::vector<std::pair<std::string, double>> scored;
                for (const auto& key : allKeys) {
                    double bm25Norm = bm25Max > 0 ? (bm25Scores[key] / bm25Max) : 0.0;
                    double vecNorm = vecMax > 0 ? (vectorScores[key] / vecMax) : 0.0;
                    double finalScore = config::BM25_WEIGHT * bm25Norm +
                                        config::VECTOR_WEIGHT * vecNorm;
                    scored.emplace_back(key, finalScore);
                }

                std::partial_sort(
                    scored.begin(),
                    scored.begin() + std::min(topK, static_cast<int>(scored.size())),
                    scored.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; }
                );

                for (int i = 0; i < std::min(topK, static_cast<int>(scored.size())); ++i) {
                    const auto& [key, finalScore] = scored[i];
                    auto colonPos = key.rfind(':');
                    SearchResult sr;
                    sr.docId = key.substr(0, colonPos);
                    sr.chunkIndex = std::stoi(key.substr(colonPos + 1));
                    sr.finalScore = finalScore;
                    sr.bm25Score = bm25Scores[key];
                    sr.vectorScore = vectorScores[key];

                    auto it = chunkStore_.find(key);
                    if (it != chunkStore_.end()) {
                        sr.content = it->second;
                    }

                    combined.push_back(sr);
                }
            }
        } catch (const std::exception& e) {
            // embedding 失败时回退到纯 BM25
        }
    }

    // ── 回退：如果向量检索不可用，只用 BM25 ──
    if (combined.empty()) {
        for (const auto& r : bm25Results) {
            SearchResult sr;
            sr.docId = r.docId;
            sr.chunkIndex = r.chunkIndex;
            sr.bm25Score = r.score;
            sr.finalScore = r.score;
            std::string key = r.docId + ":" + std::to_string(r.chunkIndex);
            auto it = chunkStore_.find(key);
            if (it != chunkStore_.end()) {
                sr.content = it->second;
            }
            combined.push_back(sr);
        }

        // 截取 TopK
        if (static_cast<int>(combined.size()) > topK) {
            combined.resize(topK);
        }
    }

    return combined;
}

std::string Retriever::buildContext(const std::vector<SearchResult>& results,
                                     int maxTokens)
{
    std::ostringstream oss;
    oss << "以下是与用户问题相关的文档内容：\n\n";

    int totalChars = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::string snippet = r.content;

        // 截断过长的内容（粗略按字符数估计 token）
        if (totalChars + static_cast<int>(snippet.size()) > maxTokens * 4) {
            snippet = snippet.substr(0, maxTokens * 4 - totalChars) + "...";
        }

        oss << "【来源 " << (i + 1) << "】" << r.docId
            << " (相关度: " << std::fixed << std::setprecision(2) << r.finalScore << ")\n";
        oss << snippet << "\n\n";

        totalChars += snippet.size();
        if (totalChars >= maxTokens * 4) break;
    }

    return oss.str();
}

} // namespace rag
