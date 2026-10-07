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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (embedding_.apiKey() == key) {
            return;  // 同 Key 重复下发（如启动时从配置加载）不清缓存，避免无谓的全库重算
        }
        embedding_.setApiKey(key);
        // Key 指向的服务/配额可能不同，同一模型名下旧向量不再可信：
        // 失效缓存，下次向量检索按新配置懒重建（P0-4）。
        ++vectorEpoch_;
        similarity_.clear();
        vectorIndexMap_.clear();
    }
}

// ── T3 配置中心：运行时热更新 ──

void Retriever::setSearchParams(double k1, double b,
                                double bm25Weight, double vectorWeight) {
    std::lock_guard<std::mutex> lock(mutex_);
    bm25_.setParams(k1, b);
    bm25Weight_ = bm25Weight;
    vectorWeight_ = vectorWeight;
}

void Retriever::setChunkParams(int maxSize, int overlap) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunkMaxSize_ = maxSize;
    chunkOverlap_ = overlap;
}

void Retriever::setEmbeddingEndpoint(const std::string& baseUrl, const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
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
    ++vectorEpoch_;
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

// ── P1：轻只读口（锁内微秒级拷贝，UI 线程可随时调用）──

bool Retriever::embeddingReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return embedding_.isReady();
}

std::string Retriever::embeddingModel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return embedding_.model();
}

size_t Retriever::vectorCacheSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return similarity_.size();
}

double Retriever::k1() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bm25_.k1();
}

double Retriever::b() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bm25_.b();
}

double Retriever::bm25Weight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bm25Weight_;
}

double Retriever::vectorWeight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vectorWeight_;
}

int Retriever::docCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.totalDocs();
}

int Retriever::chunkCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(chunkStore_.size());
}

int Retriever::documentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(documents_.size());
}

// ────────────────────────────────────────────────────────────────
// 索引写入（导入与落盘恢复共用）
// ────────────────────────────────────────────────────────────────

void Retriever::indexDocument(const StoredDocument& doc) {
    // ⚠️ 线程约定（P1）：调用方必须已持有 mutex_（短临界区，内部不再加锁）。
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
    // 分块参数快照（锁内）——解析在锁外做：文件 IO / OCR 等待绝不持锁（P1 短临界区）
    int chunkMax = 512, chunkOvl = 50;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chunkMax = chunkMaxSize_;
        chunkOvl = chunkOverlap_;
    }

    // 使用 DocumentParser::parseWithResult() 统一处理所有文件类型
    // — 文本文件：直接读取
    // — PDF：PdfExtractor 提取文本层 → OCR 回退（扫描件）
    document::DocumentParser parser;
    parser.setChunkParams(chunkMax, chunkOvl);   // T3：分块参数仅对本次导入生效
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

    // 注册段（锁内短临界区）：同 docId 旧记录清除 + 倒排/块/角色/元数据/文档记录
    // 一并落位。删旧记录会 bump vectorEpoch_，向量缓存随之失效（懒重建）。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (documents_.find(docId) != documents_.end()) {
            removeDocumentUnlocked(docId);
        }
        indexDocument(doc);
        if (!doc.metadata.isEmpty()) {
            docMeta_[docId] = doc.metadata;
        }
        documents_[docId] = std::move(doc);
        documentOrder_.push_back(docId);
        std::sort(documentOrder_.begin(), documentOrder_.end());
    }

    return {true, docId, static_cast<int>(chunks.size()), parseResult.source, ""};
}

