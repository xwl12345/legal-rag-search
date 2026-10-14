#include "favorite/favorite_store.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtGlobal>

#include <atomic>

#include "config/app_config.h"

namespace favorite {
namespace {

/// 具名连接序号：每个实例一个独立连接名，便于测试里并存多个实例
std::atomic<int> g_connectionSeq{0};

QString toQ(const std::string& s) { return QString::fromStdString(s); }
std::string toS(const QString& s) { return s.toStdString(); }

QString currentTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

FavoriteCase caseFromQuery(QSqlQuery& query) {
    FavoriteCase c;
    c.id = query.value(0).toLongLong();
    c.name = toS(query.value(1).toString());
    c.note = toS(query.value(2).toString());
    c.createdAt = toS(query.value(3).toString());
    c.favoriteCount = query.value(4).toInt();
    return c;
}

FavoriteItem itemFromQuery(QSqlQuery& query) {
    FavoriteItem item;
    item.id = query.value(0).toLongLong();
    item.caseId = query.value(1).toLongLong();
    item.docId = toS(query.value(2).toString());
    item.chunkIndex = query.value(3).toInt();
    item.title = toS(query.value(4).toString());
    item.snippet = toS(query.value(5).toString());
    item.note = toS(query.value(6).toString());
    item.score = query.value(7).toDouble();
    item.valid = query.value(8).toInt() != 0;
    item.createdAt = toS(query.value(9).toString());
    return item;
}

const char* const kCreateCases =
    "CREATE TABLE IF NOT EXISTS cases ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT NOT NULL UNIQUE,"
    "  note       TEXT NOT NULL DEFAULT '',"
    "  created_at TEXT NOT NULL"
    ")";

const char* const kCreateFavorites =
    "CREATE TABLE IF NOT EXISTS favorites ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  case_id     INTEGER NOT NULL,"
    "  doc_id      TEXT NOT NULL,"
    "  chunk_index INTEGER NOT NULL DEFAULT 0,"
    "  title       TEXT NOT NULL DEFAULT '',"
    "  snippet     TEXT NOT NULL DEFAULT '',"
    "  note        TEXT NOT NULL DEFAULT '',"
    "  score       REAL NOT NULL DEFAULT 0,"
    "  valid       INTEGER NOT NULL DEFAULT 1,"
    "  created_at  TEXT NOT NULL"
    ")";

/// 同一分组内同一块只收藏一次（重复收藏走"刷新快照"路径，见 addFavorite）
const char* const kCreateUniqueIndex =
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_favorites_unique "
    "ON favorites(case_id, doc_id, chunk_index)";

/// 文档删除联动按 doc_id 批量 UPDATE，没有这个索引会全表扫描
const char* const kCreateDocIdIndex =
    "CREATE INDEX IF NOT EXISTS idx_favorites_doc_id ON favorites(doc_id)";

}  // namespace

FavoriteStore::FavoriteStore(std::string dbPath) : dbPath_(std::move(dbPath)) {}

FavoriteStore::FavoriteStore() : dbPath_(config::dataFilePath(config::FAVORITE_DB)) {}

FavoriteStore::~FavoriteStore() {
    if (opened_) {
        const QString name = connectionName_;
        {
            QSqlDatabase db = QSqlDatabase::database(name, false);
            if (db.isOpen()) {
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
    }
}

bool FavoriteStore::open() {
    if (opened_) {
        return true;
    }
    connectionName_ = QStringLiteral("favorite_conn_%1")
                          .arg(g_connectionSeq.fetch_add(1));

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                connectionName_);
    db.setDatabaseName(toQ(dbPath_));
    if (!db.open()) {
        lastError_ = toS(db.lastError().text());
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }

    QSqlQuery query(db);
    if (!query.exec(QString::fromUtf8(kCreateCases))
        || !query.exec(QString::fromUtf8(kCreateFavorites))
        || !query.exec(QString::fromUtf8(kCreateUniqueIndex))
        || !query.exec(QString::fromUtf8(kCreateDocIdIndex))) {
        lastError_ = toS(query.lastError().text());
        db.close();
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }

    opened_ = true;
    return true;
}

std::string FavoriteStore::lastError() const {
    return lastError_.empty() ? std::string("（无）") : lastError_;
}

// ── 案件分组 ──

long long FavoriteStore::createCase(const std::string& name, const std::string& note) {
    if (!opened_ || name.empty()) {
        return 0;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);

    // 重名幂等：先查后插，返回已存在分组的 id（收藏弹窗允许手打已存在的名字）
    QSqlQuery exists(db);
    exists.prepare(QStringLiteral("SELECT id FROM cases WHERE name = ?"));
    exists.addBindValue(toQ(name));
    if (exists.exec() && exists.next()) {
        return exists.value(0).toLongLong();
    }

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO cases (name, note, created_at) VALUES (?, ?, ?)"));
    insert.addBindValue(toQ(name));
    insert.addBindValue(toQ(note));
    insert.addBindValue(currentTimestamp());
    if (!insert.exec()) {
        lastError_ = toS(insert.lastError().text());
        return 0;
    }
    return insert.lastInsertId().toLongLong();
}

bool FavoriteStore::setCaseNote(long long caseId, const std::string& note) {
    if (!opened_ || caseId <= 0) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE cases SET note = ? WHERE id = ?"));
    query.addBindValue(toQ(note));
    query.addBindValue(caseId);
    if (!query.exec()) {
        lastError_ = toS(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

int FavoriteStore::removeCase(long long caseId) {
    if (!opened_ || caseId <= 0) {
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);

    QSqlQuery probe(db);
    probe.prepare(QStringLiteral("SELECT COUNT(*) FROM cases WHERE id = ?"));
    probe.addBindValue(caseId);
    if (!probe.exec() || !probe.next() || probe.value(0).toInt() == 0) {
        return -1;   // 分组不存在
    }

    // 显式事务级联删除（SQLite 外键默认关闭，不依赖 PRAGMA foreign_keys）
    if (!db.transaction()) {
        lastError_ = toS(db.lastError().text());
        return -1;
    }
    int removedFavorites = 0;
    QSqlQuery countFav(db);
    countFav.prepare(QStringLiteral("SELECT COUNT(*) FROM favorites WHERE case_id = ?"));
    countFav.addBindValue(caseId);
    if (countFav.exec() && countFav.next()) {
        removedFavorites = countFav.value(0).toInt();
    }
    QSqlQuery delFav(db);
    delFav.prepare(QStringLiteral("DELETE FROM favorites WHERE case_id = ?"));
    delFav.addBindValue(caseId);
    QSqlQuery delCase(db);
    delCase.prepare(QStringLiteral("DELETE FROM cases WHERE id = ?"));
    delCase.addBindValue(caseId);
    if (!delFav.exec() || !delCase.exec() || !db.commit()) {
        db.rollback();
        lastError_ = toS(delFav.lastError().text().isEmpty()
                             ? delCase.lastError().text()
                             : delFav.lastError().text());
        return -1;
    }
    return removedFavorites;
}

std::vector<FavoriteCase> FavoriteStore::cases() const {
    std::vector<FavoriteCase> out;
    if (!opened_) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "SELECT c.id, c.name, c.note, c.created_at, "
        "  (SELECT COUNT(*) FROM favorites f WHERE f.case_id = c.id) "
        "FROM cases c ORDER BY c.id"));
    while (query.next()) {
        out.push_back(caseFromQuery(query));
    }
    return out;
}

int FavoriteStore::caseCount() const {
    if (!opened_) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral("SELECT COUNT(*) FROM cases"));
    return query.next() ? query.value(0).toInt() : 0;
}

// ── 收藏项 ──

long long FavoriteStore::addFavorite(const FavoriteItem& item) {
    if (!opened_ || item.caseId <= 0 || item.docId.empty() || item.chunkIndex < 0) {
        return 0;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);

    // 重复收藏 = 刷新快照并复活（note 是用户资产，不覆盖）
    QSqlQuery exists(db);
    exists.prepare(QStringLiteral(
        "SELECT id FROM favorites WHERE case_id = ? AND doc_id = ? AND chunk_index = ?"));
    exists.addBindValue(item.caseId);
    exists.addBindValue(toQ(item.docId));
    exists.addBindValue(item.chunkIndex);
    if (exists.exec() && exists.next()) {
        const long long id = exists.value(0).toLongLong();
        QSqlQuery update(db);
        update.prepare(QStringLiteral(
            "UPDATE favorites SET title = ?, snippet = ?, score = ?, valid = 1 "
            "WHERE id = ?"));
        update.addBindValue(toQ(item.title));
        update.addBindValue(toQ(item.snippet));
        update.addBindValue(item.score);
        update.addBindValue(id);
        if (!update.exec()) {
            lastError_ = toS(update.lastError().text());
            return 0;
        }
        return id;
    }

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO favorites "
        "(case_id, doc_id, chunk_index, title, snippet, note, score, valid, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?)"));
    insert.addBindValue(item.caseId);
    insert.addBindValue(toQ(item.docId));
    insert.addBindValue(item.chunkIndex);
    insert.addBindValue(toQ(item.title));
    insert.addBindValue(toQ(item.snippet));
    insert.addBindValue(toQ(item.note));
    insert.addBindValue(item.score);
    insert.addBindValue(item.createdAt.empty() ? currentTimestamp()
                                               : toQ(item.createdAt));
    if (!insert.exec()) {
        lastError_ = toS(insert.lastError().text());
        return 0;
    }
    return insert.lastInsertId().toLongLong();
}

