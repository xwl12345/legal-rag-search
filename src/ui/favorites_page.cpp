#include "ui/favorites_page.h"

#include "favorite/favorite_store.h"
#include "ui/app_theme.h"
#include "ui/doc_viewer_dialog.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

constexpr int kColDoc = 0;
constexpr int kColScore = 1;
constexpr int kColStatus = 2;
constexpr int kColTime = 3;
constexpr int kColNote = 4;

/// 状态列文案：失效项明示「原文已删除」——快照仍在，可读
QString statusText(bool valid) {
    return valid ? QStringLiteral("正常") : QStringLiteral("原文已删除");
}

QString scoreText(double score) {
    return QString::number(score, 'f', 2);
}

}  // namespace

FavoritesPage::FavoritesPage(favorite::FavoriteStore* store,
                             rag::Retriever* retriever, QWidget* parent)
    : QWidget(parent)
    , store_(store)
    , retriever_(retriever)
{
    setupUi();
    refresh();
}

FavoritesPage::~FavoritesPage() = default;

void FavoritesPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 16, 22, 14);
    root->setSpacing(10);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    auto* titleLabel = new QLabel(QStringLiteral("案例收藏"), this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(
        QStringLiteral("按案件分组管理检索命中 · 收藏当时的片段快照"), this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(titleLabel);
    head->addWidget(subtitleLabel);
    head->addStretch();
    root->addLayout(head);

    // ── 工具栏 ──
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(10);

    newCaseBtn_ = new QPushButton(QStringLiteral("＋ 新建分组"), this);
    newCaseBtn_->setObjectName(QStringLiteral("favoriteNewCaseBtn"));
    AppTheme::setButtonRole(newCaseBtn_, "primary");

    caseNoteBtn_ = new QPushButton(QStringLiteral("备注"), this);
    caseNoteBtn_->setObjectName(QStringLiteral("favoriteCaseNoteBtn"));

    removeCaseBtn_ = new QPushButton(QStringLiteral("删除分组"), this);
    removeCaseBtn_->setObjectName(QStringLiteral("favoriteRemoveCaseBtn"));
    AppTheme::setButtonRole(removeCaseBtn_, "danger");

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("hint"));

    hint_ = new QLabel(QStringLiteral("提示：文档被删除后，其收藏项标记「原文已删除」，快照仍可阅读"), this);
    hint_->setObjectName(QStringLiteral("hint"));

    toolbar->addWidget(newCaseBtn_);
    toolbar->addWidget(caseNoteBtn_);
    toolbar->addWidget(removeCaseBtn_);
    toolbar->addSpacing(8);
    toolbar->addWidget(summary_);
    toolbar->addStretch();
    toolbar->addWidget(hint_);
    root->addLayout(toolbar);

    // ── 主体：左分组列表 + 右收藏表格 ──
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);

    auto* leftCard = new QFrame(this);
    leftCard->setObjectName(QStringLiteral("card"));
    auto* leftLayout = new QVBoxLayout(leftCard);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    auto* caseTitle = new QLabel(QStringLiteral("案件分组"), leftCard);
    caseTitle->setObjectName(QStringLiteral("cardTitle"));
    leftLayout->addWidget(caseTitle);

    caseList_ = new QListWidget(leftCard);
    caseList_->setObjectName(QStringLiteral("favoriteCaseList"));
    leftLayout->addWidget(caseList_, 1);

    split->addWidget(leftCard);

    auto* rightCard = new QFrame(this);
    rightCard->setObjectName(QStringLiteral("card"));
    auto* rightLayout = new QVBoxLayout(rightCard);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    auto* favTitle = new QLabel(QStringLiteral("收藏条目（按收藏时间倒序）"), rightCard);
    favTitle->setObjectName(QStringLiteral("cardTitle"));
    rightLayout->addWidget(favTitle);

    favoriteTable_ = new QTableWidget(rightCard);
    favoriteTable_->setObjectName(QStringLiteral("favoriteTable"));
    favoriteTable_->setColumnCount(5);
    favoriteTable_->setHorizontalHeaderLabels(
        {QStringLiteral("文书"), QStringLiteral("相关度"), QStringLiteral("状态"),
         QStringLiteral("收藏时间"), QStringLiteral("备注")});
    favoriteTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    favoriteTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    favoriteTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    favoriteTable_->setAlternatingRowColors(true);
    favoriteTable_->horizontalHeader()->setStretchLastSection(true);
    favoriteTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    favoriteTable_->setColumnWidth(kColDoc, 220);
    favoriteTable_->setColumnWidth(kColScore, 70);
    favoriteTable_->setColumnWidth(kColStatus, 90);
    favoriteTable_->setColumnWidth(kColTime, 150);
    rightLayout->addWidget(favoriteTable_, 3);

    auto* detailTitle = new QLabel(QStringLiteral("片段快照"), rightCard);
    detailTitle->setObjectName(QStringLiteral("cardTitle"));
    rightLayout->addWidget(detailTitle);

    detail_ = new QTextBrowser(rightCard);
    detail_->setObjectName(QStringLiteral("favoriteDetail"));
    detail_->setOpenExternalLinks(false);
    rightLayout->addWidget(detail_, 2);

    // 右下角操作按钮
    auto* favToolbar = new QHBoxLayout();
    favToolbar->setSpacing(10);

    favoriteNoteBtn_ = new QPushButton(QStringLiteral("✎ 备注"), rightCard);
    favoriteNoteBtn_->setObjectName(QStringLiteral("favoriteItemNoteBtn"));

    removeFavoriteBtn_ = new QPushButton(QStringLiteral("✕ 移除收藏"), rightCard);
    removeFavoriteBtn_->setObjectName(QStringLiteral("favoriteRemoveItemBtn"));
    AppTheme::setButtonRole(removeFavoriteBtn_, "danger");

    openFullTextBtn_ = new QPushButton(QStringLiteral("📖 打开全文"), rightCard);
    openFullTextBtn_->setObjectName(QStringLiteral("favoriteOpenFullTextBtn"));
    // T10 已落地：默认置灰，选中收藏条目后启用（见 onFavoriteSelectionChanged）
    openFullTextBtn_->setEnabled(false);
    openFullTextBtn_->setToolTip(QStringLiteral("打开该文书的整篇原文；原文已删除时仅可查看快照"));

    favToolbar->addWidget(favoriteNoteBtn_);
    favToolbar->addWidget(removeFavoriteBtn_);
    favToolbar->addStretch();
    favToolbar->addWidget(openFullTextBtn_);
    auto* favToolbarWidget = new QWidget(rightCard);
    favToolbarWidget->setLayout(favToolbar);
    rightLayout->addWidget(favToolbarWidget);

    split->addWidget(rightCard);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 7);

    root->addWidget(split, 1);

    // ── 信号连接（必须写在控件构造之后）──
    connect(newCaseBtn_, &QPushButton::clicked, this, &FavoritesPage::onNewCase);
    connect(caseNoteBtn_, &QPushButton::clicked, this, &FavoritesPage::onEditCaseNote);
    connect(removeCaseBtn_, &QPushButton::clicked, this, &FavoritesPage::onRemoveCase);
    connect(favoriteNoteBtn_, &QPushButton::clicked, this, &FavoritesPage::onEditFavoriteNote);
    connect(removeFavoriteBtn_, &QPushButton::clicked, this, &FavoritesPage::onRemoveFavorite);
    connect(openFullTextBtn_, &QPushButton::clicked, this, &FavoritesPage::onOpenFullText);
    connect(caseList_, &QListWidget::currentRowChanged,
            this, &FavoritesPage::onCaseSelectionChanged);
    connect(favoriteTable_, &QTableWidget::itemSelectionChanged,
            this, &FavoritesPage::onFavoriteSelectionChanged);
}