void Retriever::addText(const std::string& text, const std::string& docId) {
    if (text.empty() || docId.empty()) return;

    // 分块参数快照（锁内），分块在锁外做（CPU 纯计算，可不持锁）
    int chunkMax = 512, chunkOvl = 50;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chunkMax = chunkMaxSize_;
        chunkOvl = chunkOverlap_;
    }

    // 使用已有的 parser 来分块
    document::DocumentParser parser;
    parser.setChunkParams(chunkMax, chunkOvl);   // T3：分块参数仅对本次导入生效
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (documents_.find(docId) != documents_.end()) {
            removeDocumentUnlocked(docId);
        }
        indexDocument(doc);
        if (!doc.metadata.isEmpty()) {
            docMeta_[docId] = doc.metadata;
        }
        documents_[docId] = std::move(doc);
        documentOrder_.push_back(docId);
        std::sort(documentOrder_.begin(), documentOrder_.end());
    }
}

// ────────────────────────────────────────────────────────────────
// 检索
// ────────────────────────────────────────────────────────────────

std::vector<SearchResult> Retriever::search(const std::string& query, int topK) {
    // 默认路 = 加权融合（T0 以来的行为；T4 起显式走 searchWithMode）
    return searchWithMode(query, topK, SearchMode::WeightedFusion);
}

