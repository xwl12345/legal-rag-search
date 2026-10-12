#include "ui/law_page.h"

#include "law/law_store.h"
#include "rag/retriever.h"
#include "ui/app_theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace {

using law::LawArticle;

/// 条号模式：第 + 中文数字 + 条（与 tools/parse_law_html.py 同一字符集）
const QRegularExpression kArticleNoPattern(
    QStringLiteral("第[一二三四五六七八九十百千零]+条"));

/// 常用简称 → 库内法律名全称（长别名优先匹配，防止短名误命中）
const std::vector<std::pair<QString, QString>>& lawAliases() {
    static const std::vector<std::pair<QString, QString>> kAliases = {
        { QStringLiteral("中华人民共和国民法典（第五编 婚姻家庭）"),
          QStringLiteral("中华人民共和国民法典（第五编 婚姻家庭）") },
        { QStringLiteral("民法典"), QStringLiteral("中华人民共和国民法典（第五编 婚姻家庭）") },
        { QStringLiteral("婚姻家庭编"), QStringLiteral("中华人民共和国民法典（第五编 婚姻家庭）") },
        { QStringLiteral("中华人民共和国劳动合同法"), QStringLiteral("中华人民共和国劳动合同法") },
        { QStringLiteral("劳动合同法"), QStringLiteral("中华人民共和国劳动合同法") },
        { QStringLiteral("中华人民共和国劳动法"), QStringLiteral("中华人民共和国劳动法") },
        { QStringLiteral("劳动法"), QStringLiteral("中华人民共和国劳动法") },
    };
    return kAliases;
}

/// 从查询串里解析「法律名 + 条号」：
///   「民法典 第一千零七十九条」→ (民法典全称, 第一千零七十九条)
///   「第一千零七十九条」       → ("", 第一千零七十九条)
///   「离婚 损害赔偿」           → ("", "")
/// 返回是否命中条号形态。
bool parseQuery(const QString& raw, QString* lawOut, QString* articleNoOut) {
    const QString query = raw.trimmed();
    if (query.isEmpty()) {
        return false;
    }

    // 条号取查询串中最后一个匹配（条号惯例在末尾）
    QString articleNo;
    qsizetype noEnd = -1, noStart = -1;
    auto it = kArticleNoPattern.globalMatch(query);
    while (it.hasNext()) {
        const auto m = it.next();
        noStart = m.capturedStart();
        noEnd = m.capturedEnd();
        articleNo = m.captured(0);
    }
    if (articleNo.isEmpty()) {
        return false;
    }

    // 法律名 = 去掉条号后的剩余文本
    QString rest = (noStart >= 0)
        ? (query.left(noStart) + query.mid(noEnd))
        : query;
    rest = rest.trimmed();
    rest.remove(QRegularExpression(QStringLiteral(
        "^[《\\s·、,，]+|[》\\s·、,，]+$")));

    // 简称归一：长别名优先
    if (!rest.isEmpty()) {
        const auto& aliases = lawAliases();
        for (const auto& [alias, full] : aliases) {  // 已按长度降序排布
            if (rest.contains(alias)) {
                *lawOut = full;
                *articleNoOut = articleNo;
                return true;
            }
        }
    }
    if (rest.isEmpty()) {
        lawOut->clear();
        *articleNoOut = articleNo;
        return true;  // 仅条号 → 跨法律精确定位
    }
    *lawOut = rest;  // 非别名前缀 → 交由精确查询自然落空
    *articleNoOut = articleNo;
    return true;
}

QString articleDocId(const LawArticle& a) {
    return QString::fromStdString(a.lawName) + QStringLiteral("#") +
           QString::fromStdString(a.articleNo);
}

QString resultItemText(const LawArticle& a) {
    QString head = QString::fromStdString(a.text);
    const qsizetype nl = head.indexOf(QLatin1Char('\n'));
    if (nl >= 0) {
        head = head.left(nl);
    }
    if (head.size() > 42) {
        head = head.left(42) + QStringLiteral("…");
    }
    return QStringLiteral("《%1》%2　%3")
        .arg(QString::fromStdString(a.lawName),
             QString::fromStdString(a.articleNo), head);
}

QString escapeHtml(QString text) {
    return text.toHtmlEscaped();
}

}  // namespace