// ── 刷新 ──

void FavoritesPage::refresh() {
    if (!store_ || !store_->isOpen()) {
        showUnavailable(store_ ? QString::fromStdString(store_->lastError())
                               : QStringLiteral("收藏库未创建"));
        return;
    }

    const long long keepCase = currentCaseId();
    const long long keepFav = selectedFavoriteId();

    rebuildCaseList();
    rebuildFavoriteTable();
    updateSummary();

    // 尽量保持用户此前的选中（刷新后选中项仍指向同一对象）
    if (keepCase > 0) {
        for (int i = 0; i < caseList_->count(); ++i) {
            if (caseList_->item(i)->data(Qt::UserRole).toLongLong() == keepCase) {
                caseList_->setCurrentRow(i);
                break;
            }
        }
    }
    if (keepFav > 0) {
        for (int i = 0; i < favoriteTable_->rowCount(); ++i) {
            if (favoriteTable_->item(i, kColDoc)
                && favoriteTable_->item(i, kColDoc)->data(Qt::UserRole).toLongLong() == keepFav) {
                favoriteTable_->selectRow(i);
                break;
            }
        }
    }
    onFavoriteSelectionChanged();
}

void FavoritesPage::rebuildCaseList() {
    caseList_->clear();
    const auto cases = store_->cases();
    for (const auto& c : cases) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1（%2）").arg(QString::fromStdString(c.name)).arg(c.favoriteCount),
            caseList_);
        item->setData(Qt::UserRole, QVariant::fromValue<long long>(c.id));
        caseList_->addItem(item);
    }
    if (cases.empty()) {
        auto* empty = new QListWidgetItem(
            QStringLiteral("（还没有分组——在检索问答页右键结果「收藏到案例集」，或点「＋ 新建分组」）"),
            caseList_);
        empty->setFlags(Qt::NoItemFlags);
        caseList_->addItem(empty);
    } else if (caseList_->currentRow() < 0) {
        // 无选中时默认选中第一个真实分组：否则右侧表格与详情永远是空的
        // （refresh() 会在本函数之后按需恢复用户此前的选中）
        caseList_->setCurrentRow(0);
    }
}

