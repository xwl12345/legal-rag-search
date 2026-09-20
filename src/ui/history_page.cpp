#include "ui/history_page.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "history/history_store.h"

namespace {

/// 给按钮打 role 属性，配色交给全局 QSS 的属性选择器
void setButtonRole(QPushButton* button, const char* role) {
    button->setProperty("role", QString::fromUtf8(role));
}

/// 详情页里列的片段预览长度（超出折叠，避免右侧被长文本撑爆）
constexpr int kMaxSnippetChars = 160;

QString answerStateText(const history::HistoryRecord& record) {
    return record.interrupted ? QStringLiteral("⚠ 未完成") : QStringLiteral("完整");
}

QString previewOf(const QString& text, int maxChars) {
    QString out = text;
    out.replace(QLatin1Char('\n'), QLatin1Char(' '));
    out.replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (out.size() > maxChars) {
        out = out.left(maxChars) + QStringLiteral("…");
    }
    return out;
}

}  // namespace

HistoryPage::HistoryPage(history::HistoryStore* store, QWidget* parent)
    : QWidget(parent)
    , store_(store)
{
    setupUi();
    refresh();
}

HistoryPage::~HistoryPage() = default;

// ── UI 搭建 ──
void HistoryPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 16, 22, 14);
    root->setSpacing(10);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    auto* titleLabel = new QLabel(QStringLiteral("问答历史"), this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(
        QStringLiteral("提问 → 回答 → 命中来源全程留痕 · 重启不丢"), this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(titleLabel);
    head->addWidget(subtitleLabel);
    head->addStretch();
    root->addLayout(head);

    // ── 工具栏 ──
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(10);

    auto* keywordLabel = new QLabel(QStringLiteral("关键词"), this);
    keywordLabel->setObjectName(QStringLiteral("fieldLabel"));

    keywordInput_ = new QLineEdit(this);
    keywordInput_->setObjectName(QStringLiteral("historyKeyword"));
    keywordInput_->setPlaceholderText(
        QStringLiteral("输入关键词，实时筛选「问题」与「回答」"));
    keywordInput_->setMinimumHeight(36);
    keywordInput_->setMinimumWidth(300);

    exportBtn_ = new QPushButton(QStringLiteral("导出 Markdown"), this);
    exportBtn_->setObjectName(QStringLiteral("historyExportBtn"));
    setButtonRole(exportBtn_, "primary");

    removeBtn_ = new QPushButton(QStringLiteral("删除选中"), this);
    removeBtn_->setObjectName(QStringLiteral("historyRemoveBtn"));
    setButtonRole(removeBtn_, "danger");

    clearBtn_ = new QPushButton(QStringLiteral("清空历史"), this);
    clearBtn_->setObjectName(QStringLiteral("historyClearBtn"));
    setButtonRole(clearBtn_, "danger");

    refreshBtn_ = new QPushButton(QStringLiteral("刷新"), this);
    refreshBtn_->setObjectName(QStringLiteral("historyRefreshBtn"));

    toolbar->addWidget(keywordLabel);
    toolbar->addWidget(keywordInput_);
    toolbar->addSpacing(4);
    toolbar->addWidget(exportBtn_);
    toolbar->addWidget(removeBtn_);
    toolbar->addWidget(clearBtn_);
    toolbar->addWidget(refreshBtn_);
    toolbar->addStretch();
    root->addLayout(toolbar);

    // ── 统计 / 提示 ──
    auto* infoLayout = new QHBoxLayout();
    infoLayout->setSpacing(8);
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("hint"));
    hint_ = new QLabel(QStringLiteral("选中一行查看完整问答，双击亦可"), this);
    hint_->setObjectName(QStringLiteral("hint"));
    infoLayout->addWidget(summary_);
    infoLayout->addStretch();
    infoLayout->addWidget(hint_);
    root->addLayout(infoLayout);

    // ── 主体：左列表 + 右详情 ──
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);

    auto* leftCard = new QFrame(this);
    leftCard->setObjectName(QStringLiteral("card"));
    auto* leftLayout = new QVBoxLayout(leftCard);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    auto* tableTitle = new QLabel(QStringLiteral("历史记录（新在上）"), leftCard);
    tableTitle->setObjectName(QStringLiteral("cardTitle"));
    leftLayout->addWidget(tableTitle);

    table_ = new QTableWidget(leftCard);
    table_->setObjectName(QStringLiteral("historyTable"));
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({
        QStringLiteral("时间"),
        QStringLiteral("问题"),
        QStringLiteral("命中块数"),
        QStringLiteral("回答状态"),
    });
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setWordWrap(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    leftLayout->addWidget(table_, 1);
    split->addWidget(leftCard);

    auto* rightCard = new QFrame(this);
    rightCard->setObjectName(QStringLiteral("card"));
    auto* rightLayout = new QVBoxLayout(rightCard);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    detailTitle_ = new QLabel(QStringLiteral("问答详情"), rightCard);
    detailTitle_->setObjectName(QStringLiteral("cardTitle"));
    rightLayout->addWidget(detailTitle_);

    detailArea_ = new QTextEdit(rightCard);
    detailArea_->setObjectName(QStringLiteral("historyDetail"));
    detailArea_->setReadOnly(true);
    detailArea_->setPlaceholderText(
        QStringLiteral("选中左侧任意一行，这里显示完整的问题、回答与命中来源…"));
    rightLayout->addWidget(detailArea_, 1);
    split->addWidget(rightCard);

    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 4);
    root->addWidget(split, 1);

    // ── 信号 ──
    connect(keywordInput_, &QLineEdit::textChanged,
            this, &HistoryPage::onKeywordChanged);
    connect(refreshBtn_, &QPushButton::clicked, this, &HistoryPage::refresh);
    connect(exportBtn_, &QPushButton::clicked, this, &HistoryPage::onExportSelected);
    connect(removeBtn_, &QPushButton::clicked, this, &HistoryPage::onRemoveSelected);
    connect(clearBtn_, &QPushButton::clicked, this, &HistoryPage::onClearAll);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &HistoryPage::onSelectionChanged);
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &HistoryPage::onRowActivated);
}

