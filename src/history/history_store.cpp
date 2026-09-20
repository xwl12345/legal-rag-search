#include "history/history_store.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QVariant>
#include <QtGlobal>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif

#include <atomic>
#include <iomanip>
#include <sstream>

#include "config/app_config.h"

namespace history {
namespace {

/// 具名连接序号：每个实例一个独立连接名，便于测试里并存多个实例
std::atomic<int> g_connectionSeq{0};

QString toQ(const std::string& s) { return QString::fromStdString(s); }
std::string toS(const QString& s) { return s.toStdString(); }

constexpr int kScoreDigits = 3;
constexpr int kRecentDefault = 200;

QString currentTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// ── 来源数组的 JSON 编解码 ──
std::string sourcesToJson(const std::vector<SourceItem>& sources) {
    QJsonArray array;
    for (const auto& item : sources) {
        QJsonObject obj;
        obj[QStringLiteral("docId")] = toQ(item.docId);
        obj[QStringLiteral("chunkIndex")] = item.chunkIndex;
        obj[QStringLiteral("finalScore")] = item.finalScore;
        obj[QStringLiteral("snippet")] = toQ(item.snippet);
        array.append(obj);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact).toStdString();
}

std::vector<SourceItem> sourcesFromJson(const std::string& json) {
    std::vector<SourceItem> sources;
    if (json.empty()) {
        return sources;
    }

    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isArray()) {
        return sources;
    }

    const QJsonArray array = doc.array();
    sources.reserve(static_cast<size_t>(array.size()));
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        SourceItem item;
        item.docId = toS(obj.value(QStringLiteral("docId")).toString());
        item.chunkIndex = obj.value(QStringLiteral("chunkIndex")).toInt();
        item.finalScore = obj.value(QStringLiteral("finalScore")).toDouble();
        item.snippet = toS(obj.value(QStringLiteral("snippet")).toString());
        sources.push_back(std::move(item));
    }
    return sources;
}

/// LIKE 通配符转义：用户输入里的 % _ \ 不当作通配符
QString escapeLike(const QString& keyword) {
    QString out;
    out.reserve(keyword.size());
    for (const QChar ch : keyword) {
        if (ch == QLatin1Char('%') || ch == QLatin1Char('_') || ch == QLatin1Char('\\')) {
            out.append(QLatin1Char('\\'));
        }
        out.append(ch);
    }
    return out;
}

HistoryRecord recordFromQuery(QSqlQuery& query) {
    HistoryRecord record;
    record.id = query.value(0).toLongLong();
    record.createdAt = toS(query.value(1).toString());
    record.query = toS(query.value(2).toString());
    record.answer = toS(query.value(3).toString());
    record.sources = sourcesFromJson(toS(query.value(4).toString()));
    record.interrupted = query.value(6).toInt() != 0;
    record.note = toS(query.value(7).toString());
    return record;
}

/// Markdown 表格单元格里的竖线与换行会破坏表格结构，先规整掉
QString tableCell(const QString& text) {
    QString out = text;
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    out.replace(QLatin1Char('|'), QStringLiteral("\\|"));
    return out.trimmed();
}

QString scoreText(double score) {
    return QString::number(score, 'f', kScoreDigits);
}

}  // namespace

HistoryStore::HistoryStore(std::string dbPath)
    : dbPath_(std::move(dbPath))
    , connectionName_(QStringLiteral("history_conn_%1")
                          .arg(++g_connectionSeq, 0, 10))
{
}

HistoryStore::HistoryStore()
    : HistoryStore(std::string(config::HISTORY_DB))
{
}