void FavoritesPage::rebuildFavoriteTable() {
    favoriteTable_->clearContents();
    favoriteTable_->setRowCount(0);

    const long long caseId = currentCaseId();
    if (caseId <= 0) {
        return;
    }
    const auto items = store_->favoritesOf(caseId);
    favoriteTable_->setRowCount(static_cast<int>(items.size()));
    for (int row = 0; row < static_cast<int>(items.size()); ++row) {
        const auto& item = items[row];
        auto* docItem = new QTableWidgetItem(QString::fromStdString(item.docId));
        docItem->setData(Qt::UserRole, QVariant::fromValue<long long>(item.id));
        favoriteTable_->setItem(row, kColDoc, docItem);
        favoriteTable_->setItem(row, kColScore,
                                new QTableWidgetItem(scoreText(item.score)));
        favoriteTable_->setItem(row, kColStatus,
                                new QTableWidgetItem(statusText(item.valid)));
        favoriteTable_->setItem(row, kColTime,
                                new QTableWidgetItem(QString::fromStdString(item.createdAt)));
        favoriteTable_->setItem(row, kColNote,
                                new QTableWidgetItem(QString::fromStdString(item.note)));
    }
    // 无选中行时默认选中第一行：详情区随之显示第一条的快照
    // （用户此前选中由 refresh() 在调用本函数之后恢复）
    if (favoriteTable_->rowCount() > 0 && favoriteTable_->currentRow() < 0) {
        favoriteTable_->selectRow(0);
    }
}

void FavoritesPage::updateSummary() {
    if (summary_) {
        // 收藏总数从分组列表的计数汇总，不必再拉一次全量收藏
        int total = 0;
        for (const auto& c : store_->cases()) {
            total += c.favoriteCount;
        }
        summary_->setText(QStringLiteral("共 %1 个分组 / %2 条收藏")
                              .arg(store_->caseCount()).arg(total));
    }
    const bool hasCase = currentCaseId() > 0;
    caseNoteBtn_->setEnabled(hasCase);
    removeCaseBtn_->setEnabled(hasCase);
    const bool hasFav = selectedFavoriteId() > 0;
    favoriteNoteBtn_->setEnabled(hasFav);
    removeFavoriteBtn_->setEnabled(hasFav);
}

void FavoritesPage::showUnavailable(const QString& reason) {
    caseList_->clear();
    auto* item = new QListWidgetItem(
        QStringLiteral("案例收藏库不可用：%1").arg(reason), caseList_);
    item->setFlags(Qt::NoItemFlags);
    caseList_->addItem(item);
    favoriteTable_->clearContents();
    favoriteTable_->setRowCount(0);
    clearDetail();
    if (summary_) {
        summary_->setText(QStringLiteral("收藏库不可用：%1").arg(reason));
    }
    newCaseBtn_->setEnabled(false);
    caseNoteBtn_->setEnabled(false);
    removeCaseBtn_->setEnabled(false);
    favoriteNoteBtn_->setEnabled(false);
    removeFavoriteBtn_->setEnabled(false);
}

// ── 详情 ──

