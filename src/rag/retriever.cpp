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
    if (embedding_.apiKey() == key) {
        return;  // 同 Key 重复下发（如启动时从配置加载）不清缓存，避免无谓的全库重算
    }
    embedding_.setApiKey(key);
    // Key 指向的服务/配额可能不同，同一模型名下旧向量不再可信：
    // 失效缓存，下次向量检索按新配置懒重建（P0-4）。
    similarity_.clear();
    vectorIndexMap_.clear();
}

// ── T3 配置中心：运行时热更新 ──

void Retriever::setSearchParams(double k1, double b,
                                double bm25Weight, double vectorWeight) {
    bm25_.setParams(k1, b);
    bm25Weight_ = bm25Weight;
    vectorWeight_ = vectorWeight;
}

void Retriever::setChunkParams(int maxSize, int overlap) {
    chunkMaxSize_ = maxSize;
    chunkOverlap_ = overlap;
}

void Retriever::setEmbeddingEndpoint(const std::string& baseUrl, const std::string& model) {
    // setEndpoint 对空串字段保持原值，先算出生效值再判断是否真的变了
    const std::string newBase = baseUrl.empty() ? embedding_.apiBaseUrl() : baseUrl;
    const std::string newModel = model.empty() ? embedding_.model() : model;
    if (newBase == embedding_.apiBaseUrl() && newModel == embedding_.model()) {
        return;  // 未变化（如启动时按配置原样下发）不动缓存
    }
    embedding_.setEndpoint(baseUrl, model);
    // 模型/服务变了，旧模型的文档向量对新查询向量毫无意义（跨维度恒 0、
    // 同维度是纯噪声分）——必须失效缓存，下次向量检索按新配置懒重建（P0-4）。
    // 这让「改 Key/模型后无需重新导入，下次检索自动按新配置重算」的注释承诺成真。
    similarity_.clear();
    vectorIndexMap_.clear();
}

std::string Retriever::embeddingHost() const {
    // 展示用：从规范化后的服务地址取域名部分（https://api.siliconflow.cn → api.siliconflow.cn）
    std::string host = embedding_.apiBaseUrl();
    const std::string scheme = "://";
    const auto pos = host.find(scheme);
    if (pos != std::string::npos) {
        host = host.substr(pos + scheme.size());
    }
    while (!host.empty() && host.back() == '/') {
        host.pop_back();
    }
    return host;
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

        // T5：角色与块同键同生命周期（缺省 Unknown，兼容旧落盘文件）
        document::ChunkRole role = document::ChunkRole::Unknown;
        if (i < doc.chunkRoles.size()) {
            role = doc.chunkRoles[i];
        }
        chunkRoles_[chunkKey(doc.docId, chunkIndex)] = role;
    }
}

