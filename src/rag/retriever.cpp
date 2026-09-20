#include "rag/retriever.h"
#include "document/parser.h"
#include "document/metadata.h"
#include "config/app_config.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace {

bool isLocationQuestion(const std::string& query) {
    static constexpr std::array<const char*, 4> locationWords = {
        "哪里", "哪儿", "何处", "在哪"
    };

    return std::any_of(locationWords.begin(), locationWords.end(),
                       [&query](const char* word) {
                           return query.find(word) != std::string::npos;
                       });
}

void expandLegalLocationTerms(const std::string& query,
                              std::vector<std::string>& terms) {
    if (!isLocationQuestion(query)) return;

    std::unordered_set<std::string> seen(terms.begin(), terms.end());
    static constexpr std::array<const char*, 5> locationTerms = {
        "住所地", "住址", "所在地", "地址", "坐落于"
    };
    for (const char* term : locationTerms) {
        if (seen.insert(term).second) {
            terms.emplace_back(term);
        }
    }
}

/// 文本块在 chunkStore_ 中的键：(docId, chunkIndex) 唯一确定一个块。
std::string chunkKey(const std::string& docId, int chunkIndex) {
    return docId + ":" + std::to_string(chunkIndex);
}

} // namespace

namespace rag {

Retriever::Retriever()
    : store_(std::make_unique<index_store::IndexStore>()) {}

void Retriever::setApiKey(const std::string& key) {
    embedding_.setApiKey(key);
}

// ────────────────────────────────────────────────────────────────
// 索引写入（导入与落盘恢复共用）
// ────────────────────────────────────────────────────────────────

void Retriever::indexDocument(const StoredDocument& doc) {
    for (size_t i = 0; i < doc.chunks.size(); ++i) {
        const int chunkIndex = static_cast<int>(i);
        const std::string& content = doc.chunks[i];

        auto terms = tokenizer_.cutForIndex(content);
        index_.addDocument(doc.docId, chunkIndex, terms);
        chunkStore_[chunkKey(doc.docId, chunkIndex)] = content;
    }
}

ImportResult Retriever::addDocument(const std::string& filePath,
                                    const std::function<bool()>& cancelled,
                                    const std::function<void(int, int)>& onPage) {
    // 使用 DocumentParser::parseWithResult() 统一处理所有文件类型
    // — 文本文件：直接读取
    // — PDF：PdfExtractor 提取文本层 → OCR 回退（扫描件）
    document::DocumentParser parser;
    auto parseResult = parser.parseWithResult(filePath, cancelled, onPage);

    if (!parseResult.isSuccess()) {
        const bool userCancelled = parseResult.status == document::ParseStatus::OcrCancelled;
        return {false, "", 0, parseResult.source, parseResult.diagnostic, userCancelled};
    }

    auto& chunks = parseResult.chunks;
    if (chunks.empty()) {
        return {false, "", 0, parseResult.source, "解析结果为空", false};
    }

    const std::string docId = chunks[0].docId;

    // 同一 docId 重复导入：先清掉旧记录，避免产生"幽灵块"
    // （倒排里留着旧块、chunkStore_ 里却已被新块覆盖）。
    if (documents_.find(docId) != documents_.end()) {
        removeDocument(docId);
    }

    StoredDocument doc;
    doc.docId = docId;
    doc.sourcePath = filePath;
    doc.importedAt = index_store::nowTimestamp();
    doc.ocr = (parseResult.source == document::ParseSource::Ocr);

    // 整篇原文（T10 全文阅读的数据源；分块只够判断相关性）
    for (const auto& chunk : chunks) {
        doc.fullText += chunk.content;
        doc.chunks.push_back(chunk.content);
    }

    doc.metadata = document::MetadataExtractor::extract(doc.fullText);

    indexDocument(doc);

    if (!doc.metadata.isEmpty()) {
        docMeta_[docId] = doc.metadata;
    }
    documents_[docId] = std::move(doc);
    documentOrder_.push_back(docId);
    std::sort(documentOrder_.begin(), documentOrder_.end());

    return {true, docId, static_cast<int>(chunks.size()), parseResult.source, ""};
}

void Retriever::addText(const std::string& text, const std::string& docId) {
    if (text.empty() || docId.empty()) return;

    if (documents_.find(docId) != documents_.end()) {
        removeDocument(docId);
    }

    // 使用已有的 parser 来分块
    document::DocumentParser parser;
    auto chunks = parser.parseText(text, docId);

    StoredDocument doc;
    doc.docId = docId;
    doc.sourcePath = "";
    doc.importedAt = index_store::nowTimestamp();
    doc.ocr = false;
    doc.fullText = text;

    for (const auto& chunk : chunks) {
        doc.chunks.push_back(chunk.content);
    }

    doc.metadata = document::MetadataExtractor::extract(doc.fullText);

    indexDocument(doc);

    if (!doc.metadata.isEmpty()) {
        docMeta_[docId] = doc.metadata;
    }
    documents_[docId] = std::move(doc);
    documentOrder_.push_back(docId);
    std::sort(documentOrder_.begin(), documentOrder_.end());
}

// ────────────────────────────────────────────────────────────────
// 检索
// ────────────────────────────────────────────────────────────────

std::vector<SearchResult> Retriever::search(const std::string& query, int topK) {
    // ── Step 1: BM25 关键词检索 ──
    auto queryTerms = tokenizer_.cutForIndex(query);
    // 法律文书通常用“住所地”等字段表达地点，补充自然语言位置问法。
    expandLegalLocationTerms(query, queryTerms);
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
                    std::string key = chunkKey(docId, chunkIdx);
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
                    std::string key = chunkKey(r.docId, r.chunkIndex);
                    bm25Scores[key] = r.score;
                }

