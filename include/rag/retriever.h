#pragma once
#include "index/inverted_index.h"
#include "index/bm25_ranker.h"
#include "index/index_store.h"
#include "vector/embedding.h"
#include "vector/similarity.h"
#include "document/tokenizer.h"
#include "document/parser.h"
#include "document/metadata.h"
#include <cstdint>
#include <functional>
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

/// 单个文件的导入结果。
struct ImportResult {
    bool imported = false;
    std::string documentId;
    int chunksAdded = 0;
    document::ParseSource source = document::ParseSource::None;
    std::string diagnostic;
    bool cancelled = false;   // 用户主动取消（区别于失败）
};

/// 文档库列表项：由 Retriever 汇总，供展示层只读消费（T1 文档库页 / T10 全文阅读）
struct DocumentInfo {
    std::string docId;              // 文档 ID（文件名）
    std::string sourcePath;         // 原始文件路径
    std::string importedAt;         // 导入时间 yyyy-MM-dd HH:mm:ss
    int chunkCount = 0;             // 文本块数
    std::uint64_t byteSize = 0;     // 原文字节数
    bool ocr = false;               // 是否经 OCR 识别
    document::ResultTendency tendency = document::ResultTendency::Unknown;  // 第 8 类
};

/// 索引落盘 / 恢复的结果
struct PersistResult {
    bool ok = false;
    int documentCount = 0;
    int chunkCount = 0;
    std::uint64_t bytes = 0;
    long long elapsedMs = -1;       // 恢复耗时（毫秒），落盘时无意义
    std::string diagnostic;
};

/// 内存中的文档完整记录（导入与落盘恢复共用）
struct StoredDocument {
    std::string docId;
    std::string sourcePath;
    std::string importedAt;
    bool ocr = false;
    document::DocMetadata metadata;
    std::string fullText;             // 整篇文书原文（T10 数据源）
    std::vector<std::string> chunks;  // chunks[i] 对应 chunkIndex = i
};

/// RAG 检索器：混合 BM25 + 向量检索
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本类属于"核心检索链路"，**不得** include 任何 ui/ 头、不得持有页面指针。
///   展示层只能对 Retriever 做单向只读调用（documentInfos / allDocIds /
///   getMetadata / getFullText…），删除任意页面不影响本类。
class Retriever {
public:
    Retriever();

    /// 向检索引擎添加文档，返回实际索引结果
    /// @param cancelled 每约 200ms 轮询一次，返回 true 时取消 OCR 回退流程
    /// @param onPage    OCR 逐页识别的进度回调（当前页号、总页号）
    ImportResult addDocument(const std::string& filePath,
                             const std::function<bool()>& cancelled = {},
                             const std::function<void(int, int)>& onPage = {});
    void addText(const std::string& text, const std::string& docId);

    /// 混合检索：BM25 + 向量
    std::vector<SearchResult> search(const std::string& query, int topK = 5);

    /// 设置 API Key（用于 embedding）
    void setApiKey(const std::string& key);

    /// ⚠️ 注意：本函数走 InvertedIndex::totalDocs()，而 totalDocs_ 是按
    /// (docId, chunkIndex) 逐块累加的——**它返回的是文本块数，不是文档数**。
    /// 需要真实文档数请用 allDocIds().size() 或 documentCount()。
    int docCount() const { return index_.totalDocs(); }

    /// 文本块总数
    int chunkCount() const { return static_cast<int>(chunkStore_.size()); }

    /// 真实文档数
    int documentCount() const { return static_cast<int>(documents_.size()); }

    /// 获取检索上下文（用于 AI 生成答案）
    std::string buildContext(const std::vector<SearchResult>& results,
                             int maxTokens = 2000);

    /// 获取文档元数据（案号、法院、日期等）
    const document::DocMetadata* getMetadata(const std::string& docId) const;

    /// 已导入的所有文档 ID（按 docId 排序，稳定可复现）
    std::vector<std::string> allDocIds() const;

    // ── T1 新增：文档库只读视图 ──

    /// 文档库列表项（按 docId 排序）
    std::vector<DocumentInfo> documentInfos() const;

    /// 单篇文档信息；不存在返回 false
    bool getDocumentInfo(const std::string& docId, DocumentInfo& out) const;

    /// 整篇文书原文（T10 全文阅读数据源）；不存在返回 false
    bool getFullText(const std::string& docId, std::string& out) const;

    /// 某个文本块的原文；不存在返回 false
    bool getChunk(const std::string& docId, int chunkIndex, std::string& out) const;

    // ── T1 新增：删除与持久化 ──

    /// 删除单篇文档：倒排索引（逐块）、文本块、向量槽位、元数据、全文一并清理。
    /// 返回实际移除的文本块数；docId 不存在返回 0。
    int removeDocument(const std::string& docId);

    /// 清空全部索引（内存；alsoDeletePersistedFile 为真时同时删除落盘文件）
    void clearAll(bool alsoDeletePersistedFile = true);

    /// 把当前索引落盘
    PersistResult saveIndex() const;

    /// 从落盘文件恢复索引（内部会先清空内存索引，不删除落盘文件）
    PersistResult loadIndex();

    /// 落盘文件路径
    std::string indexFilePath() const;

    /// 改写落盘文件路径（测试/多实例隔离用；应用层不调用，保持默认路径）
    void setIndexFilePath(const std::string& path);

    /// 落盘文件是否存在
    bool hasPersistedIndex() const;

    /// 是否已成功从磁盘恢复（供状态栏/测试观察）
    bool restoredFromDisk() const { return restoredFromDisk_; }

    /// 上次恢复耗时（毫秒），未恢复过为 -1
    long long lastLoadMs() const { return lastLoadMs_; }

private:
    /// 把一篇文档的全部内容写入内存索引（导入与落盘恢复共用）
    void indexDocument(const StoredDocument& doc);

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

    // ── T1：文档级信息（供文档库页与全文阅读页只读消费）──
    std::unordered_map<std::string, StoredDocument> documents_;
    std::vector<std::string> documentOrder_;   // 按 docId 排序，稳定顺序

    // ── T1：持久化 ──
    std::unique_ptr<index_store::IndexStore> store_;
    bool restoredFromDisk_ = false;
    long long lastLoadMs_ = -1;
};

} // namespace rag