/// 释放结果列表各 item 挂载的条文对象（onSearch 重建前 / 退出结果模式时）
void clearResultPayloads(QListWidget* list);

LawPage::LawPage(law::LawStore* store, rag::Retriever* lawRetriever,
                 QWidget* parent)
    : QWidget(parent), store_(store), lawRetriever_(lawRetriever) {
    setupUi();
}

void LawPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(12);

    // ── 卡片 ──
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("card"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto* titleLabel = new QLabel(QStringLiteral("法条速查"), card);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    cardLayout->addWidget(titleLabel);

    auto* body = new QWidget(card);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(16, 12, 16, 12);
    bodyLayout->setSpacing(10);
    cardLayout->addWidget(body, 1);
    root->addWidget(card, 1);

    // ── 顶部：搜索框 + 统计标签 ──
    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(10);
    queryInput_ = new QLineEdit(body);
    queryInput_->setObjectName(QStringLiteral("lawQueryInput"));
    queryInput_->setPlaceholderText(QStringLiteral(
        "条号精确定位（如：民法典 第一千零七十九条 / 第一千零七十九条）或关键词检索…"));
    queryInput_->setClearButtonEnabled(true);
    queryInput_->setMinimumHeight(34);
    searchBtn_ = new QPushButton(QStringLiteral("检索"), body);
    searchBtn_->setObjectName(QStringLiteral("lawSearchBtn"));
    AppTheme::setButtonRole(searchBtn_, "primary");
    searchBtn_->setMinimumHeight(34);
    searchRow->addWidget(queryInput_, 1);
    searchRow->addWidget(searchBtn_);
    bodyLayout->addLayout(searchRow);

    statsLabel_ = new QLabel(body);
    statsLabel_->setObjectName(QStringLiteral("lawStats"));
    if (store_ != nullptr && store_->isOpen()) {
        statsLabel_->setText(QStringLiteral("收录 %1 部法律 / %2 条　文本来源：官方发布文本（详见条文页脚）")
                                 .arg(store_->lawCount())
                                 .arg(store_->articleCount()));
    } else {
        statsLabel_->setText(QStringLiteral("法条库不可用"));
    }
    bodyLayout->addWidget(statsLabel_);

    // ── 中部：左目录 / 右详情 ──
    auto* splitter = new QSplitter(Qt::Horizontal, body);
    splitter->setChildrenCollapsible(false);

    auto* leftPane = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);
    leftTitle_ = new QLabel(QStringLiteral("法律目录"), leftPane);
    leftTitle_->setObjectName(QStringLiteral("lawLeftTitle"));
    leftLayout->addWidget(leftTitle_);

    tree_ = new QTreeWidget(leftPane);
    tree_->setObjectName(QStringLiteral("lawTree"));
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    leftLayout->addWidget(tree_, 1);

    resultList_ = new QListWidget(leftPane);
    resultList_->setObjectName(QStringLiteral("lawResultList"));
    resultList_->hide();
    leftLayout->addWidget(resultList_, 1);

    detail_ = new QTextBrowser(splitter);
    detail_->setObjectName(QStringLiteral("lawDetail"));
    detail_->setOpenExternalLinks(false);
    splitter->addWidget(leftPane);
    splitter->addWidget(detail_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({360, 720});
    bodyLayout->addWidget(splitter, 1);

    connect(queryInput_, &QLineEdit::returnPressed,
            this, &LawPage::onSearch);
    connect(searchBtn_, &QPushButton::clicked,
            this, &LawPage::onSearch);
    connect(tree_, &QTreeWidget::itemSelectionChanged,
            this, &LawPage::onTreeItemSelected);
    connect(resultList_, &QListWidget::itemSelectionChanged,
            this, &LawPage::onResultItemSelected);
    connect(queryInput_, &QLineEdit::textChanged,
            this, [this](const QString& text) {
                if (text.trimmed().isEmpty() && resultMode_) {
                    exitResultMode();
                }
            });

    rebuildTree();
}

