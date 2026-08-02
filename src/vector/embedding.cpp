#include "vector/embedding.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <stdexcept>

namespace vector_engine {

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

    // Build JSON request body
    QJsonObject body;
    body["model"] = QString::fromStdString("text-embedding-3-small");

    QJsonArray inputs;
    for (const auto& t : texts) {
        inputs.append(QString::fromStdString(t));
    }
    body["input"] = inputs;

    QJsonDocument doc(body);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    // Setup HTTPS request
    QNetworkRequest request(QUrl("https://api.deepseek.com/v1/embeddings"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Bearer " + apiKey_).c_str());

    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.post(request, data);

    // Synchronous wait via local event loop
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        std::string errMsg = reply->errorString().toStdString();
        reply->deleteLater();
        throw std::runtime_error("Embedding API error: " + errMsg);
    }

    QByteArray response = reply->readAll();
    reply->deleteLater();

    // Parse response
    QJsonParseError parseError;
    QJsonDocument respDoc = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        throw std::runtime_error("JSON parse error: " + parseError.errorString().toStdString());
    }

    QJsonObject respObj = respDoc.object();
    QJsonArray dataArray = respObj["data"].toArray();

    std::vector<std::vector<double>> result;
    result.reserve(dataArray.size());

    for (const auto& item : dataArray) {
        QJsonObject itemObj = item.toObject();
        QJsonArray embedding = itemObj["embedding"].toArray();

        std::vector<double> vec;
        vec.reserve(embedding.size());
        for (const auto& v : embedding) {
            vec.push_back(v.toDouble());
        }
        result.push_back(std::move(vec));
    }

    return result;
}

} // namespace vector_engine
