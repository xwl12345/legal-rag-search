#include "rag/synonym.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace rag {
namespace synonym {

SynonymMap SynonymMap::defaults() {
    SynonymMap map;
    // 内置默认映射（保守起步，S2 验收环节以 eval before/after 实测为准调优）：
    //  - 电信诈骗：qrels 失败查询核心条（语料用词「诈骗/骗取」，见任务卡核实）
    //  - 欠钱不还 → 借款/债务：民间借贷文书高频词
    //  - 打官司 → 诉讼：通用法律口语
    map.entries = {
        {"电信诈骗", {"诈骗", "骗取"}},
        {"欠钱不还", {"借款", "债务"}},
        {"打官司", {"诉讼"}},
    };
    return map;
}

const std::vector<std::string>* SynonymMap::find(const std::string& spoken) const {
    for (const auto& [key, terms] : entries) {
        if (key == spoken) {
            return &terms;
        }
    }
    return nullptr;
}

int expandTerms(const std::string& query, std::vector<std::string>& terms,
                const SynonymMap& map) {
    if (query.empty() || map.entries.empty()) {
        return 0;
    }

    // 去重集合：既有词项 + 本轮已追加，扩展语义 = 补充，不放大既有词权重
    std::vector<std::string> added;
    for (const auto& [spoken, synonymTerms] : map.entries) {
        if (spoken.empty() || query.find(spoken) == std::string::npos) {
            continue;
        }
        for (const auto& term : synonymTerms) {
            if (term.empty()) {
                continue;
            }
            const bool exists = [&]() {
                for (const auto& existing : terms) {
                    if (existing == term) return true;
                }
                for (const auto& existing : added) {
                    if (existing == term) return true;
                }
                return false;
            }();
            if (!exists) {
                terms.push_back(term);
                added.push_back(term);
            }
        }
    }
    return static_cast<int>(added.size());
}

bool load(const std::string& filePath, SynonymMap& out) {
    out = SynonymMap::defaults();   // 先置默认：任何失败路径都回落默认，绝不静默丢扩展能力

    QFile file(QString::fromStdString(filePath));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;   // 文件不存在（首次运行）属正常路径，调用方按需生成
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;   // JSON 损坏：保持默认
    }

    const QJsonValue mappings = doc.object().value(QLatin1String("mappings"));
    if (!mappings.isArray()) {
        return false;   // 字段缺失/类型不对：保持默认
    }

    SynonymMap parsed;
    for (const QJsonValue& item : mappings.toArray()) {
        if (!item.isObject()) {
            continue;   // 单条损坏跳过，其余条目照常生效
        }
        const QJsonObject entry = item.toObject();
        const QJsonValue spoken = entry.value(QLatin1String("spoken"));
        const QJsonValue terms = entry.value(QLatin1String("terms"));
        if (!spoken.isString() || !terms.isArray()) {
            continue;
        }
        std::vector<std::string> termList;
        for (const QJsonValue& term : terms.toArray()) {
            if (term.isString()) {
                termList.push_back(term.toString().toStdString());
            }
        }
        if (termList.empty()) {
            continue;
        }
        parsed.entries.emplace_back(spoken.toString().toStdString(),
                                    std::move(termList));
    }

    if (parsed.entries.empty()) {
        return false;   // 全部条目无效：保持默认（空映射会让扩展能力静默失效）
    }
    out = std::move(parsed);
    return true;
}

bool save(const std::string& filePath, const SynonymMap& data,
          std::string* diagnostic) {
    if (filePath.empty()) {
        if (diagnostic) {
            *diagnostic = "保存路径为空";
        }
        return false;
    }

    QJsonArray mappings;
    for (const auto& [spoken, terms] : data.entries) {
        QJsonArray termArray;
        for (const auto& term : terms) {
            termArray.append(QString::fromStdString(term));
        }
        QJsonObject entry;
        entry.insert(QLatin1String("spoken"), QString::fromStdString(spoken));
        entry.insert(QLatin1String("terms"), termArray);
        mappings.append(entry);
    }
    QJsonObject root;
    root.insert(QLatin1String("version"), 1);
    root.insert(QLatin1String("mappings"), mappings);

    QFile file(QString::fromStdString(filePath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (diagnostic) {
            *diagnostic = "无法写入文件（路径无效、被占用或权限不足）：" + filePath;
        }
        return false;
    }
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    const bool ok = file.write(json) == json.size();
    file.close();
    if (!ok && diagnostic) {
        *diagnostic = "写入不完整：" + filePath;
    }
    return ok;
}

} // namespace synonym
} // namespace rag