bool Retriever::isAggregateQuery(const std::string& query) {
    // 聚合型问题检测：仅用明确指向「全部文档」的短语，避免误判聚焦型查询
    // （P2 自检索页 search_page.cpp 下沉；短语表变更有单测钉死）
    static const std::vector<std::string> AGGREGATE_MARKERS = {
        "这些案件", "所有案件", "全部案件", "各案件", "各个案件", "每个案件",
        "这些文档", "所有文档", "全部文档", "各文档", "这些文件", "所有文件",
        "哪些案件", "汇总", "统计", "总共", "一共"
    };
    for (const auto& marker : AGGREGATE_MARKERS) {
        if (query.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<SearchResult> Retriever::searchAggregate(const std::string& query, int width,
                                                     int perDocLimit) {
    auto wideResults = search(query, width);

    std::vector<SearchResult> deduped;
    std::unordered_map<std::string, int> docCount;
    for (auto& r : wideResults) {
        int& cnt = docCount[r.docId];
        if (cnt >= perDocLimit) continue;
        ++cnt;
        deduped.push_back(std::move(r));
    }
    return deduped;
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
    // ═══ P1 线程约定 ═══ 本函数运行在引擎线程（或测试的单线程环境）。
    // mutex_ 只保护共享容器的短临界区；网络等待（embed/embedBatch 内部泵
    // 引擎线程事件）一律不持锁——同线程重入的队列化任务（如检索等待期间
    // 的设置热更新）必须能拿到锁，且重入引发的缓存失效靠 vectorEpoch_ 兜底。

    // ── Step 1: BM25 关键词检索（除 VectorOnly 外所有路都需要；锁内，纯 CPU）──
    auto queryTerms = tokenizer_.cutForIndex(query);
    // 法律文书通常用“住所地”等字段表达地点，补充自然语言位置问法。
    expandLegalLocationTerms(query, queryTerms);
    const int width = std::max(topK * 2, 10);
    std::decay_t<decltype(bm25_.search(queryTerms, index_, width))> bm25Results{};
    if (mode != SearchMode::VectorOnly) {
        std::lock_guard<std::mutex> lock(mutex_);
        bm25Results = bm25_.search(queryTerms, index_, width);
    }

    // ── Step 2: 向量语义检索（未配置 / 异常 → available=false，由各路自行降级）──
    bool vectorAvailable = false;
    std::vector<vector_engine::VectorSearchResult> vectorResults;
    std::unordered_map<std::string, double> vectorScores;

    if (embedding_.isReady()) {   // 同线程读自身配置（写方也在引擎线程），无需锁
        try {
            auto queryVec = embedding_.embed(query);   // 网络等待，不持锁

            // 懒重建：锁内判定 + 取快照，锁外批量调 API，锁内按代次提交。
            // （导入文档不调 API，首次向量检索才批量补算；改 Key/模型后无需
            //   重新导入，下次检索自动按新配置重算——P0-4 起该承诺成立。）
            std::uint64_t epoch = 0;
            std::vector<std::string> rebuildTexts;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (similarity_.size() != static_cast<size_t>(index_.totalDocs())) {
                    similarity_.clear();
                    vectorIndexMap_.clear();
                    epoch = vectorEpoch_;   // 快照代次：期间失效则放弃本轮
                    auto allDocs = index_.allDocs();
                    vectorIndexMap_.reserve(allDocs.size());
                    rebuildTexts.reserve(allDocs.size());
                    for (const auto& [docId, chunkIdx] : allDocs) {
                        auto it = chunkStore_.find(chunkKey(docId, chunkIdx));
                        if (it != chunkStore_.end()) {
                            rebuildTexts.push_back(it->second);
                            vectorIndexMap_.emplace_back(docId, chunkIdx);
                        }
                    }
                }
            }

            if (!rebuildTexts.empty()) {
                for (size_t i = 0; i < rebuildTexts.size(); i += 20) {
                    size_t batchEnd = std::min(i + 20, rebuildTexts.size());
                    std::vector<std::string> batch(
                        rebuildTexts.begin() + i,
                        rebuildTexts.begin() + batchEnd
                    );
                    auto vecs = embedding_.embedBatch(batch);   // 网络等待，不持锁
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (vectorEpoch_ != epoch) {
                        // 快照之后发生了失效（删除/清空/换 Key/换端点）：
                        // 放弃本轮提交，留下已被清空的缓存，下次检索整体重建。
                        break;
                    }
                    for (size_t j = 0; j < vecs.size(); ++j) {
                        similarity_.addVector(static_cast<int>(i + j), vecs[j]);
                    }
                }
            }

            // 向量检索 + key→分数映射（锁内）
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (similarity_.size() > 0) {
                    vectorResults = similarity_.search(queryVec, width);
                    vectorAvailable = !vectorResults.empty();
                }
                if (vectorAvailable) {
                    for (const auto& r : vectorResults) {
                        if (r.index >= 0 && static_cast<size_t>(r.index) < vectorIndexMap_.size()) {
                            auto [docId, chunkIdx] = vectorIndexMap_[r.index];
                            vectorScores[chunkKey(docId, chunkIdx)] = r.similarity;
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            // embedding 失败：vectorAvailable 保持 false，各路按降级语义处理
            vectorAvailable = false;
        }
    }

    // 把 chunkKey 转成 SearchResult 的公共收尾（锁内查共享容器）
    const auto makeResult = [this](const std::string& key) {
        SearchResult sr;
        std::lock_guard<std::mutex> lock(mutex_);
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
            SearchResult sr = makeResult(chunkKey(r.docId, r.chunkIndex));
            sr.bm25Score = r.score;
            sr.finalScore = r.score;
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
    // 锁内拼装（纯 CPU 拼接，微秒级；results 本身是调用方已持有的副本，
    // 但 chunkStore_ 等共享状态在引擎线程上可能被并发修改——这里虽只读
    // results，为一致性惯例统一持锁）
    std::lock_guard<std::mutex> lock(mutex_);
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
// 只读视图（文档库页 / 全文阅读页消费；锁内拷贝，UI 线程可随时调用）
// ────────────────────────────────────────────────────────────────

const document::DocMetadata* Retriever::getMetadata(const std::string& docId) const {
    // ⚠️ 返回内部指针：仅限无并发写场景（单线程测试/工具）。UI 用 metadataOf()。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = docMeta_.find(docId);
    if (it != docMeta_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::optional<document::DocMetadata> Retriever::metadataOf(const std::string& docId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = docMeta_.find(docId);
    if (it != docMeta_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string Retriever::metadataSummary(const std::vector<std::string>& docIds) const {
    // P2 合一：检索页聚焦/聚合两份重复拼装的唯一实现（锁内逐份拷贝元数据）
    std::ostringstream oss;
    for (const auto& docId : docIds) {
        std::optional<document::DocMetadata> meta;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = docMeta_.find(docId);
            if (it != docMeta_.end()) meta = it->second;
        }
        if (!meta || meta->isEmpty()) continue;

        oss << "- " << docId;
        if (!meta->caseNumber.empty()) oss << " | 案号: " << meta->caseNumber;
        if (!meta->court.empty()) oss << " | 法院: " << meta->court;
        if (!meta->caseType.empty()) oss << " | 类型: " << meta->caseType;
        if (!meta->date.empty()) oss << " | 日期: " << meta->date;
        if (!meta->procedure.empty()) oss << " | 程序: " << meta->procedure;
        if (!meta->litigants.empty()) oss << " | 当事人: " << meta->litigants;
        oss << "\n";
    }
    return oss.str();
}

void Retriever::setEmbeddingTransport(std::shared_ptr<IHttpTransport> transport) {
    std::lock_guard<std::mutex> lock(mutex_);
    embedding_.setTransport(std::move(transport));
}

std::vector<std::string> Retriever::allDocIds() const {
    // 按 documentOrder_ 输出，保证跨次运行顺序稳定（unordered_map 遍历顺序不可依赖）
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = documents_.find(docId);
    if (it == documents_.end()) return false;
    out = it->second.fullText;
    return true;
}

bool Retriever::getChunk(const std::string& docId, int chunkIndex, std::string& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = chunkStore_.find(chunkKey(docId, chunkIndex));
    if (it == chunkStore_.end()) return false;
    out = it->second;
    return true;
}

document::ChunkRole Retriever::getChunkRole(const std::string& docId, int chunkIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = chunkRoles_.find(chunkKey(docId, chunkIndex));
    return it != chunkRoles_.end() ? it->second : document::ChunkRole::Unknown;
}

// ────────────────────────────────────────────────────────────────
// 删除
// ────────────────────────────────────────────────────────────────

int Retriever::removeDocument(const std::string& docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return removeDocumentUnlocked(docId);
}

int Retriever::removeDocumentUnlocked(const std::string& docId) {
    // ⚠️ 线程约定：调用方已持有 mutex_。
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
    //    代次 +1：正在进行的懒重建按快照代次察觉失效并放弃提交（P1）。
    ++vectorEpoch_;
    similarity_.clear();
    vectorIndexMap_.clear();

    return chunkTotal;
}

void Retriever::clearAll(bool alsoDeletePersistedFile) {
    std::lock_guard<std::mutex> lock(mutex_);
    clearAllUnlocked(alsoDeletePersistedFile);
}

void Retriever::clearAllUnlocked(bool alsoDeletePersistedFile) {
    // ⚠️ 线程约定：调用方已持有 mutex_。
    index_.clear();
    chunkStore_.clear();
    chunkRoles_.clear();
    docMeta_.clear();
    documents_.clear();
    documentOrder_.clear();
    ++vectorEpoch_;
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

    // 锁内快照（拷贝全部文档记录），文件写入在锁外做（P1 短临界区）
    std::vector<index_store::StoredDocument> records;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 按 docId 排序写出，保证落盘内容可复现（便于 diff 与测试比对）
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
            for (const auto& role : doc.chunkRoles) {   // T5：角色随记录落盘
                record.chunkRoles.push_back(static_cast<std::uint8_t>(role));
            }
            // 容忍角色向量短于块向量（历史数据），缺省补 Unknown
            while (record.chunkRoles.size() < record.chunks.size()) {
                record.chunkRoles.push_back(0);
            }

            records.push_back(std::move(record));
        }
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

        // 注册段（锁内短临界区；文件读取与重标注都在锁外完成）
        {
            std::lock_guard<std::mutex> lock(mutex_);
            indexDocument(doc);
            if (!doc.metadata.isEmpty()) {
                docMeta_[doc.docId] = doc.metadata;
            }
            const std::string id = doc.docId;
            documents_[id] = std::move(doc);
            documentOrder_.push_back(id);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::sort(documentOrder_.begin(), documentOrder_.end());
    }

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
