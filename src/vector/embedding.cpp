#include "vector/embedding.h"
#include "net/qt_transport.h"
#include "config/app_config.h"
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <stdexcept>

namespace vector_engine {

std::string EmbeddingService::normalizedBaseUrl() const {
    std::string base = apiBaseUrl_;
    // 剥掉全部尾部 '/'（用户可能手填 https://xxx.cn/ 甚至 https://xxx.cn//）
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    // 剥掉尾部 '/v1'（用户看到文档里的 /v1/embeddings 会顺手带上）
    const std::string v1 = "/v1";
    if (base.size() > v1.size() &&
        base.compare(base.size() - v1.size(), v1.size(), v1) == 0) {
        base.erase(base.size() - v1.size());
    }
    return base;
}

std::vector<double> EmbeddingService::embed(const std::string& text) {
    auto batch = embedBatch({text});
    if (batch.empty()) {
        return {};
    }
    return batch[0];
}

std::vector<std::vector<double>> EmbeddingService::embedBatch(
    const std::vector<std::string>& texts)
{
    if (texts.empty() || apiKey_.empty()) {
        return {};
    }
    if (!transport_) {
        transport_ = std::make_shared<QtTransport>();   // 懒创建：绑定当前（引擎）线程
    }

    // Build JSON request body
    QJsonObject body;
    body["model"] = QString::fromStdString(model_);

    QJsonArray inputs;
    for (const auto& t : texts) {
        inputs.append(QString::fromStdString(t));
    }
    body["input"] = inputs;

    QJsonDocument doc(body);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    // Setup HTTPS request（P2：URL 规范化逻辑不变）
    QUrl url(QString::fromStdString(normalizedBaseUrl()) + QStringLiteral("/v1/embeddings"));
    QList<QPair<QByteArray, QByteArray>> headers;
    headers << qMakePair(QByteArray("Authorization"),
                         QByteArray(("Bearer " + apiKey_).c_str()));

    // 同步等待：传输回调在本线程投递，onFinished 恰好一次（P2 结构化版本）
    QEventLoop loop;
    HttpResponse resp;
    auto handle = transport_->post(
        url, headers, data, config::HTTP_TIMEOUT * 1000,
        nullptr,   // 嵌入响应无流式增量
        [&](const HttpResponse& r) {
            resp = r;
            loop.quit();
        });
    loop.exec();

    if (resp.networkError || resp.statusCode != 200) {
        const QString reason = resp.errorText.isEmpty()
            ? QStringLiteral("HTTP %1").arg(resp.statusCode)
            : resp.errorText;
        throw std::runtime_error("Embedding API error: " + reason.toStdString());
    }

    // Parse response（200 包错误体此前被静默当空结果——体检缺陷 #20 同源，这里如实抛）
    QJsonParseError parseError;
    QJsonDocument respDoc = QJsonDocument::fromJson(resp.body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        throw std::runtime_error("JSON parse error: " + parseError.errorString().toStdString());
    }

    QJsonObject respObj = respDoc.object();
    if (respObj.contains(QStringLiteral("error"))) {
        const QString msg = respObj.value(QStringLiteral("error")).toObject()
                                .value(QStringLiteral("message")).toString();
        throw std::runtime_error("Embedding API error: " + msg.toStdString());
    }

    QJsonArray dataArray = respObj["data"].toArray();

    // P2 缺陷修复（#8）：按响应的 index 字段重排，不再假设服务端保序——
    // OpenAI 兼容端点规范允许乱序返回，错位是静默的语义级数据损坏。
    std::vector<std::vector<double>> result(texts.size());
    int sequential = 0;   // 服务端未带 index 时的保底（按数组顺序）
    for (const auto& item : dataArray) {
        QJsonObject itemObj = item.toObject();
        QJsonArray embedding = itemObj["embedding"].toArray();

        std::vector<double> vec;
        vec.reserve(embedding.size());
        for (const auto& v : embedding) {
            vec.push_back(v.toDouble());
        }

        int idx = -1;
        if (itemObj.contains(QStringLiteral("index"))) {
            idx = itemObj.value(QStringLiteral("index")).toInt(-1);
        }
        if (idx < 0 || idx >= static_cast<int>(texts.size())) {
            idx = sequential;
        }
        ++sequential;
        if (idx >= 0 && idx < static_cast<int>(result.size())) {
            result[idx] = std::move(vec);
        }
    }

    return result;
}

} // namespace vector_engine
