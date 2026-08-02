#pragma once
#include "index/inverted_index.h"
#include "index/bm25_ranker.h"
#include "vector/embedding.h"
#include "vector/similarity.h"
#include "document/tokenizer.h"
#include "document/metadata.h"
#include <memory>
#include <vector>

namespace rag {

/// 单条检索结果（混合排序后）
struct SearchResult {
    std::string docId;
    int chunkIndex = 0;
    std::string content;
    double bm25Score = 0.0;
    double vectorScore = 0.0;
    double finalScore = 0.0;    // 加权融合后的分数
};

/// RAG 检索器：混合 BM25 + 向量检索
class Retriever {
public:
    Retriever();

    /// 向检索引擎添加文档
    void addDocument(const std::string& filePath);
    void addText(const std::string& text, const std::string& docId);

    /// 混合检索：BM25 + 向量
    std::vector<SearchResult> search(const std::string& query, int topK = 5);

    /// 设置 API Key（用于 embedding）
    void setApiKey(const std::string& key);

    /// 已索引文档数
    int docCount() const { return index_.totalDocs(); }

    /// 获取检索上下文（用于 AI 生成答案）
    std::string buildContext(const std::vector<SearchResult>& results,
                             int maxTokens = 2000);

    /// 获取文档元数据（案号、法院、日期等）
    const document::DocMetadata* getMetadata(const std::string& docId) const;

    /// 已导入的所有文档 ID
    std::vector<std::string> allDocIds() const;

private:
    document::Tokenizer tokenizer_;
    search_index::InvertedIndex index_;
    search_index::BM25Ranker bm25_;
    vector_engine::EmbeddingService embedding_;
    vector_engine::SimilarityEngine similarity_;

    // 存储所有文本块，按 (docId, chunkIndex) 索引
    std::unordered_map<std::string, std::string> chunkStore_;

    // 向量库索引 → (docId, chunkIndex) 的映射
    std::vector<std::pair<std::string, int>> vectorIndexMap_;

    // docId → 元数据
    std::unordered_map<std::string, document::DocMetadata> docMeta_;
};

} // namespace rag