int HistoryPage::rowCount() const {
    return table_ ? table_->rowCount() : 0;
}

// ── 刷新 ──
void HistoryPage::refresh() {
    rebuildTable();
    updateSummary();
    clearDetails();
}

void HistoryPage::rebuildTable() {
    if (!table_) return;

    table_->setRowCount(0);
    if (!store_) return;

    const QString keyword = keywordInput_ ? keywordInput_->text().trimmed() : QString();
    const auto records = store_->search(keyword.toStdString());

    table_->setRowCount(static_cast<int>(records.size()));
    for (int row = 0; row < static_cast<int>(records.size()); ++row) {
        const auto& record = records[row];

        auto* timeItem = new QTableWidgetItem(QString::fromStdString(record.createdAt));
        timeItem->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(record.id));
        table_->setItem(row, 0, timeItem);

        auto* queryItem = new QTableWidgetItem(QString::fromStdString(record.query));
        queryItem->setToolTip(QString::fromStdString(record.query));
        table_->setItem(row, 1, queryItem);

        auto* hitItem = new QTableWidgetItem(QString::number(record.hitCount()));
        hitItem->setTextAlignment(Qt::AlignCenter);
        table_->setItem(row, 2, hitItem);

        table_->setItem(row, 3, new QTableWidgetItem(answerStateText(record)));
    }
}

void HistoryPage::updateSummary() {
    if (!summary_) return;

    if (!store_ || !store_->isOpen()) {
        summary_->setText(QStringLiteral("⚠ 问答历史库不可用（SQLite 打开失败，详见运行日志）"));
        return;
    }

    const int total = store_->count();
    const int shown = rowCount();
    if (shown == total) {
        summary_->setText(QStringLiteral("共 %1 条记录").arg(total));
    } else {
        summary_->setText(QStringLiteral("命中 %1 条 / 共 %2 条记录").arg(shown).arg(total));
    }
}

void HistoryPage::onKeywordChanged() {
    rebuildTable();
    updateSummary();
}

// ── 选中 / 详情 ──
long long HistoryPage::idAtRow(int row) const {
    if (!table_) return 0;
    auto* item = table_->item(row, 0);
    if (!item) return 0;
    return item->data(Qt::UserRole).toLongLong();
}

QStringList HistoryPage::selectedIds() const {
    QStringList ids;
    if (!table_ || !table_->selectionModel()) {
        return ids;
    }
    for (const auto& index : table_->selectionModel()->selectedRows()) {
        const long long id = idAtRow(index.row());
        if (id > 0) {
            ids << QString::number(id);
        }
    }
    return ids;
}

void HistoryPage::onSelectionChanged() {
    const QStringList ids = selectedIds();
    if (removeBtn_) removeBtn_->setEnabled(!ids.isEmpty());
    if (exportBtn_) exportBtn_->setEnabled(!ids.isEmpty());
    if (ids.size() == 1) {
        showDetails(ids.first().toLongLong());
    } else {
        clearDetails();
    }
}

void HistoryPage::onRowActivated(int row, int /*column*/) {
    showDetails(idAtRow(row));
}