void FavoritesPage::showFavoriteDetail(const favorite::FavoriteItem& item) {
    QString html;
    html += QStringLiteral("<div style='font-weight:700;color:#14213D;'>%1</div>")
                .arg(QString::fromStdString(item.title).toHtmlEscaped());
    html += QStringLiteral("<div style='color:#64748B;font-size:11px;'>块 #%1 · 相关度 %2 · 收藏于 %3 · 状态：%4</div>")
                .arg(item.chunkIndex)
                .arg(scoreText(item.score))
                .arg(QString::fromStdString(item.createdAt))
                .arg(statusText(item.valid));
    if (!item.note.empty()) {
        html += QStringLiteral("<div style='margin-top:6px;'><b>备注：</b>%1</div>")
                    .arg(QString::fromStdString(item.note).toHtmlEscaped());
    }
    if (!item.valid) {
        html += QStringLiteral("<div style='margin-top:6px;color:#B7791F;font-weight:700;'>"
                               "⚠ 原文已删除——以下为收藏当时的片段快照</div>");
    }
    html += QStringLiteral("<div style='margin-top:8px;white-space:pre-wrap;'>%1</div>")
                .arg(QString::fromStdString(item.snippet).toHtmlEscaped());
    detail_->setHtml(html);
}

void FavoritesPage::clearDetail() {
    detail_->clear();
    detail_->setPlaceholderText(QStringLiteral("选中左侧分组与右侧收藏项后，这里显示片段快照"));
    if (openFullTextBtn_) {
        openFullTextBtn_->setEnabled(false);  // 无选中即无全文可开
    }
}

// ── 选中辅助 ──

