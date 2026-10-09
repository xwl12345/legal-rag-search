#include "rag/generator.h"
#include "net/qt_transport.h"
#include "config/app_config.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEventLoop>
#include <sstream>
#include <stdexcept>

namespace rag {

void Generator::cancel() {
    // 同线程调用（引擎线程内）：中断活动传输；onFinished 仍会恰好一次，
    // 等待中的事件循环以网络错误（OperationCanceled）收场 → generate() 抛中断。
    cancelRequested_ = true;
    if (activeHandle_) {
        activeHandle_->cancel();
    }
}

std::string Generator::generate(const std::string& query,
                                 const std::string& context,
                                 StreamCallback callback)
{
    if (apiKey_.empty()) {
        return "[错误] 未设置 API Key，请设置环境变量 DEEPSEEK_API_KEY";
    }
    if (!transport_) {
        transport_ = std::make_shared<QtTransport>();   // 懒创建：绑定当前（引擎）线程
    }

    std::string prompt = buildPrompt(query, context, metaContext_);

    bool legal = isLegalContext(context);

    // Build request body
    QJsonObject body;
    body["model"] = QString::fromStdString(chatModel_);   // P2：模型名成员化（旧为硬编码）
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
    body["temperature"] = temperature_;
    body["max_tokens"] = 2048;

    QJsonDocument doc(body);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    // Setup request（P2：URL 用成员拼接，apiBaseUrl_ 不再是死成员）
    QUrl url(QString::fromStdString(apiBaseUrl_) + QStringLiteral("/v1/chat/completions"));
    QList<QPair<QByteArray, QByteArray>> headers;
    headers << qMakePair(QByteArray("Authorization"),
                         QByteArray(("Bearer " + apiKey_).c_str()))
            << qMakePair(QByteArray("Accept"), QByteArray("text/event-stream"));

    QEventLoop loop;
    std::string fullAnswer;
    std::string sseBuffer;  // buffer for partial SSE lines
    HttpResponse finalResp;

    // 处理单条 SSE 行（"data: {...}" / "data: [DONE]" / 注释行）
    auto processSseLine = [&](const std::string& line) {
        if (line.empty() || line[0] == ':') return;

        if (line.rfind("data: ", 0) != 0) return;
        const std::string jsonStr = line.substr(6);
        if (jsonStr == "[DONE]") return;

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
    };

    // Process SSE stream chunks as they arrive.
    // 只消费以 \n 结尾的完整行，不完整的尾行留在缓冲区等下一次数据到达。
    // 接收路径由传输层保证：回调只在 post 的调用线程投递，onFinished 恰好一次。
    auto handle = transport_->post(
        url, headers, data, config::HTTP_TIMEOUT * 2000,
        [&](const QByteArray& chunk) {
            sseBuffer += chunk.toStdString();

            size_t pos = 0;
            size_t nl;
            while ((nl = sseBuffer.find('\n', pos)) != std::string::npos) {
                std::string line = sseBuffer.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();  // 兼容 CRLF
                processSseLine(line);
            }
            sseBuffer.erase(0, pos);  // 保留不完整的尾行（pos ≤ size，安全）
        },
        [&](const HttpResponse& resp) {
            finalResp = resp;
            loop.quit();
        });

    // 活动句柄登记（cancel 的作用目标）；generate 的所有出口都不留悬挂
    cancelRequested_ = false;
    activeHandle_ = handle;
    loop.exec();
    activeHandle_.reset();

    // 流结束后处理缓冲区中残留的最后一行（可能没有换行结尾）
    if (!sseBuffer.empty()) {
        std::string lastLine = sseBuffer;
        if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
        processSseLine(lastLine);
    }

    // ── 错误归并（P2：与旧语义对齐 + 新增错误体解析）──
    // 用户主动停止恒抛中断：增量已经由回调送进调用方的缓冲，抛出让上层
    // 把本回合标记为 interrupted——绝不能把"用户停止"伪装成完整回答。
    if (cancelRequested_) {
        throw std::runtime_error("LLM API request failed: Operation canceled");
    }

    // 传输层失败 / HTTP >= 400 / 200 包错误体，都走"有增量则保残卷、无增量则抛"
    QString failReason;
    if (finalResp.networkError || finalResp.statusCode >= 400) {
        failReason = finalResp.errorText.isEmpty()
            ? QStringLiteral("HTTP %1").arg(finalResp.statusCode)
            : finalResp.errorText;
    } else if (finalResp.statusCode == 0) {
        failReason = QStringLiteral("no response from server");
    } else {
        // HTTP 200：响应体可能是 {"error": {...}}（网关/上游错误此前被静默吞掉，
        // 界面上像"按钮失灵"——体检缺陷 #20）。这里解析并如实上报。
        QJsonParseError parseError;
        QJsonDocument respDoc = QJsonDocument::fromJson(finalResp.body, &parseError);
        if (parseError.error == QJsonParseError::NoError && respDoc.isObject()) {
            const QJsonObject root = respDoc.object();
            if (root.contains(QStringLiteral("error"))) {
                const QString msg = root.value(QStringLiteral("error")).toObject()
                                        .value(QStringLiteral("message")).toString();
                failReason = msg.isEmpty()
                    ? QStringLiteral("服务返回错误响应体")
                    : msg;
            }
        }
    }

    if (!failReason.isEmpty()) {
        if (fullAnswer.empty()) {
            throw std::runtime_error("LLM API request failed: " + failReason.toStdString());
        }
        // 已有部分内容：按旧语义返回残卷（调用方按正常完成落库）
        return fullAnswer;
    }

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

} // namespace rag
