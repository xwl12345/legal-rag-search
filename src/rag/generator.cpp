#include "rag/generator.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <sstream>
#include <stdexcept>

namespace rag {

std::string Generator::generate(const std::string& query,
                                 const std::string& context,
                                 StreamCallback callback)
{
    if (apiKey_.empty()) {
        return "[错误] 未设置 API Key，请设置环境变量 DEEPSEEK_API_KEY";
    }

    std::string prompt = buildPrompt(query, context);

    // Build request body
    QJsonObject body;
    body["model"] = QString::fromStdString("deepseek-chat");
    body["stream"] = true;

    QJsonArray messages;
    QJsonObject sysMsg;
    sysMsg["role"] = QString::fromStdString("system");
    sysMsg["content"] = QString::fromStdString(
        "你是一个专业的文档检索助手。请基于提供的文档内容回答问题。如果文档中没有相关信息，请如实说明。");
    messages.append(sysMsg);

    QJsonObject userMsg;
    userMsg["role"] = QString::fromStdString("user");
    userMsg["content"] = QString::fromStdString(prompt);
    messages.append(userMsg);

    body["messages"] = messages;
    body["temperature"] = 0.3;
    body["max_tokens"] = 2048;

    QJsonDocument doc(body);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    // Setup request
    QNetworkRequest request(QUrl("https://api.deepseek.com/v1/chat/completions"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Bearer " + apiKey_).c_str());
    request.setRawHeader("Accept", "text/event-stream");

    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.post(request, data);

    std::string fullAnswer;
    std::string sseBuffer;  // buffer for partial SSE lines

    // Process SSE stream chunks as they arrive
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        QByteArray chunk = reply->readAll();
        sseBuffer += chunk.toStdString();

        std::istringstream stream(sseBuffer);
        std::string line;
        std::string processed;

        while (std::getline(stream, line)) {
            // Check if this is a complete line (ends with \n)
            // getline consumes the \n, so if stream is good, it was a complete line
            if (line.empty() || line[0] == ':') {
                processed += line + "\n";
                continue;
            }

            if (line.rfind("data: ", 0) == 0) {
                std::string jsonStr = line.substr(6);
                if (jsonStr == "[DONE]") {
                    processed += line + "\n";
                    continue;
                }

                QJsonParseError parseError;
                QJsonDocument jdoc = QJsonDocument::fromJson(
                    QByteArray::fromStdString(jsonStr), &parseError);
                if (parseError.error == QJsonParseError::NoError && jdoc.isObject()) {
                    QJsonObject root = jdoc.object();
                    QJsonArray choices = root["choices"].toArray();
                    if (!choices.isEmpty()) {
                        QJsonObject choice = choices[0].toObject();
                        QJsonObject delta = choice["delta"].toObject();
                        if (delta.contains("content")) {
                            std::string content = delta["content"].toString().toStdString();
                            fullAnswer += content;
                            if (callback) {
                                callback(content);
                            }
                        }
                    }
                }
            }
            processed += line + "\n";
        }

        // Keep any incomplete last line in the buffer
        sseBuffer = sseBuffer.substr(processed.size());
    });

    // Synchronous wait via local event loop
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    // Check for network errors (but don't throw - return what we got)
    if (reply->error() != QNetworkReply::NoError) {
        std::string errMsg = reply->errorString().toStdString();
        reply->deleteLater();
        if (fullAnswer.empty()) {
            throw std::runtime_error("LLM API request failed: " + errMsg);
        }
        // If we got partial content, return it
        return fullAnswer;
    }

    reply->deleteLater();
    return fullAnswer;
}

std::string Generator::buildPrompt(const std::string& query,
                                    const std::string& context)
{
    std::ostringstream oss;
    oss << context << "\n";
    oss << "用户问题：" << query << "\n\n";
    oss << "请基于以上文档内容回答问题。要求：\n";
    oss << "1. 回答要准确、简洁\n";
    oss << "2. 标注信息来源（如\"根据【来源 1】...\"）\n";
    oss << "3. 如果文档中没有相关信息，请明确说明";
    return oss.str();
}

std::string Generator::parseDelta(const std::string& jsonLine) {
    QJsonParseError parseError;
    QJsonDocument jdoc = QJsonDocument::fromJson(
        QByteArray::fromStdString(jsonLine), &parseError);
    if (parseError.error == QJsonParseError::NoError && jdoc.isObject()) {
        QJsonObject root = jdoc.object();
        QJsonArray choices = root["choices"].toArray();
        if (!choices.isEmpty()) {
            QJsonObject choice = choices[0].toObject();
            QJsonObject delta = choice["delta"].toObject();
            if (delta.contains("content")) {
                return delta["content"].toString().toStdString();
            }
        }
    }
    return "";
}

} // namespace rag