void LawPage::rebuildTree() {
    tree_->clear();
    if (store_ == nullptr || !store_->isOpen()) {
        detail_->setPlainText(QStringLiteral(
            "法条库不可用（law_corpus.db 缺失或损坏）。\n"
            "该库随程序分发；缺失时请用 tools/build_law_db.py 重新生成。"));
        detail_->setEnabled(false);
        searchBtn_->setEnabled(false);
        return;
    }

    const auto laws = store_->laws();
    for (const auto& info : laws) {
        auto* lawItem = new QTreeWidgetItem(tree_);
        lawItem->setData(0, Qt::UserRole, QStringLiteral("law"));
        lawItem->setData(0, Qt::UserRole + 1,
                         QString::fromStdString(info.name));
        lawItem->setText(0, QStringLiteral("《%1》")
                                .arg(QString::fromStdString(info.name)));

        const auto articles = store_->articlesOf(info.name);
        for (const auto& article : articles) {
            auto* artItem = new QTreeWidgetItem(lawItem);
            artItem->setData(0, Qt::UserRole, QStringLiteral("article"));
            artItem->setData(0, Qt::UserRole + 1,
                             QString::fromStdString(info.name));
            artItem->setData(0, Qt::UserRole + 2,
                             QString::fromStdString(article.articleNo));
            artItem->setText(0, QString::fromStdString(article.articleNo));
        }
    }
    tree_->expandToDepth(0);
}

void LawPage::onSearch() {
    if (store_ == nullptr || !store_->isOpen()) {
        return;
    }
    const QString raw = queryInput_->text().trimmed();
    if (raw.isEmpty()) {
        exitResultMode();
        return;
    }

    // ── 分流一：条号形态 → 精确定位（SQLite 结构化查询）──
    QString lawName, articleNo;
    if (parseQuery(raw, &lawName, &articleNo)) {
        LawArticle article;
        if (!lawName.isEmpty()) {
            // 法律名 + 条号
            if (store_->findByArticle(lawName.toStdString(),
                                      articleNo.toStdString(), article)) {
                exitResultMode();
                showArticle(article);
                return;
            }
            // 别名带了但不命中 → 明确告知未收录，不静默转关键词
            exitResultMode();
            detail_->setEnabled(true);
            detail_->setHtml(QStringLiteral(
                "<h3>未收录</h3><p>库中无《%1》%2。</p>"
                "<p>当前收录范围见上方统计标签。</p>")
                .arg(escapeHtml(lawName), escapeHtml(articleNo)));
            return;
        }

        // 仅条号 → 跨法律精确定位
        const auto hits = store_->findByArticleNo(articleNo.toStdString());
        if (!hits.empty()) {
            if (hits.size() == 1) {
                exitResultMode();
                showArticle(hits.front());
                return;
            }
            // 多部法律同名条号 → 结果列表模式，逐条可看
            leftTitle_->setText(QStringLiteral("「%1」命中 %2 条")
                                    .arg(articleNo)
                                    .arg(hits.size()));
            tree_->hide();
            resultList_->clear();
            resultList_->show();
            resultMode_ = true;
            for (const auto& hit : hits) {
                resultList_->addItem(resultItemText(hit));
            }
            return;
        }
        exitResultMode();
        detail_->setEnabled(true);
        detail_->setHtml(QStringLiteral(
            "<h3>未收录</h3><p>库中无 %1。</p>")
            .arg(escapeHtml(articleNo)));
        return;
    }

    // ── 分流二：关键词 → 独立法条检索实例（BM25 混合，无 Key 自动降级纯 BM25）──
    const auto results = lawRetriever_->search(raw.toStdString(), 20);
    if (results.empty()) {
        exitResultMode();
        detail_->setEnabled(true);
        detail_->setHtml(QStringLiteral(
            "<h3>无命中</h3><p>关键词「%1」未命中任何条文。</p>")
            .arg(escapeHtml(raw)));
        return;
    }

    // 同一条文可能多块命中（长文分块），按 docId 去重保序
    std::vector<LawArticle> hits;
    std::map<std::string, int> seen;
    for (const auto& r : results) {
        if (seen.count(r.docId)) {
            continue;
        }
        seen[r.docId] = 1;
        // docId = 法律名 + "#" + 条号
        const QString id = QString::fromStdString(r.docId);
        const qsizetype sep = id.indexOf(QLatin1Char('#'));
        if (sep < 0) {
            continue;  // 非法条 docId（防御；独立实例内不该出现）
        }
        LawArticle article;
        if (store_->findByArticle(id.left(sep).toStdString(),
                                  id.mid(sep + 1).toStdString(), article)) {
            hits.push_back(std::move(article));
        }
    }

    leftTitle_->setText(QStringLiteral("「%1」命中 %2 条")
                            .arg(raw)
                            .arg(hits.size()));
    tree_->hide();
    resultList_->clear();
    resultList_->show();
    resultMode_ = true;
    // 条文对象挂到 item 的 UserRole，选中即渲染（免二次查库）
    for (const auto& article : hits) {
        auto* item = new QListWidgetItem(resultItemText(article), resultList_);
        item->setData(Qt::UserRole, QVariant::fromValue(
            static_cast<void*>(new LawArticle(article))));
    }
    if (resultList_->count() > 0) {
        resultList_->setCurrentRow(0);
    }
}