ImportResult Retriever::addDocument(const std::string& filePath,
                                    const std::function<bool()>& cancelled,
                                    const std::function<void(int, int)>& onPage) {
    // 使用 DocumentParser::parseWithResult() 统一处理所有文件类型
    // — 文本文件：直接读取
    // — PDF：PdfExtractor 提取文本层 → OCR 回退（扫描件）
    document::DocumentParser parser;
    parser.setChunkParams(chunkMaxSize_, chunkOverlap_);   // T3：分块参数仅对本次导入生效
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
    // P0-5：直接存解析出的原文——旧实现逐块 += 拼接，而相邻块带 50 字节 overlap，
    // 拼出的"全文"每 512 字节就重复 50 字节，全文阅读 / byteSize / 元数据提取输入全被污染。
    doc.fullText = std::move(parseResult.content);
    doc.chunkRoles.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        doc.chunks.push_back(chunk.content);
        doc.chunkRoles.push_back(chunk.role);   // T5：解析时已打好的角色
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
    parser.setChunkParams(chunkMaxSize_, chunkOverlap_);   // T3：分块参数仅对本次导入生效
    auto chunks = parser.parseText(text, docId);

    StoredDocument doc;
    doc.docId = docId;
    doc.sourcePath = "";
    doc.importedAt = index_store::nowTimestamp();
    doc.ocr = false;
    doc.fullText = text;

    for (const auto& chunk : chunks) {
        doc.chunks.push_back(chunk.content);
        doc.chunkRoles.push_back(chunk.role);   // T5
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
    // 默认路 = 加权融合（T0 以来的行为；T4 起显式走 searchWithMode）
    return searchWithMode(query, topK, SearchMode::WeightedFusion);
}

std::vector<std::pair<std::string, double>> Retriever::rrfFuse(
    const std::vector<std::pair<std::string, double>>& bm25Ranking,
    const std::vector<std::pair<std::string, double>>& vectorRanking,
    int topK, int k)
{
    // 倒数排名融合：score(d) = Σ_paths 1/(k + rank)，rank 从 1 计。
    // 只用名次不用分值——两路分数量纲不同（BM25 无上界 / 余弦 0~1），
    // 名次融合对量纲天然免疫，这是它与加权融合并列为对照算法的原因。
    std::unordered_map<std::string, double> rrf;
    const auto addPath = [&rrf, k](const std::vector<std::pair<std::string, double>>& ranking) {
        for (size_t i = 0; i < ranking.size(); ++i) {
            rrf[ranking[i].first] += 1.0 / static_cast<double>(k + static_cast<int>(i) + 1);
        }
    };
    addPath(bm25Ranking);
    addPath(vectorRanking);

    std::vector<std::pair<std::string, double>> scored(rrf.begin(), rrf.end());
    const int keep = std::min(topK, static_cast<int>(scored.size()));
    std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    scored.resize(keep);
    return scored;
}

std::vector<SearchResult> Retriever::searchWithMode(const std::string& query, int topK,
                                                    SearchMode mode)
{
    // ── Step 1: BM25 关键词检索（除 VectorOnly 外所有路都需要）──
    auto queryTerms = tokenizer_.cutForIndex(query);
    // 法律文书通常用“住所地”等字段表达地点，补充自然语言位置问法。
    expandLegalLocationTerms(query, queryTerms);
    const int width = std::max(topK * 2, 10);
    std::decay_t<decltype(bm25_.search(queryTerms, index_, width))> bm25Results{};
    if (mode != SearchMode::VectorOnly) {
        bm25Results = bm25_.search(queryTerms, index_, width);
    }

    // ── Step 2: 向量语义检索（未配置 / 异常 → available=false，由各路自行降级）──
    bool vectorAvailable = false;
    std::vector<vector_engine::VectorSearchResult> vectorResults;

    if (embedding_.isReady()) {
        try {
            auto queryVec = embedding_.embed(query);

            // 确保向量库和索引同步
            if (similarity_.size() != static_cast<size_t>(index_.totalDocs())) {
                // 重建向量库（从 chunkStore 批量生成 embedding，每批最多 20 条）
                // 注意：懒计算——导入文档不调 API，首次向量检索才批量补算；
                // 改 Key/模型后无需重新导入，下次检索自动按新配置重算（2026-10-05 实测）。
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
                vectorResults = similarity_.search(queryVec, width);
                vectorAvailable = !vectorResults.empty();
            }
        } catch (const std::exception&) {
            // embedding 失败：vectorAvailable 保持 false，各路按降级语义处理
            vectorAvailable = false;
        }
    }

    // chunkKey → 原始分（向量路可用时填充，供各融合路回填展示分）
    std::unordered_map<std::string, double> vectorScores;
    if (vectorAvailable) {
        for (const auto& r : vectorResults) {
            if (r.index >= 0 && static_cast<size_t>(r.index) < vectorIndexMap_.size()) {
                auto [docId, chunkIdx] = vectorIndexMap_[r.index];
                vectorScores[chunkKey(docId, chunkIdx)] = r.similarity;
            }
        }
    }

    // 把 chunkKey 转成 SearchResult 的公共收尾
    const auto makeResult = [this](const std::string& key) {
        SearchResult sr;
        auto colonPos = key.rfind(':');
        sr.docId = key.substr(0, colonPos);
        sr.chunkIndex = std::stoi(key.substr(colonPos + 1));
        auto it = chunkStore_.find(key);
        if (it != chunkStore_.end()) {
            sr.content = it->second;
        }
        auto roleIt = chunkRoles_.find(key);
        if (roleIt != chunkRoles_.end()) {
            sr.role = roleIt->second;   // T5
        }
        return sr;
    };

    std::vector<SearchResult> combined;

    switch (mode) {
    case SearchMode::Bm25Only:
        for (const auto& r : bm25Results) {
            SearchResult sr = makeResult(chunkKey(r.docId, r.chunkIndex));
            sr.bm25Score = r.score;
            sr.finalScore = r.score;
            combined.push_back(sr);
        }
        break;

    case SearchMode::VectorOnly:
        // 未配置 / 异常 → 返回空（调用方据此提示「已降级」），不静默伪装成 BM25
        for (const auto& [key, sim] : vectorScores) {
            SearchResult sr = makeResult(key);
            sr.vectorScore = sim;
            sr.finalScore = sim;
            combined.push_back(sr);
        }
        std::sort(combined.begin(), combined.end(),
                  [](const SearchResult& a, const SearchResult& b) {
                      return a.finalScore > b.finalScore;
                  });
        break;

    case SearchMode::WeightedFusion: {
        if (!vectorAvailable) {
            break;   // 走下方统一的 BM25 降级（与 T0 以来行为一致）
        }

        // ── 加权融合：各路分数除以本路最大值归一化后线性加权 ──
        std::unordered_map<std::string, double> bm25Scores;
        for (const auto& r : bm25Results) {
            bm25Scores[chunkKey(r.docId, r.chunkIndex)] = r.score;
        }

        // 收集所有出现过的文档
        std::unordered_set<std::string> allKeys;
        for (const auto& [k, _] : bm25Scores) allKeys.insert(k);
        for (const auto& [k, _] : vectorScores) allKeys.insert(k);

        double bm25Max = 0.0, vecMax = 0.0;
        for (const auto& [_, s] : bm25Scores) bm25Max = std::max(bm25Max, s);
        for (const auto& [_, s] : vectorScores) vecMax = std::max(vecMax, s);

        std::vector<std::pair<std::string, double>> scored;
        for (const auto& key : allKeys) {
            double bm25Norm = bm25Max > 0 ? (bm25Scores[key] / bm25Max) : 0.0;
            double vecNorm = vecMax > 0 ? (vectorScores[key] / vecMax) : 0.0;
            double finalScore = bm25Weight_ * bm25Norm + vectorWeight_ * vecNorm;
            scored.emplace_back(key, finalScore);
        }

        const int keep = std::min(topK, static_cast<int>(scored.size()));
        std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });

        for (int i = 0; i < keep; ++i) {
            const auto& [key, finalScore] = scored[i];
            SearchResult sr = makeResult(key);
            sr.bm25Score = bm25Scores[key];
            sr.vectorScore = vectorScores[key];
            sr.finalScore = finalScore;
            combined.push_back(sr);
        }
        break;
    }

    case SearchMode::RrfFusion: {
        if (!vectorAvailable) {
            break;   // 走下方统一的 BM25 降级
        }

        std::vector<std::pair<std::string, double>> bm25Ranking;
        for (const auto& r : bm25Results) {
            bm25Ranking.emplace_back(chunkKey(r.docId, r.chunkIndex), r.score);
        }
        std::vector<std::pair<std::string, double>> vectorRanking;
        for (const auto& r : vectorResults) {
            if (r.index >= 0 && static_cast<size_t>(r.index) < vectorIndexMap_.size()) {
                auto [docId, chunkIdx] = vectorIndexMap_[r.index];
                vectorRanking.emplace_back(chunkKey(docId, chunkIdx), r.similarity);
            }
        }

        for (const auto& [key, rrfScore] : rrfFuse(bm25Ranking, vectorRanking, topK)) {
            SearchResult sr = makeResult(key);
            auto bm = std::find_if(bm25Ranking.begin(), bm25Ranking.end(),
                                   [&](const auto& p) { return p.first == key; });
            auto ve = std::find_if(vectorRanking.begin(), vectorRanking.end(),
                                   [&](const auto& p) { return p.first == key; });
            sr.bm25Score = bm != bm25Ranking.end() ? bm->second : 0.0;
            sr.vectorScore = ve != vectorRanking.end() ? ve->second : 0.0;
            sr.finalScore = rrfScore;
            combined.push_back(sr);
        }
        break;
    }
    }

    // ── 降级：融合路向量不可用 / 融合结果为空时，只用 BM25 ──
    if (combined.empty() && mode != SearchMode::VectorOnly) {
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
            auto roleIt = chunkRoles_.find(key);
            if (roleIt != chunkRoles_.end()) {
                sr.role = roleIt->second;   // T5
            }
            combined.push_back(sr);
        }

        // 截取 TopK
        if (static_cast<int>(combined.size()) > topK) {
            combined.resize(topK);
        }
    }

    if (static_cast<int>(combined.size()) > topK) {
        combined.resize(topK);
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

document::ChunkRole Retriever::getChunkRole(const std::string& docId, int chunkIndex) const {
    auto it = chunkRoles_.find(chunkKey(docId, chunkIndex));
    return it != chunkRoles_.end() ? it->second : document::ChunkRole::Unknown;
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
        chunkRoles_.erase(chunkKey(docId, i));   // T5：角色与块同键同生命周期
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
    chunkRoles_.clear();
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
        record.chunkRoles.reserve(doc.chunks.size());
        for (int i = 0; i < static_cast<int>(doc.chunks.size()); ++i) {
            record.chunkRoles.push_back(
                static_cast<std::uint8_t>(getChunkRole(doc.docId, i)));   // T5
        }

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
        doc.chunkRoles.reserve(record.chunkRoles.size());
        for (std::uint8_t role : record.chunkRoles) {
            doc.chunkRoles.push_back(static_cast<document::ChunkRole>(role));   // T5
        }

        // T5 补丁：v2 旧索引没有角色字节（chunkRoles 与 chunks 数目不齐）。
        // 块文本完整在手，按同一标注算法免费重算，而不是把"全 Unknown"
        // 在下次落盘时固化进 v3 文件——否则升级后「只看本院认为」会静默为空。
        if (doc.chunkRoles.size() != doc.chunks.size()) {
            std::vector<document::TextChunk> reannotated;
            reannotated.reserve(doc.chunks.size());
            for (std::size_t ci = 0; ci < doc.chunks.size(); ++ci) {
                document::TextChunk tc;
                tc.docId = doc.docId;
                tc.chunkIndex = static_cast<int>(ci);
                tc.content = doc.chunks[ci];
                reannotated.push_back(std::move(tc));
            }
            document::annotateChunkRoles(reannotated);
            doc.chunkRoles.clear();
            doc.chunkRoles.reserve(reannotated.size());
            for (const auto& tc : reannotated) {
                doc.chunkRoles.push_back(tc.role);
            }
        }

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