HistoryStore::~HistoryStore() {
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
        if (db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

std::string HistoryStore::lastError() const {
    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    const QString text = db.isValid() ? db.lastError().text() : QString();
    return text.isEmpty() ? QStringLiteral("（无）").toStdString() : toS(text);
}

bool HistoryStore::open() {
    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    if (!db.isValid()) {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    }
    db.setDatabaseName(QString::fromStdString(dbPath_));

    if (!db.open()) {
        opened_ = false;
        return false;
    }

    QSqlQuery query(db);
    const char* const kCreateTable =
        "CREATE TABLE IF NOT EXISTS history ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at  TEXT NOT NULL,"
        "  query       TEXT NOT NULL,"
        "  answer      TEXT NOT NULL,"
        "  sources     TEXT NOT NULL,"
        "  hit_count   INTEGER NOT NULL,"
        "  interrupted INTEGER NOT NULL DEFAULT 0,"
        "  note        TEXT NOT NULL DEFAULT ''"
        ")";
    if (!query.exec(QString::fromUtf8(kCreateTable))) {
        opened_ = false;
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_history_created_at ON history(created_at DESC)"))) {
        opened_ = false;
        return false;
    }

    opened_ = true;
    return true;
}

long long HistoryStore::append(const HistoryRecord& record) {
    if (!opened_) {
        return 0;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO history (created_at, query, answer, sources, hit_count, interrupted, note) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(record.createdAt.empty() ? currentTimestamp()
                                                : QString::fromStdString(record.createdAt));
    query.addBindValue(toQ(record.query));
    query.addBindValue(toQ(record.answer));
    query.addBindValue(toQ(sourcesToJson(record.sources)));
    query.addBindValue(record.hitCount());
    query.addBindValue(record.interrupted ? 1 : 0);
    query.addBindValue(toQ(record.note));

    if (!query.exec()) {
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

std::vector<HistoryRecord> HistoryStore::recent(int limit) const {
    std::vector<HistoryRecord> out;
    if (!opened_) {
        return out;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, created_at, query, answer, sources, hit_count, interrupted, note "
        "FROM history ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(limit > 0 ? limit : kRecentDefault);
    if (!query.exec()) {
        return out;
    }
    while (query.next()) {
        out.push_back(recordFromQuery(query));
    }
    return out;
}

std::vector<HistoryRecord> HistoryStore::search(const std::string& keyword, int limit) const {
    if (keyword.empty()) {
        return recent(limit);
    }

    std::vector<HistoryRecord> out;
    if (!opened_) {
        return out;
    }

    const QString pattern = QStringLiteral("%") + escapeLike(toQ(keyword)) + QStringLiteral("%");

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, created_at, query, answer, sources, hit_count, interrupted, note "
        "FROM history WHERE query LIKE ? ESCAPE '\\' OR answer LIKE ? ESCAPE '\\' "
        "ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    query.addBindValue(limit > 0 ? limit : kRecentDefault);
    if (!query.exec()) {
        return out;
    }
    while (query.next()) {
        out.push_back(recordFromQuery(query));
    }
    return out;
}

bool HistoryStore::get(long long id, HistoryRecord& out) const {
    if (!opened_ || id <= 0) {
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, created_at, query, answer, sources, hit_count, interrupted, note "
        "FROM history WHERE id = ?"));
    query.addBindValue(qlonglong(id));
    if (!query.exec() || !query.next()) {
        return false;
    }
    out = recordFromQuery(query);
    return true;
}

bool HistoryStore::remove(long long id) {
    if (!opened_ || id <= 0) {
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM history WHERE id = ?"));
    query.addBindValue(qlonglong(id));
    if (!query.exec()) {
        return false;
    }
    return query.numRowsAffected() > 0;
}

int HistoryStore::removeAll() {
    if (!opened_) {
        return 0;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("DELETE FROM history"))) {
        return 0;
    }
    return query.numRowsAffected();
}

int HistoryStore::count() const {
    if (!opened_) {
        return 0;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, /*open=*/false);
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM history")) || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

// ── Markdown 导出 ──────────────────────────────────────────────────────────

std::string HistoryStore::toMarkdown(const HistoryRecord& record) {
    QString text;
    QTextStream out(&text);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#else
    out.setEncoding(QStringConverter::Utf8);
#endif

    out << QStringLiteral("# 检索问答记录\n\n");
    out << QStringLiteral("- 记录编号：%1\n").arg(record.id);
    out << QStringLiteral("- 提问时间：%1\n").arg(toQ(record.createdAt));
    out << QStringLiteral("- 命中块数：%1\n").arg(record.hitCount());
    if (record.interrupted) {
        const QString reason = record.note.empty() ? QStringLiteral("生成过程未正常结束")
                                                   : toQ(record.note);
        out << QStringLiteral("- 回答状态：⚠ 未完成（%1）\n").arg(reason);
    } else {
        out << QStringLiteral("- 回答状态：完整\n");
    }

    out << QStringLiteral("\n## 一、检索问题\n\n%1\n").arg(toQ(record.query));

    if (record.interrupted) {
        out << QStringLiteral("\n## 二、AI 回答（未完成）\n\n%1\n").arg(toQ(record.answer));
    } else {
        out << QStringLiteral("\n## 二、AI 回答\n\n%1\n").arg(toQ(record.answer));
    }

    out << QStringLiteral("\n## 三、命中来源\n\n");
    if (record.sources.empty()) {
        out << QStringLiteral("（本次回答没有引用任何文本块）\n");
    } else {
        out << QStringLiteral("| # | 文档 | 块号 | 相关度 | 片段预览 |\n");
        out << QStringLiteral("| --- | --- | --- | --- | --- |\n");
        for (size_t i = 0; i < record.sources.size(); ++i) {
            const auto& item = record.sources[i];
            out << QStringLiteral("| %1 | %2 | %3 | %4 | %5 |\n")
                       .arg(static_cast<int>(i + 1))
                       .arg(tableCell(toQ(item.docId)))
                       .arg(item.chunkIndex)
                       .arg(scoreText(item.finalScore))
                       .arg(tableCell(toQ(item.snippet)));
        }
    }

    out.flush();
    return toS(text);
}

std::string HistoryStore::toMarkdownAll(const std::vector<HistoryRecord>& records) {
    QString text;
    QTextStream out(&text);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#else
    out.setEncoding(QStringConverter::Utf8);
#endif

    out << QStringLiteral("# 检索问答历史导出\n\n");
    out << QStringLiteral("共 %1 条记录\n").arg(static_cast<int>(records.size()));
    for (const auto& record : records) {
        out << QStringLiteral("\n---\n\n") << toQ(toMarkdown(record));
    }
    out.flush();
    return toS(text);
}

bool HistoryStore::writeMarkdownFile(const QString& path, const std::string& markdown,
                                     QString* errorOut) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorOut) {
            *errorOut = QStringLiteral("无法写入文件：%1").arg(file.errorString());
        }
        return false;
    }

    // BOM：Windows 记事本靠它判定 UTF-8，否则导出件里中文极易显示为乱码
    if (file.write(QByteArray("\xEF\xBB\xBF")) != 3) {
        if (errorOut) {
            *errorOut = QStringLiteral("写入文件头失败");
        }
        return false;
    }

    QTextStream stream(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    stream.setCodec("UTF-8");
#else
    stream.setEncoding(QStringConverter::Utf8);
#endif
    stream << QString::fromStdString(markdown);
    stream.flush();
    if (stream.status() != QTextStream::Ok) {
        if (errorOut) {
            *errorOut = QStringLiteral("写入内容失败");
        }
        return false;
    }
    return true;
}

}  // namespace history