void HistoryPage::showDetails(long long id) {
    if (!detailArea_ || !store_ || id <= 0) {
        clearDetails();
        return;
    }

    history::HistoryRecord record;
    if (!store_->get(id, record)) {
        clearDetails();
        return;
    }

    QString text;
    text += QStringLiteral("◆ 记录编号：%1\n").arg(record.id);
    text += QStringLiteral("◆ 提问时间：%1\n").arg(QString::fromStdString(record.createdAt));
    text += QStringLiteral("◆ 命中块数：%1\n").arg(record.hitCount());
    text += QStringLiteral("◆ 回答状态：%1\n").arg(answerStateText(record));
    if (record.interrupted && !record.note.empty()) {
        text += QStringLiteral("　　（%1）\n").arg(QString::fromStdString(record.note));
    }

    text += QStringLiteral("\n── 检索问题 ──\n%1\n").arg(QString::fromStdString(record.query));

    text += record.interrupted
                ? QStringLiteral("\n── AI 回答（未完成）──\n%1\n")
                : QStringLiteral("\n── AI 回答 ──\n%1\n");
    text = text.arg(QString::fromStdString(record.answer));

    text += QStringLiteral("\n── 命中来源（%1 个文本块）──\n").arg(record.hitCount());
    if (record.sources.empty()) {
        text += QStringLiteral("（本次回答没有引用文本块）\n");
    } else {
        for (size_t i = 0; i < record.sources.size(); ++i) {
            const auto& item = record.sources[i];
            text += QStringLiteral("【%1】%2 · 第 %3 块 · 相关度 %4\n")
                        .arg(static_cast<int>(i + 1))
                        .arg(QString::fromStdString(item.docId))
                        .arg(item.chunkIndex)
                        .arg(item.finalScore, 0, 'f', 3);
            text += QStringLiteral("　　%1\n")
                        .arg(previewOf(QString::fromStdString(item.snippet), kMaxSnippetChars));
        }
    }

    if (detailTitle_) {
        detailTitle_->setText(QStringLiteral("问答详情 · 记录 #%1").arg(record.id));
    }
    detailArea_->setPlainText(text);
}

void HistoryPage::clearDetails() {
    if (detailArea_) detailArea_->clear();
    if (detailTitle_) detailTitle_->setText(QStringLiteral("问答详情"));
}

// ── 导出 ──
bool HistoryPage::exportRecordById(long long id, const QString& path, bool confirm) {
    if (!store_ || id <= 0 || path.isEmpty()) {
        return false;
    }

    const auto pathStr = QObject::tr("导出失败");

    history::HistoryRecord record;
    if (!store_->get(id, record)) {
        if (confirm) {
            QMessageBox::warning(this, QStringLiteral("导出失败"),
                                 QStringLiteral("找不到记录 #%1（可能已被删除）。").arg(id));
        }
        return false;
    }

    QString error;
    if (!history::HistoryStore::writeMarkdownFile(
            path, history::HistoryStore::toMarkdown(record), &error)) {
        if (confirm) {
            QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        }
        return false;
    }

    if (confirm) {
        QMessageBox::information(this, QStringLiteral("导出成功"),
                                 QStringLiteral("已导出到：\n%1").arg(path));
    }
    return true;
}

void HistoryPage::onExportSelected() {
    if (!store_) return;

    const QStringList ids = selectedIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先在列表中选中要导出的记录。"));
        return;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString suggested = QStringLiteral("问答历史_%1.md").arg(stamp);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 Markdown"), suggested,
        QStringLiteral("Markdown 文件 (*.md);;所有文件 (*)"));
    if (path.isEmpty()) return;

    // 多选时合并成一个 Markdown 文档（每条一个二级章节）
    std::vector<history::HistoryRecord> records;
    for (const auto& id : ids) {
        history::HistoryRecord record;
        if (store_->get(id.toLongLong(), record)) {
            records.push_back(record);
        }
    }
    if (records.empty()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("选中的记录已不存在，请刷新后重试。"));
        return;
    }

    QString error;
    const std::string markdown = records.size() == 1
        ? history::HistoryStore::toMarkdown(records.front())
        : history::HistoryStore::toMarkdownAll(records);
    if (!history::HistoryStore::writeMarkdownFile(path, markdown, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }

    if (hint_) {
        hint_->setText(QStringLiteral("✓ 已导出 %1 条记录到 %2").arg(records.size()).arg(path));
    }
}

// ── 删除 ──
bool HistoryPage::removeRecordById(long long id, bool confirm) {
    if (!store_ || id <= 0) return false;

    if (confirm) {
        const auto reply = QMessageBox::question(
            this,
            QStringLiteral("确认删除"),
            QStringLiteral("确定要删除这条问答记录吗？删除后不可恢复。"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return false;
        }
    }

    if (!store_->remove(id)) {
        refresh();
        return false;
    }

    refresh();
    return true;
}

void HistoryPage::onRemoveSelected() {
    if (!store_) return;

    const QStringList ids = selectedIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先在列表中选中要删除的记录。"));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("确认删除"),
        QStringLiteral("确定要删除选中的 %1 条问答记录吗？删除后不可恢复。").arg(ids.size()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    int removed = 0;
    for (const auto& id : ids) {
        if (store_->remove(id.toLongLong())) {
            ++removed;
        }
    }

    refresh();
    if (hint_) {
        hint_->setText(QStringLiteral("✓ 已删除 %1 条记录").arg(removed));
    }
}

void HistoryPage::onClearAll() {
    if (!store_) return;

    const int total = store_->count();
    if (total == 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("问答历史已经是空的。"));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("确认清空"),
        QStringLiteral("确定要清空全部 %1 条问答记录吗？删除后不可恢复。").arg(total),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    const int removed = store_->removeAll();
    refresh();
    if (hint_) {
        hint_->setText(QStringLiteral("✓ 已清空 %1 条记录").arg(removed));
    }
}