long long FavoritesPage::currentCaseId() const {
    auto* item = caseList_->currentItem();
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

long long FavoritesPage::selectedFavoriteId() const {
    const int row = favoriteTable_->currentRow();
    if (row < 0) {
        return 0;
    }
    auto* item = favoriteTable_->item(row, kColDoc);
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

long long FavoritesPage::caseIdByName(const QString& name) const {
    for (const auto& c : store_->cases()) {
        if (QString::fromStdString(c.name) == name) {
            return c.id;
        }
    }
    return 0;
}

// ── 测试断言辅助 ──

int FavoritesPage::caseCount() const {
    if (!store_ || !store_->isOpen()) {
        return 0;
    }
    return store_->caseCount();
}

int FavoritesPage::currentFavoriteCount() const {
    return favoriteTable_->rowCount();
}

QString FavoritesPage::currentCaseName() const {
    const long long id = currentCaseId();
    if (id <= 0 || !store_) {
        return QString();
    }
    for (const auto& c : store_->cases()) {
        if (c.id == id) {
            return QString::fromStdString(c.name);
        }
    }
    return QString();
}

QString FavoritesPage::cellText(int row, int col) const {
    auto* item = favoriteTable_->item(row, col);
    return item ? item->text() : QString();
}

// ── 分组操作 ──

void FavoritesPage::onNewCase() {
    if (!store_ || !store_->isOpen()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("新建分组"),
        QStringLiteral("案件分组名称（如：民间借贷·交付凭证类案）："),
        QLineEdit::Normal, QString(), &ok)
        .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    // 重名不报错：createCase 幂等返回已存在分组，这里给出明确反馈
    if (caseIdByName(name) > 0) {
        QMessageBox::information(this, QStringLiteral("分组已存在"),
                                 QStringLiteral("分组「%1」已存在，已为你选中。").arg(name));
    }
    const long long id = store_->createCase(name.toStdString());
    if (id <= 0) {
        QMessageBox::warning(this, QStringLiteral("新建失败"),
                             QStringLiteral("无法创建分组「%1」。").arg(name));
        return;
    }
    refresh();
    for (int i = 0; i < caseList_->count(); ++i) {
        if (caseList_->item(i)->data(Qt::UserRole).toLongLong() == id) {
            caseList_->setCurrentRow(i);
            break;
        }
    }
}

void FavoritesPage::onEditCaseNote() {
    const long long id = currentCaseId();
    if (!store_ || id <= 0) {
        return;
    }
    QString current;
    for (const auto& c : store_->cases()) {
        if (c.id == id) {
            current = QString::fromStdString(c.note);
        }
    }
    bool ok = false;
    const QString note = QInputDialog::getMultiLineText(
        this, QStringLiteral("分组备注"),
        QStringLiteral("分组「%1」的备注：").arg(currentCaseName()), current, &ok);
    if (!ok) {
        return;
    }
    store_->setCaseNote(id, note.toStdString());
    refresh();
}

void FavoritesPage::onRemoveCase() {
    const long long id = currentCaseId();
    if (!store_ || id <= 0) {
        return;
    }
    const int favCount = store_->favoriteCountOf(id);
    const auto reply = QMessageBox::question(
        this, QStringLiteral("确认删除分组"),
        favCount > 0
            ? QStringLiteral("确定要删除分组「%1」吗？\n其下 %2 条收藏将一并删除，快照不再保留。")
                  .arg(currentCaseName()).arg(favCount)
            : QStringLiteral("确定要删除分组「%1」吗？").arg(currentCaseName()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }
    store_->removeCase(id);
    refresh();
}

// ── 收藏操作 ──

void FavoritesPage::onFavoriteSelectionChanged() {
    updateSummary();
    const long long id = selectedFavoriteId();
    if (id <= 0 || !store_) {
        clearDetail();
        return;
    }
    // T10：有选中且引擎在位 → 「打开全文」可用（失效条目也启用，
    // 点击时明确提示仅可看快照，不静默降级）
    if (openFullTextBtn_) {
        openFullTextBtn_->setEnabled(retriever_ != nullptr);
    }
    for (const auto& item : store_->favoritesOf(currentCaseId())) {
        if (item.id == id) {
            showFavoriteDetail(item);
            return;
        }
    }
    clearDetail();
}

void FavoritesPage::onEditFavoriteNote() {
    const long long id = selectedFavoriteId();
    if (!store_ || id <= 0) {
        return;
    }
    QString current;
    favorite::FavoriteItem target;
    for (const auto& item : store_->favoritesOf(currentCaseId())) {
        if (item.id == id) {
            current = QString::fromStdString(item.note);
            target = item;
        }
    }
    bool ok = false;
    const QString note = QInputDialog::getMultiLineText(
        this, QStringLiteral("收藏备注"),
        QStringLiteral("《%1》块 #%2 的备注：")
            .arg(QString::fromStdString(target.docId)).arg(target.chunkIndex),
        current, &ok);
    if (!ok) {
        return;
    }
    store_->setFavoriteNote(id, note.toStdString());
    refresh();
}

void FavoritesPage::onRemoveFavorite() {
    removeFavoriteById(selectedFavoriteId(), /*confirm=*/true);
}

bool FavoritesPage::removeFavoriteById(long long id, bool confirm) {
    if (!store_ || id <= 0) {
        return false;
    }
    if (confirm) {
        const auto reply = QMessageBox::question(
            this, QStringLiteral("确认移除"),
            QStringLiteral("确定要从当前分组移除这条收藏吗？快照将不再保留。"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return false;
        }
    }
    const bool removed = store_->removeFavorite(id);
    if (removed) {
        refresh();
    }
    return removed;
}

void FavoritesPage::onOpenFullText() {
    // T10：打开选中收藏的整篇原文。失效项（原文已删除）明确提示仅可看快照
    //（不静默降级——快照在详情区本就可见），不崩溃。
    if (!store_ || !retriever_) {
        return;
    }
    const long long id = selectedFavoriteId();
    if (id <= 0) {
        return;
    }
    for (const auto& item : store_->favoritesOf(currentCaseId())) {
        if (item.id != id) {
            continue;
        }
        if (!item.valid) {
            QMessageBox::information(
                this, QStringLiteral("原文已删除"),
                QStringLiteral("该收藏对应的文书已从文档库删除，整篇原文不可用。\n"
                               "下方的片段快照仍可查看。"));
            return;
        }
        // 命中块即收藏时的块号（单处定位）；弹窗非模态 + 关闭即销毁，
        // 与检索页打开全文同一实现（DocViewerDialog），两页互不引用不变。
        auto* viewer = new DocViewerDialog(
            retriever_, QString::fromStdString(item.docId),
            {item.chunkIndex}, this);
        viewer->setAttribute(Qt::WA_DeleteOnClose);
        viewer->show();
        viewer->raise();
        viewer->activateWindow();
        return;
    }
}

void FavoritesPage::onCaseSelectionChanged() {
    rebuildFavoriteTable();
    onFavoriteSelectionChanged();
}
