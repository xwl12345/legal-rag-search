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

    std::string prompt = buildPrompt(query, context, metaContext_);

    bool legal = isLegalContext(context);

    // Build request body
    QJsonObject body;
    body["model"] = QString::fromStdString("deepseek-chat");
    body["stream"] = true;

    QJsonArray messages;
    QJsonObject sysMsg;
    sysMsg["role"] = QString::fromStdString("system");
    sysMsg["content"] = QString::fromStdString(getSystemPrompt(legal));
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

// ═══════════════════════════════════════════════════════════════
// Prompt 模板
// ═══════════════════════════════════════════════════════════════

bool Generator::isLegalContext(const std::string& context) {
    // 检测法律关键词
    static const std::vector<std::string> LEGAL_MARKERS = {
        "人民法院", "检察院", "原告", "被告", "判决", "裁定",
        "案号", "（20", "(20",      // 案号年份
        "合同法", "刑法", "民法", "诉讼法",
        "违约责任", "侵权", "上诉", "起诉", "答辩",
        "审判长", "审判员", "合议庭",
    };
    for (const auto& marker : LEGAL_MARKERS) {
        if (context.find(marker) != std::string::npos) return true;
    }
    return false;
}

std::string Generator::getSystemPrompt(bool isLegal) {
    if (isLegal) {
        return
            "你是一位专业的中国法律助手，擅长分析裁判文书、合同、法律法规等法律文档。\n"
            "请基于提供的法律文档内容，以严谨、客观、专业的态度回答问题。\n\n"
            "要求：\n"
            "1. 准确引用案号、法条、裁判观点，注明来源\n"
            "2. 分析条理清晰，分点论述，使用法律专业术语\n"
            "3. 如涉及具体案件，先概述案情再分析\n"
            "4. 如需给出建议，请注明「仅供参考，不构成法律意见」\n"
            "5. 如文档信息不足以回答，请明确说明局限性";
    }
    return
        "你是一个专业的文档检索助手。请基于提供的文档内容回答问题。\n"
        "如果文档中没有相关信息，请如实说明。";
}

std::string Generator::buildLegalPrompt(const std::string& query,
                                         const std::string& context,
                                         const std::string& metaContext) {
    std::ostringstream oss;

    // 元数据摘要
    if (!metaContext.empty()) {
        oss << "【文档元数据】\n" << metaContext << "\n\n";
    }

    oss << "【法律文档内容】\n" << context << "\n\n";
    oss << "【用户问题】\n" << query << "\n\n";
    oss << "请基于以上法律文档，按以下结构回答：\n\n";
    oss << "## 一、案件概述\n";
    oss << "（如涉及具体案件，简述案由、当事人、审理法院和程序）\n\n";
    oss << "## 二、法律分析\n";
    oss << "（结合文档中的裁判观点或法律条文，进行分点分析）\n\n";
    oss << "## 三、结论\n";
    oss << "（总结要点，必要时给出建议。如为非法律建议性质的问题，可省略建议部分）\n\n";
    oss << "## 四、参考来源\n";
    oss << "（列出回答引用的文档来源，格式：【来源 N】案号或文档名 + 具体引用内容）";
    return oss.str();
}

std::string Generator::buildGeneralPrompt(const std::string& query,
                                           const std::string& context) {
    std::ostringstream oss;
    oss << context << "\n\n";
    oss << "用户问题：" << query << "\n\n";
    oss << "请基于以上文档内容回答问题。要求：\n";
    oss << "1. 回答准确、简洁，分点论述\n";
    oss << "2. 标注信息来源（如\"根据【来源 1】...\"）\n";
    oss << "3. 如果文档中没有相关信息，请明确说明";
    return oss.str();
}

std::string Generator::buildPrompt(const std::string& query,
                                    const std::string& context,
                                    const std::string& metaContext) {
    if (isLegalContext(context)) {
        return buildLegalPrompt(query, context, metaContext);
    }
    return buildGeneralPrompt(query, context);
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
