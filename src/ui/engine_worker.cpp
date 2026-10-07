#include "ui/engine_worker.h"

#include <QFileInfo>
#include <QMetaType>

#include "rag/eval_metrics.h"
#include "rag/golden_queries.h"

namespace {

/// 批量评测的四个通路（与质量页/评测工具同一组定义）
struct EvalModeRow {
    const char* name;
    rag::SearchMode mode;
    std::vector<rag::QueryMetrics> rows;
};

}  // namespace

EngineWorker::EngineWorker(QObject* parent)
    : QObject(parent)
{
    // 队列化信号携带的自定义类型必须注册（跨线程 queued connection 用）
    qRegisterMetaType<std::vector<rag::SearchResult>>("std::vector<rag::SearchResult>");
    qRegisterMetaType<ui_engine::ImportSummary>("ui_engine::ImportSummary");
    qRegisterMetaType<ui_engine::CompareResult>("ui_engine::CompareResult");
    qRegisterMetaType<config::AppSettings>("config::AppSettings");
}

void EngineWorker::search(const QString& query, int topK) {
    auto results = retriever_.search(query.toStdString(), topK);
    emit searchFinished(results, query);
}

void EngineWorker::compareModes(const QString& query, int topK) {
    const std::string q = query.toStdString();
    ui_engine::CompareResult result;
    result.bm25 = retriever_.searchWithMode(q, topK, rag::SearchMode::Bm25Only);
    result.vector = retriever_.searchWithMode(q, topK, rag::SearchMode::VectorOnly);
    result.weighted = retriever_.searchWithMode(q, topK, rag::SearchMode::WeightedFusion);
    result.rrf = retriever_.searchWithMode(q, topK, rag::SearchMode::RrfFusion);
    emit compareFinished(result);
}

void EngineWorker::runBatchEval(int width) {
    const auto& golden = rag::goldenQueries();
    std::vector<EvalModeRow> modes = {
        {"① BM25 单路",     rag::SearchMode::Bm25Only,      {}},
        {"② 向量单路",      rag::SearchMode::VectorOnly,    {}},
        {"③ 加权融合",      rag::SearchMode::WeightedFusion,{}},
        {"④ RRF 融合",      rag::SearchMode::RrfFusion,     {}},
    };
    const bool vectorMissing = !retriever_.embeddingReady();

    for (size_t gi = 0; gi < golden.size(); ++gi) {
        emit evalProgress(static_cast<int>(gi) + 1, static_cast<int>(golden.size()),
                          QString::fromStdString(golden[gi].query));

        for (auto& m : modes) {
            auto results = retriever_.searchWithMode(golden[gi].query, width, m.mode);
            std::vector<std::string> chunkDocs;
            chunkDocs.reserve(results.size());
            for (const auto& r : results) {
                chunkDocs.push_back(r.docId);
            }
            m.rows.push_back(rag::computeQueryMetrics(
                rag::docLevelRanking(chunkDocs), golden[gi].relevant));
        }
    }

    for (size_t i = 0; i < modes.size(); ++i) {
        const rag::QueryMetrics avg = rag::averageMetrics(modes[i].rows);
        emit evalRow(static_cast<int>(i), QString::fromUtf8(modes[i].name),
                     avg.p5, avg.hit5, avg.r10, avg.mrr);
    }
    emit evalFinished(vectorMissing);
}

void EngineWorker::importDocuments(const QStringList& files) {
    ui_engine::ImportSummary summary;

    // 取消令牌：UI 线程只写原子标志，这里轮询读取（OCR 回退每约 200ms 问一次）
    const std::function<bool()> cancelledQuery = [this]() -> bool {
        return importCancel_ && importCancel_->load(std::memory_order_relaxed);
    };
    const std::function<void(int, int)> onPage = [this](int page, int total) {
        emit importOcrPage(page, total);
    };

    for (int i = 0; i < files.size(); ++i) {
        if (cancelledQuery()) {
            summary.userCancelled = true;
            summary.remaining = files.size() - i;
            break;
        }
        try {
            const auto result = retriever_.addDocument(
                files[i].toStdString(), cancelledQuery, onPage);
            if (result.imported) {
                ++summary.imported;
                summary.chunksAdded += result.chunksAdded;
                if (result.source == document::ParseSource::Ocr) {
                    ++summary.ocrImported;
                }
            } else if (result.cancelled) {
                summary.userCancelled = true;
                summary.remaining = files.size() - i - 1;
                break;
            } else {
                const QString reason = result.diagnostic.empty()
                    ? QStringLiteral("未能提取可检索文本")
                    : QString::fromStdString(result.diagnostic);
                summary.errors.append(QFileInfo(files[i]).fileName()
                                      + QStringLiteral("：") + reason);
            }
        } catch (const std::exception& e) {
            summary.errors.append(QFileInfo(files[i]).fileName()
                                  + QStringLiteral("：") + QString::fromUtf8(e.what()));
        }
        emit importProgress(i + 1, files.size(), QFileInfo(files[i]).fileName());
    }

    emit importFinished(summary);
}

void EngineWorker::generateAnswer(const QString& query, const QString& context,
                                  const QString& metaContext, double temperature) {
    generator_.setTemperature(temperature);
    generator_.setMetadataContext(metaContext.toStdString());
    try {
        generator_.generate(query.toStdString(), context.toStdString(),
                            [this](const std::string& delta) {
                                emit generationDelta(QString::fromStdString(delta));
                            });
        // 与旧同步语义一致：网络错误但已有部分内容时 generate 正常返回（不抛），
        // 此处按"正常完成（残卷）"上报，中断标记由 UI 侧回答内容判空兜底。
        emit generationFinished(false, QString());
    } catch (const std::exception& e) {
        emit generationFinished(true, QString::fromUtf8(e.what()));
    }
}

void EngineWorker::cancelGeneration() {
    generator_.cancel();
}

void EngineWorker::applySettings(config::AppSettings settings) {
    retriever_.setSearchParams(settings.k1, settings.b,
                               settings.bm25Weight, settings.vectorWeight);
    retriever_.setChunkParams(settings.chunkSize, settings.chunkOverlap);
    retriever_.setEmbeddingEndpoint(settings.embeddingBaseUrl, settings.embeddingModel);
    if (!settings.embeddingApiKey.empty()) {
        retriever_.setApiKey(settings.embeddingApiKey);
    }
    emit settingsApplied();
}

void EngineWorker::setLlmApiKey(const QString& key) {
    llmKey_ = key;
    generator_.setApiKey(llmKey_.toStdString());
}