                for (const auto& r : vectorResults) {
                    if (r.index >= 0 && static_cast<size_t>(r.index) < vectorIndexMap_.size()) {
                        auto [docId, chunkIdx] = vectorIndexMap_[r.index];
                        std::string key = chunkKey(docId, chunkIdx);
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
            std::string key = chunkKey(r.docId, r.chunkIndex);
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

// ────────────────────────────────────────────────────────────────
// 只读视图（文档库页 / 全文阅读页消费）
// ────────────────────────────────────────────────────────────────

const document::DocMetadata* Retriever::getMetadata(const std::string& docId) const {
    auto it = docMeta_.find(docId);
    if (it != docMeta_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<std::string> Retriever::allDocIds() const {
    // 按 documentOrder_ 输出，保证跨次运行顺序稳定（unordered_map 遍历顺序不可依赖）
    std::vector<std::string> ids;
    ids.reserve(documentOrder_.size());
    for (const auto& docId : documentOrder_) {
        if (documents_.find(docId) != documents_.end()) {
            ids.push_back(docId);
        }
    }
    return ids;
}

std::vector<DocumentInfo> Retriever::documentInfos() const {
    std::vector<DocumentInfo> infos;
    infos.reserve(documentOrder_.size());

    for (const auto& docId : documentOrder_) {
        auto it = documents_.find(docId);
        if (it == documents_.end()) continue;
        const auto& doc = it->second;

        DocumentInfo info;
        info.docId = doc.docId;
        info.sourcePath = doc.sourcePath;
        info.importedAt = doc.importedAt;
        info.chunkCount = static_cast<int>(doc.chunks.size());
        info.byteSize = static_cast<std::uint64_t>(doc.fullText.size());
        info.ocr = doc.ocr;
        info.tendency = doc.metadata.tendency;
        infos.push_back(std::move(info));
    }
    return infos;
}

bool Retriever::getDocumentInfo(const std::string& docId, DocumentInfo& out) const {
    auto it = documents_.find(docId);
    if (it == documents_.end()) return false;

    const auto& doc = it->second;
    out.docId = doc.docId;
    out.sourcePath = doc.sourcePath;
    out.importedAt = doc.importedAt;
    out.chunkCount = static_cast<int>(doc.chunks.size());
    out.byteSize = static_cast<std::uint64_t>(doc.fullText.size());
    out.ocr = doc.ocr;
    out.tendency = doc.metadata.tendency;
    return true;
}

bool Retriever::getFullText(const std::string& docId, std::string& out) const {
    auto it = documents_.find(docId);
    if (it == documents_.end()) return false;
    out = it->second.fullText;
    return true;
}

bool Retriever::getChunk(const std::string& docId, int chunkIndex, std::string& out) const {
    auto it = chunkStore_.find(chunkKey(docId, chunkIndex));
    if (it == chunkStore_.end()) return false;
    out = it->second;
    return true;
}

// ────────────────────────────────────────────────────────────────
// 删除
// ────────────────────────────────────────────────────────────────

int Retriever::removeDocument(const std::string& docId) {
    auto it = documents_.find(docId);
    if (it == documents_.end()) return 0;

    const StoredDocument& doc = it->second;
    const int chunkTotal = static_cast<int>(doc.chunks.size());

    // 1. 倒排索引：逐块摘除 posting，并同步 totalDocs_
    for (int i = 0; i < chunkTotal; ++i) {
        index_.removeChunk(docId, i);
        chunkStore_.erase(chunkKey(docId, i));
    }

    // 2. 元数据与文档记录
    docMeta_.erase(docId);
    documentOrder_.erase(
        std::remove(documentOrder_.begin(), documentOrder_.end(), docId),
        documentOrder_.end());
    documents_.erase(it);

    // 3. 向量库：槽位与块索引强绑定，删块后按下标对应关系失效，
    //    直接作废整个向量库，下次 search() 时按现有块重建（懒加载）。
    similarity_.clear();
    vectorIndexMap_.clear();

    return chunkTotal;
}

void Retriever::clearAll(bool alsoDeletePersistedFile) {
    index_.clear();
    chunkStore_.clear();
    docMeta_.clear();
    documents_.clear();
    documentOrder_.clear();
    similarity_.clear();
    vectorIndexMap_.clear();
    restoredFromDisk_ = false;
    lastLoadMs_ = -1;

    if (alsoDeletePersistedFile && store_) {
        store_->remove();
    }
}

// ────────────────────────────────────────────────────────────────
// 持久化
// ────────────────────────────────────────────────────────────────

PersistResult Retriever::saveIndex() const {
    PersistResult result;

    if (!store_) {
        result.diagnostic = "持久化层未初始化";
        return result;
    }

    // 按 docId 排序写出，保证落盘内容可复现（便于 diff 与测试比对）
    std::vector<index_store::StoredDocument> records;
    records.reserve(documentOrder_.size());

    for (const auto& docId : documentOrder_) {
        auto it = documents_.find(docId);
        if (it == documents_.end()) continue;
        const StoredDocument& doc = it->second;

        index_store::StoredDocument record;
        record.docId = doc.docId;
        record.sourcePath = doc.sourcePath;
        record.importedAt = doc.importedAt;
        record.chunkCount = static_cast<int>(doc.chunks.size());
        record.byteSize = static_cast<std::uint64_t>(doc.fullText.size());
        record.ocr = doc.ocr;
        record.metadata = doc.metadata;
        record.fullText = doc.fullText;
        record.chunks = doc.chunks;

        records.push_back(std::move(record));
    }

    auto saveResult = store_->save(records);
    result.ok = saveResult.ok;
    result.bytes = saveResult.bytesWritten;
    result.diagnostic = saveResult.diagnostic;
    result.documentCount = static_cast<int>(records.size());

    int chunks = 0;
    for (const auto& r : records) {
        chunks += static_cast<int>(r.chunks.size());
    }
    result.chunkCount = chunks;

    return result;
}

PersistResult Retriever::loadIndex() {
    PersistResult result;

    if (!store_) {
        result.diagnostic = "持久化层未初始化";
        return result;
    }

    const auto begin = std::chrono::steady_clock::now();

    std::vector<index_store::StoredDocument> records;
    auto loadResult = store_->load(records);

    // 无论成败，先把内存清干净——避免失败时残留半套旧索引，
    // 导致"文档列表是新的、倒排是旧的"这类不一致。
    const bool hadFile = store_->exists();
    clearAll(false);

    result.ok = loadResult.ok;
    result.diagnostic = loadResult.diagnostic;
    result.documentCount = loadResult.documentCount;
    result.chunkCount = loadResult.chunkCount;
    result.bytes = hadFile ? store_->fileSize() : 0;

    if (!loadResult.ok) {
        const auto end = std::chrono::steady_clock::now();
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               end - begin).count();
        lastLoadMs_ = result.elapsedMs;
        restoredFromDisk_ = false;
        return result;
    }

    for (auto& record : records) {
        StoredDocument doc;
        doc.docId = record.docId;
        doc.sourcePath = record.sourcePath;
        doc.importedAt = record.importedAt;
        doc.ocr = record.ocr;
        doc.metadata = record.metadata;
        doc.fullText = record.fullText;
        doc.chunks = record.chunks;

        if (doc.chunks.empty()) continue;

        indexDocument(doc);

        if (!doc.metadata.isEmpty()) {
            docMeta_[doc.docId] = doc.metadata;
        }
        const std::string id = doc.docId;
        documents_[id] = std::move(doc);
        documentOrder_.push_back(id);
    }
    std::sort(documentOrder_.begin(), documentOrder_.end());

    const auto end = std::chrono::steady_clock::now();
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           end - begin).count();
    lastLoadMs_ = result.elapsedMs;
    restoredFromDisk_ = !documents_.empty();

    // 以实际重建结果为准，防止文件头记录数与内容不符时误导调用方
    result.documentCount = static_cast<int>(documents_.size());
    result.chunkCount = static_cast<int>(chunkStore_.size());

    return result;
}

std::string Retriever::indexFilePath() const {
    return store_ ? store_->filePath() : std::string();
}

void Retriever::setIndexFilePath(const std::string& path) {
    store_ = std::make_unique<index_store::IndexStore>(path);
}

bool Retriever::hasPersistedIndex() const {
    return store_ && store_->exists();
}

} // namespace rag
