#include "config/app_settings.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariant>

namespace config {

AppSettings AppSettings::defaults() {
    return AppSettings{};   // 头文件里的成员初始化即默认值
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
    out.embeddingBaseUrl = readString(obj, "embeddingBaseUrl", out.embeddingBaseUrl);
    out.embeddingModel = readString(obj, "embeddingModel", out.embeddingModel);
    out.embeddingApiKey = readString(obj, "embeddingApiKey", out.embeddingApiKey);
    return true;
}

bool AppSettings::save(const std::string& filePath, const AppSettings& data,
                       std::string* diagnostic) {
    QJsonObject obj;
    obj.insert(QStringLiteral("k1"), static_cast<double>(data.k1));
    obj.insert(QStringLiteral("b"), static_cast<double>(data.b));
    obj.insert(QStringLiteral("bm25Weight"), static_cast<double>(data.bm25Weight));
    obj.insert(QStringLiteral("vectorWeight"), static_cast<double>(data.vectorWeight));
    obj.insert(QStringLiteral("topK"), data.topK);
    obj.insert(QStringLiteral("chunkSize"), data.chunkSize);
    obj.insert(QStringLiteral("chunkOverlap"), data.chunkOverlap);
    obj.insert(QStringLiteral("temperature"), static_cast<double>(data.temperature));
    obj.insert(QStringLiteral("embeddingBaseUrl"),
               QString::fromStdString(data.embeddingBaseUrl));
    obj.insert(QStringLiteral("embeddingModel"),
               QString::fromStdString(data.embeddingModel));
    obj.insert(QStringLiteral("embeddingApiKey"),
               QString::fromStdString(data.embeddingApiKey));

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
