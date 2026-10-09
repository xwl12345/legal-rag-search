#include "config/app_settings.h"

#include <cmath>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariant>

namespace config {

AppSettings AppSettings::defaults() {
    return AppSettings{};   // 头文件里的成员初始化即默认值
}

void AppSettings::sanitize() {
    // 钳制惯用式 !(x in range)：同时拦住 NaN（NaN 与任何比较均为 false）
    if (!(k1 > 0.0) || k1 > 100.0) k1 = 1.5;
    if (!(b >= 0.0) || !(b <= 1.0)) b = 0.75;

    // 融合权重：非负；和 <= 0 或非数值回落默认；否则重归一化到和为 1
    if (!(bm25Weight >= 0.0)) bm25Weight = 0.4;
    if (!(vectorWeight >= 0.0)) vectorWeight = 0.6;
    const double sum = bm25Weight + vectorWeight;
    if (!(sum > 0.0)) {
        bm25Weight = 0.4;
        vectorWeight = 0.6;
    } else if (std::abs(sum - 1.0) > 1e-9) {
        bm25Weight /= sum;
        vectorWeight /= sum;
    }

    if (topK < 1) topK = 20;
    if (topK > 200) topK = 200;
    if (chunkSize < 64) chunkSize = 512;
    if (chunkSize > 65536) chunkSize = 65536;
    if (chunkOverlap < 0) chunkOverlap = 50;
    if (chunkOverlap >= chunkSize) chunkOverlap = chunkSize / 4;

    if (!(temperature >= 0.0) || !(temperature <= 2.0)) temperature = 0.3;

    // 服务地址/模型空串回落默认（Key 例外：留空是合法语义=未配置）
    if (chatBaseUrl.empty()) chatBaseUrl = defaults().chatBaseUrl;
    if (chatModel.empty()) chatModel = defaults().chatModel;
    if (embeddingBaseUrl.empty()) embeddingBaseUrl = defaults().embeddingBaseUrl;
    if (embeddingModel.empty()) embeddingModel = defaults().embeddingModel;
}

namespace {

/// 从 JSON 对象读 double：键不存在或类型不对返回 fallback。
/// QJsonDouble 只能是数字；true/false/字符串一律视为类型不对（回落默认）。
double readDouble(const QJsonObject& obj, const char* key, double fallback) {
    const auto value = obj.value(QLatin1String(key));
    if (!value.isDouble()) {
        return fallback;
    }
    return value.toDouble(fallback);
}

int readInt(const QJsonObject& obj, const char* key, int fallback) {
    const auto value = obj.value(QLatin1String(key));
    if (!value.isDouble()) {
        return fallback;
    }
    return static_cast<int>(value.toDouble(fallback));
}

std::string readString(const QJsonObject& obj, const char* key, const std::string& fallback) {
    const auto value = obj.value(QLatin1String(key));
    if (!value.isString()) {
        return fallback;
    }
    return value.toString().toStdString();
}

}  // namespace

bool AppSettings::load(const std::string& filePath, AppSettings& out) {
    out = defaults();   // 先置默认：任何失败路径都回落默认，绝不读到 0

    QFile file(QString::fromStdString(filePath));
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;   // 首次运行没有配置文件属正常，静默用默认值
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;   // JSON 被手改坏：整体回落默认值
    }

    // 逐字段读取：缺失 / 类型不对的字段保持上面 defaults() 的值
    const QJsonObject obj = doc.object();
    out.k1 = readDouble(obj, "k1", out.k1);
    out.b = readDouble(obj, "b", out.b);
    out.bm25Weight = readDouble(obj, "bm25Weight", out.bm25Weight);
    out.vectorWeight = readDouble(obj, "vectorWeight", out.vectorWeight);
    out.topK = readInt(obj, "topK", out.topK);
    out.chunkSize = readInt(obj, "chunkSize", out.chunkSize);
    out.chunkOverlap = readInt(obj, "chunkOverlap", out.chunkOverlap);
    out.temperature = readDouble(obj, "temperature", out.temperature);
    out.chatBaseUrl = readString(obj, "chatBaseUrl", out.chatBaseUrl);
    out.chatModel = readString(obj, "chatModel", out.chatModel);
    out.embeddingBaseUrl = readString(obj, "embeddingBaseUrl", out.embeddingBaseUrl);
    out.embeddingModel = readString(obj, "embeddingModel", out.embeddingModel);
    out.embeddingApiKey = readString(obj, "embeddingApiKey", out.embeddingApiKey);

    // P3：读到的任何越界/非法值在此统一钳制（与保存路径同一套规则）
    out.sanitize();
    return true;
}

bool AppSettings::save(const std::string& filePath, const AppSettings& data,
                       std::string* diagnostic) {
    // P3：保存前钳制——UI 表单、测试构造、手工 JSON 都经同一漏斗
    AppSettings sanitized = data;
    sanitized.sanitize();

    QJsonObject obj;
    obj.insert(QStringLiteral("k1"), static_cast<double>(sanitized.k1));
    obj.insert(QStringLiteral("b"), static_cast<double>(sanitized.b));
    obj.insert(QStringLiteral("bm25Weight"), static_cast<double>(sanitized.bm25Weight));
    obj.insert(QStringLiteral("vectorWeight"), static_cast<double>(sanitized.vectorWeight));
    obj.insert(QStringLiteral("topK"), sanitized.topK);
    obj.insert(QStringLiteral("chunkSize"), sanitized.chunkSize);
    obj.insert(QStringLiteral("chunkOverlap"), sanitized.chunkOverlap);
    obj.insert(QStringLiteral("temperature"), static_cast<double>(sanitized.temperature));
    obj.insert(QStringLiteral("chatBaseUrl"),
               QString::fromStdString(sanitized.chatBaseUrl));
    obj.insert(QStringLiteral("chatModel"),
               QString::fromStdString(sanitized.chatModel));
    obj.insert(QStringLiteral("embeddingBaseUrl"),
               QString::fromStdString(sanitized.embeddingBaseUrl));
    obj.insert(QStringLiteral("embeddingModel"),
               QString::fromStdString(sanitized.embeddingModel));
    obj.insert(QStringLiteral("embeddingApiKey"),
               QString::fromStdString(sanitized.embeddingApiKey));

    const QJsonDocument doc(obj);
    QFile file(QString::fromStdString(filePath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (diagnostic) {
            *diagnostic = file.errorString().toStdString();
        }
        return false;
    }
    const qint64 written = file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    if (written < 0) {
        if (diagnostic) {
            *diagnostic = "写入配置文件失败";
        }
        return false;
    }
    return true;
}

}  // namespace config