/// 释放结果列表各 item 挂载的条文对象（onSearch 重建前 / 退出结果模式时）
void clearResultPayloads(QListWidget* list) {
    for (int i = 0; i < list->count(); ++i) {
        auto* item = list->item(i);
        delete static_cast<LawArticle*>(item->data(Qt::UserRole).value<void*>());
        item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(nullptr)));
    }
}

void LawPage::onTreeItemSelected() {
    if (tree_->selectedItems().isEmpty()) {
        return;
    }
    auto* item = tree_->selectedItems().front();
    const QString kind = item->data(0, Qt::UserRole).toString();
    if (kind == QStringLiteral("article")) {
        LawArticle article;
        if (store_->findByArticle(
                item->data(0, Qt::UserRole + 1).toString().toStdString(),
                item->data(0, Qt::UserRole + 2).toString().toStdString(),
                article)) {
            showArticle(article);
        }
        return;
    }
    // 法律节点：显示该法律的收录说明（名称 / 沿革 / 来源）
    const auto laws = store_->laws();
    const QString name = item->data(0, Qt::UserRole + 1).toString();
    for (const auto& info : laws) {
        if (QString::fromStdString(info.name) == name) {
            detail_->setEnabled(true);
            detail_->setHtml(QStringLiteral(
                "<h3>《%1》</h3>"
                "<p><b>收录条文：</b>%2 条</p>"
                "<p><b>文本沿革：</b>%3</p>"
                "<p><b>文本来源：</b>%4</p>"
                "<p><b>来源页：</b>%5</p>")
                .arg(escapeHtml(name))
                .arg(store_->articlesOf(info.name).size())
                .arg(escapeHtml(QString::fromStdString(info.amendment)))
                .arg(escapeHtml(QString::fromStdString(info.sourceDesc)))
                .arg(escapeHtml(QString::fromStdString(info.sourceUrl))));
            return;
        }
    }
}

void LawPage::onResultItemSelected() {
    if (resultList_->selectedItems().isEmpty()) {
        return;
    }
    auto* item = resultList_->selectedItems().front();
    if (auto* article = static_cast<LawArticle*>(
            item->data(Qt::UserRole).value<void*>())) {
        showArticle(*article);
    }
}

void LawPage::showArticle(const LawArticle& article) {
    detail_->setEnabled(true);
    // 条文正文逐段渲染；来源如实带出（T9 第一红线：可溯源）
    QString body;
    const QStringList lines =
        QString::fromStdString(article.text)
            .split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QString t = line.trimmed();
        if (t.isEmpty()) {
            continue;
        }
        body += QStringLiteral("<p>%1</p>").arg(escapeHtml(t));
    }
    detail_->setHtml(QStringLiteral(
        "<h3>《%1》%2</h3>%3<hr>"
        "<p style='color:#6B7280'>文本来源：%4<br>来源页：%5</p>")
        .arg(escapeHtml(QString::fromStdString(article.lawName)),
             escapeHtml(QString::fromStdString(article.articleNo)),
             body,
             escapeHtml(QString::fromStdString(article.sourceDesc)),
             escapeHtml(QString::fromStdString(article.sourceUrl))));
}

void LawPage::exitResultMode() {
    if (!resultMode_) {
        return;
    }
    clearResultPayloads(resultList_);
    resultList_->clear();
    resultList_->hide();
    tree_->show();
    leftTitle_->setText(QStringLiteral("法律目录"));
    resultMode_ = false;
}