bool FavoriteStore::removeFavorite(long long id) {
    if (!opened_ || id <= 0) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM favorites WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        lastError_ = toS(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool FavoriteStore::setFavoriteNote(long long id, const std::string& note) {
    if (!opened_ || id <= 0) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE favorites SET note = ? WHERE id = ?"));
    query.addBindValue(toQ(note));
    query.addBindValue(id);
    if (!query.exec()) {
        lastError_ = toS(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() > 0;
}

std::vector<FavoriteItem> FavoriteStore::favoritesOf(long long caseId) const {
    std::vector<FavoriteItem> out;
    if (!opened_ || caseId <= 0) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT id, case_id, doc_id, chunk_index, title, snippet, note, score, valid, created_at "
        "FROM favorites WHERE case_id = ? ORDER BY created_at DESC, id DESC"));
    query.addBindValue(caseId);
    query.exec();
    while (query.next()) {
        out.push_back(itemFromQuery(query));
    }
    return out;
}

std::vector<FavoriteItem> FavoriteStore::allFavorites() const {
    std::vector<FavoriteItem> out;
    if (!opened_) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "SELECT id, case_id, doc_id, chunk_index, title, snippet, note, score, valid, created_at "
        "FROM favorites ORDER BY case_id, created_at DESC, id DESC"));
    while (query.next()) {
        out.push_back(itemFromQuery(query));
    }
    return out;
}

int FavoriteStore::favoriteCountOf(long long caseId) const {
    if (!opened_ || caseId <= 0) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM favorites WHERE case_id = ?"));
    query.addBindValue(caseId);
    return (query.exec() && query.next()) ? query.value(0).toInt() : 0;
}

// ── 文档删除联动 ──

int FavoriteStore::markInvalidByDocIds(const std::vector<std::string>& docIds) {
    if (!opened_ || docIds.empty()) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    int flipped = 0;
    for (const auto& docId : docIds) {
        if (docId.empty()) {
            continue;
        }
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "UPDATE favorites SET valid = 0 WHERE doc_id = ? AND valid = 1"));
        query.addBindValue(toQ(docId));
        if (!query.exec()) {
            lastError_ = toS(query.lastError().text());
            return flipped;
        }
        flipped += query.numRowsAffected();
    }
    return flipped;
}

}  // namespace favorite
