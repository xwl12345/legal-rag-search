#include "law/law_store.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>
#include <atomic>

#include "config/app_config.h"

namespace law {
namespace {

/// 具名连接序号：每个实例一个独立连接名，便于测试里并存多个实例
std::atomic<int> g_connectionSeq{0};

QString toQ(const std::string& s) { return QString::fromStdString(s); }
std::string toS(const QString& s) { return s.toStdString(); }

LawInfo lawInfoFromQuery(QSqlQuery& query) {
    LawInfo info;
    info.name = toS(query.value(0).toString());
    info.amendment = toS(query.value(1).toString());
    info.sourceDesc = toS(query.value(2).toString());
    info.sourceUrl = toS(query.value(3).toString());
    info.fetchedAt = toS(query.value(4).toString());
    return info;
}

LawArticle articleFromQuery(QSqlQuery& query) {
    LawArticle article;
    article.lawName = toS(query.value(0).toString());
    article.articleNo = toS(query.value(1).toString());
    article.text = toS(query.value(2).toString());
    article.sourceDesc = toS(query.value(3).toString());
    article.sourceUrl = toS(query.value(4).toString());
    return article;
}

/// 条号排序键：中文条号转数字（第一千零七十九条 → 1079），
/// 转不动（异常输入）的排最后。仅用于展示顺序，正文不受影响。
long long articleNoKey(const std::string& no) {
    static const std::string kDigits = "零一二三四五六七八九";
    // 剥掉「第」前缀（E7 AC AC）与「条」后缀（E6 9D A1）
    std::string body = no;
    const std::string kQian = "\xe7\xac\xac";   // 第
    const std::string kTiao = "\xe6\x9d\xa1";   // 条
    if (body.compare(0, kQian.size(), kQian) == 0) {
        body.erase(0, kQian.size());
    }
    if (body.size() >= kTiao.size() &&
        body.compare(body.size() - kTiao.size(), kTiao.size(), kTiao) == 0) {
        body.erase(body.size() - kTiao.size());
    }
    long long total = 0, num = 0;
    bool ok = !body.empty();
    for (size_t i = 0; ok && i < body.size(); i += 3) {
        if (i + 3 > body.size()) {  // 残缺的 UTF-8 序列
            return -1;
        }
        const std::string ch = body.substr(i, 3);
        const size_t d = kDigits.find(ch);
        if (d != std::string::npos) {
            num = static_cast<long long>(d) / 3;  // 字节偏移 → 数字（UTF-8 汉字 3 字节）
            continue;
        }
        long long unit = 0;
        if (ch == "\xe5\x8d\x81") unit = 10;        // 十
        else if (ch == "\xe7\x99\xbe") unit = 100;  // 百
        else if (ch == "\xe5\x8d\x83") unit = 1000; // 千
        if (unit == 0) {
            return -1;  // 非数字字符
        }
        total += (num > 0 ? num : 1) * unit;
        num = 0;
    }
    return ok ? total + num : -1;
}

}  // namespace

LawStore::LawStore(std::string dbPath) : dbPath_(std::move(dbPath)) {}

LawStore::LawStore() : dbPath_(config::dataFilePath(config::LAW_DB)) {}

LawStore::~LawStore() {
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

bool LawStore::open() {
    if (opened_) {
        return true;
    }
    connectionName_ = QStringLiteral("law_conn_%1")
                          .arg(g_connectionSeq.fetch_add(1));

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                connectionName_);
    db.setDatabaseName(toQ(dbPath_));
    if (!db.open()) {
        lastError_ = toS(db.lastError().text());
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }

    // 库结构自检：表不存在（文件缺失/被替换）时如实报错，不静默当空库
    QSqlQuery check(db);
    const bool hasArticles = check.exec(
        QStringLiteral("SELECT 1 FROM sqlite_master "
                       "WHERE type='table' AND name='articles'")) &&
        check.next();
    if (!hasArticles) {
        lastError_ = "law_corpus.db 结构异常（缺少 articles 表）";
        db.close();
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }

    opened_ = true;
    return true;
}

std::string LawStore::lastError() const { return lastError_; }

std::vector<LawInfo> LawStore::laws() const {
    std::vector<LawInfo> out;
    if (!opened_) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "SELECT name, amendment, source_desc, source_url, fetched_at "
        "FROM laws ORDER BY id"));
    while (query.next()) {
        out.push_back(lawInfoFromQuery(query));
    }
    return out;
}

int LawStore::lawCount() const {
    if (!opened_) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral("SELECT COUNT(*) FROM laws"));
    return query.next() ? query.value(0).toInt() : 0;
}

int LawStore::articleCount() const {
    if (!opened_) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral("SELECT COUNT(*) FROM articles"));
    return query.next() ? query.value(0).toInt() : 0;
}

bool LawStore::findByArticle(const std::string& lawName,
                             const std::string& articleNo,
                             LawArticle& out) const {
    if (!opened_) {
        return false;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT l.name, a.article_no, a.article_text, l.source_desc, l.source_url "
        "FROM articles a JOIN laws l ON a.law_id = l.id "
        "WHERE l.name = ? AND a.article_no = ?"));
    query.addBindValue(toQ(lawName));
    query.addBindValue(toQ(articleNo));
    if (!query.exec() || !query.next()) {
        return false;
    }
    out = articleFromQuery(query);
    return true;
}

std::vector<LawArticle> LawStore::findByArticleNo(
    const std::string& articleNo) const {
    std::vector<LawArticle> out;
    if (!opened_) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT l.name, a.article_no, a.article_text, l.source_desc, l.source_url "
        "FROM articles a JOIN laws l ON a.law_id = l.id "
        "WHERE a.article_no = ? ORDER BY l.id"));
    query.addBindValue(toQ(articleNo));
    query.exec();
    while (query.next()) {
        out.push_back(articleFromQuery(query));
    }
    return out;
}

std::vector<LawArticle> LawStore::articlesOf(const std::string& lawName) const {
    std::vector<LawArticle> out;
    if (!opened_) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT l.name, a.article_no, a.article_text, l.source_desc, l.source_url "
        "FROM articles a JOIN laws l ON a.law_id = l.id "
        "WHERE l.name = ?"));
    query.addBindValue(toQ(lawName));
    query.exec();
    while (query.next()) {
        out.push_back(articleFromQuery(query));
    }
    // 条号按数值升序（第一条 < 第十条 < 第二条 的字典序陷阱在这里消掉）
    std::sort(out.begin(), out.end(),
              [](const LawArticle& a, const LawArticle& b) {
                  return articleNoKey(a.articleNo) < articleNoKey(b.articleNo);
              });
    return out;
}

std::vector<LawArticle> LawStore::allArticles() const {
    std::vector<LawArticle> out;
    if (!opened_) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    QSqlQuery query(db);
    query.exec(QStringLiteral(
        "SELECT l.name, a.article_no, a.article_text, l.source_desc, l.source_url "
        "FROM articles a JOIN laws l ON a.law_id = l.id ORDER BY l.id"));
    while (query.next()) {
        out.push_back(articleFromQuery(query));
    }
    // 同一部法律内按条号数值升序；法律间保持 id 序（与 laws() 一致）
    std::stable_sort(out.begin(), out.end(),
                     [](const LawArticle& a, const LawArticle& b) {
                         return a.lawName == b.lawName &&
                                articleNoKey(a.articleNo) <
                                    articleNoKey(b.articleNo);
                     });
    return out;
}

}  // namespace law
