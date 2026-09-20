#include "ui/library_page.h"
#include "ui/app_theme.h"

#include <QAbstractItemView>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <sstream>

namespace {

/// 给按钮打 role 属性，配色交给全局 QSS 的属性选择器
void setButtonRole(QPushButton* button, const char* role) {
    button->setProperty("role", QString::fromUtf8(role));
}

/// 字节数可读化（1.2 KB / 3.4 MB）
QString humanBytes(std::uint64_t bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    }
    const double mb = kb / 1024.0;
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 2);
}

/// 结果倾向 → 中文标签（未知时显示"—"，避免界面出现空洞）
QString tendencyText(document::ResultTendency tendency) {
    if (tendency == document::ResultTendency::Unknown) {
        return QStringLiteral("—");
    }
    return QString::fromUtf8(document::resultTendencyLabel(tendency));
}

constexpr int kMaxDetailChars = 20000;

}  // namespace

LibraryPage::LibraryPage(rag::Retriever* retriever, QWidget* parent)
    : QWidget(parent)
    , retriever_(retriever)
{
    setupUi();
    refresh();
}

LibraryPage::~LibraryPage() = default;

// ── UI 搭建 ──
void LibraryPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 16, 22, 14);
    root->setSpacing(10);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    auto* titleLabel = new QLabel(QStringLiteral("文档库"), this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(
        QStringLiteral("已导入文书管理 · 索引持久化（重启自动恢复）"), this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(titleLabel);
    head->addWidget(subtitleLabel);
    head->addStretch();
    root->addLayout(head);

    // ── 工具栏 ──
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(10);

    refreshBtn_ = new QPushButton(QStringLiteral("刷新"), this);
    refreshBtn_->setObjectName(QStringLiteral("libraryRefreshBtn"));

    removeBtn_ = new QPushButton(QStringLiteral("删除选中"), this);
    removeBtn_->setObjectName(QStringLiteral("libraryRemoveBtn"));
    setButtonRole(removeBtn_, "danger");

    clearBtn_ = new QPushButton(QStringLiteral("清空重建"), this);
    clearBtn_->setObjectName(QStringLiteral("libraryClearBtn"));
    setButtonRole(clearBtn_, "danger");

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("hint"));

    hint_ = new QLabel(QStringLiteral("提示：双击行可在右侧查看元数据与全文"), this);
    hint_->setObjectName(QStringLiteral("hint"));

    toolbar->addWidget(refreshBtn_);
    toolbar->addWidget(removeBtn_);
    toolbar->addWidget(clearBtn_);
    toolbar->addSpacing(8);
    toolbar->addWidget(summary_);
    toolbar->addStretch();
    toolbar->addWidget(hint_);
    root->addLayout(toolbar);

    // ── 主体：左表格 + 右详情 ──
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);

    auto* leftCard = new QFrame(this);
    leftCard->setObjectName(QStringLiteral("card"));
    auto* leftLayout = new QVBoxLayout(leftCard);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    auto* tableTitle = new QLabel(QStringLiteral("已导入文书"), leftCard);
    tableTitle->setObjectName(QStringLiteral("cardTitle"));
    leftLayout->addWidget(tableTitle);

    table_ = new QTableWidget(leftCard);
    table_->setObjectName(QStringLiteral("libraryTable"));
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({
        QStringLiteral("文件名"),
        QStringLiteral("类型"),
        QStringLiteral("块数"),
        QStringLiteral("案件类型"),
        QStringLiteral("结果倾向"),
        QStringLiteral("导入时间"),
    });
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setWordWrap(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 6; ++c) {
        table_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    }
    leftLayout->addWidget(table_, 1);
    split->addWidget(leftCard);

    auto* rightCard = new QFrame(this);
    rightCard->setObjectName(QStringLiteral("card"));
    auto* rightLayout = new QVBoxLayout(rightCard);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    detailTitle_ = new QLabel(QStringLiteral("文书详情"), rightCard);
    detailTitle_->setObjectName(QStringLiteral("cardTitle"));
    rightLayout->addWidget(detailTitle_);

    detailArea_ = new QTextEdit(rightCard);
    detailArea_->setObjectName(QStringLiteral("libraryDetail"));
    detailArea_->setReadOnly(true);
    detailArea_->setPlaceholderText(
        QStringLiteral("选中左侧任意一行，这里显示该文书的元数据与全文…"));
    rightLayout->addWidget(detailArea_, 1);
    split->addWidget(rightCard);

    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 4);
    root->addWidget(split, 1);

    // ── 信号 ──
    connect(refreshBtn_, &QPushButton::clicked, this, &LibraryPage::refresh);
    connect(removeBtn_, &QPushButton::clicked, this, &LibraryPage::onRemoveSelected);
    connect(clearBtn_, &QPushButton::clicked, this, &LibraryPage::onClearAll);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &LibraryPage::onSelectionChanged);
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &LibraryPage::onRowActivated);
}

// ── 刷新 ──
void LibraryPage::refresh() {
    rebuildTable();
    updateSummary();
    clearDetails();
}

void LibraryPage::rebuildTable() {
    if (!table_) return;

    table_->setRowCount(0);
    if (!retriever_) return;

    const auto infos = retriever_->documentInfos();
    table_->setRowCount(static_cast<int>(infos.size()));

    for (int row = 0; row < static_cast<int>(infos.size()); ++row) {
        const auto& info = infos[row];

        // 文件名
        auto* nameItem = new QTableWidgetItem(QString::fromStdString(info.docId));
        nameItem->setData(Qt::UserRole, QString::fromStdString(info.docId));
        nameItem->setToolTip(QString::fromStdString(info.sourcePath));
        table_->setItem(row, 0, nameItem);

        // 类型（文本 / 扫描件 OCR）
        table_->setItem(row, 1, new QTableWidgetItem(
            info.ocr ? QStringLiteral("扫描件 OCR") : QStringLiteral("文本")));

        // 块数
        auto* chunkItem = new QTableWidgetItem(QString::number(info.chunkCount));
        chunkItem->setTextAlignment(Qt::AlignCenter);
        table_->setItem(row, 2, chunkItem);

        // 案件类型
        const auto* meta = retriever_->getMetadata(info.docId);
        const QString caseType = (meta && !meta->caseType.empty())
            ? QString::fromStdString(meta->caseType)
            : QStringLiteral("—");
        table_->setItem(row, 3, new QTableWidgetItem(caseType));

        // 结果倾向（第 8 类元数据）
        table_->setItem(row, 4, new QTableWidgetItem(tendencyText(info.tendency)));

        // 导入时间
        table_->setItem(row, 5, new QTableWidgetItem(
            QString::fromStdString(info.importedAt)));
    }
}

void LibraryPage::updateSummary() {
    if (!summary_) return;
    if (!retriever_) {
        summary_->setText(QStringLiteral("引擎未就绪"));
        return;
    }

    const int docs = retriever_->documentCount();
    const int chunks = retriever_->chunkCount();
    QString text = QStringLiteral("共 %1 篇 / %2 文本块").arg(docs).arg(chunks);
    if (retriever_->hasPersistedIndex()) {
        text += QStringLiteral(" · 索引已落盘");
    } else {
        text += QStringLiteral(" · 索引未落盘");
    }
    summary_->setText(text);
}

int LibraryPage::rowCount() const {
    return table_ ? table_->rowCount() : 0;
}

// ── 选中/详情 ──
QStringList LibraryPage::selectedDocIds() const {
    QStringList ids;
    if (!table_) return ids;

    const auto rows = table_->selectionModel()
                          ? table_->selectionModel()->selectedRows()
                          : QModelIndexList();
    for (const auto& index : rows) {
        auto* item = table_->item(index.row(), 0);
        if (item) {
            ids << item->data(Qt::UserRole).toString();
        }
    }
    return ids;
}

void LibraryPage::onSelectionChanged() {
    const QStringList ids = selectedDocIds();
    if (removeBtn_) {
        removeBtn_->setEnabled(!ids.isEmpty());
    }
    if (ids.size() == 1) {
        showDetails(ids.first());
    }
}

void LibraryPage::onRowActivated(int row, int /*column*/) {
    auto* item = table_->item(row, 0);
    if (item) {
        showDetails(item->data(Qt::UserRole).toString());
    }
}

void LibraryPage::showDetails(const QString& docId) {
    if (!detailArea_ || !retriever_) return;

    const QString id = docId;
    if (id.isEmpty()) {
        clearDetails();
        return;
    }

    rag::DocumentInfo info;
    if (!retriever_->getDocumentInfo(id.toStdString(), info)) {
        clearDetails();
        return;
    }

    const auto* meta = retriever_->getMetadata(id.toStdString());

    QString text;
    text += QStringLiteral("◆ 文件：%1\n").arg(id);
    if (!info.sourcePath.empty()) {
        text += QStringLiteral("◆ 路径：%1\n")
                    .arg(QString::fromStdString(info.sourcePath));
    }
    text += QStringLiteral("◆ 来源：%1\n")
                .arg(info.ocr ? QStringLiteral("扫描件（OCR 识别文本已持久化，重开不重跑）")
                              : QStringLiteral("文本文件"));
    text += QStringLiteral("◆ 文本块：%1 块   原文：%2\n")
                .arg(info.chunkCount)
                .arg(humanBytes(info.byteSize));
    text += QStringLiteral("◆ 导入时间：%1\n")
                .arg(QString::fromStdString(info.importedAt));

    text += QStringLiteral("\n── 元数据（8 类）──\n");
    if (meta) {
        text += QStringLiteral("案号　　：%1\n")
                    .arg(meta->caseNumber.empty() ? QStringLiteral("—")
                                                  : QString::fromStdString(meta->caseNumber));
        text += QStringLiteral("法院　　：%1\n")
                    .arg(meta->court.empty() ? QStringLiteral("—")
                                             : QString::fromStdString(meta->court));
        text += QStringLiteral("裁判日期：%1\n")
                    .arg(meta->date.empty() ? QStringLiteral("—")
                                            : QString::fromStdString(meta->date));
        text += QStringLiteral("案件类型：%1\n")
                    .arg(meta->caseType.empty() ? QStringLiteral("—")
                                                : QString::fromStdString(meta->caseType));
        text += QStringLiteral("审判程序：%1\n")
                    .arg(meta->procedure.empty() ? QStringLiteral("—")
                                                 : QString::fromStdString(meta->procedure));
        text += QStringLiteral("当事人　：%1\n")
                    .arg(meta->litigants.empty() ? QStringLiteral("—")
                                                 : QString::fromStdString(meta->litigants));
        text += QStringLiteral("结果倾向：%1\n").arg(tendencyText(meta->tendency));
    } else {
        text += QStringLiteral("（未提取到元数据）\n");
    }

    // 全文预览（T10 将在此基础上升级为独立阅读页；此处先给可核对原文）
    std::string full;
    if (retriever_->getFullText(id.toStdString(), full)) {
        QString body = QString::fromStdString(full);
        const bool truncated = body.size() > kMaxDetailChars;
        if (truncated) {
            body = body.left(kMaxDetailChars);
        }
        text += QStringLiteral("\n── 全文原文%1 ──\n")
                    .arg(truncated ? QStringLiteral("（前 %1 字，已截断）").arg(kMaxDetailChars)
                                   : QString());
        text += body;
    }

    detailTitle_->setText(QStringLiteral("文书详情 · %1").arg(id));
    detailArea_->setPlainText(text);
}

void LibraryPage::clearDetails() {
    if (detailArea_) {
        detailArea_->clear();
    }
    if (detailTitle_) {
        detailTitle_->setText(QStringLiteral("文书详情"));
    }
}

// ── 删除 ──
bool LibraryPage::removeDocumentById(const QString& docId, bool confirm) {
    if (!retriever_ || docId.isEmpty()) return false;

    if (confirm) {
        const auto reply = QMessageBox::question(
            this,
            QStringLiteral("确认删除"),
            QStringLiteral("确定要从索引中删除《%1》吗？\n"
                           "删除后其内容不再被检索命中；该操作不可撤销。").arg(docId),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return false;
        }
    }

    const int removed = retriever_->removeDocument(docId.toStdString());
    if (removed <= 0) {
        // 引擎侧不存在该文档：刷新界面保持一致，但不报错（幂等）
        refresh();
        return false;
    }

    refresh();
    emit libraryChanged();
    return true;
}

void LibraryPage::onRemoveSelected() {
    const QStringList ids = selectedDocIds();
    if (ids.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先在列表中选中要删除的文书。"));
        return;
    }

    const int count = ids.size();
    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("确认删除"),
        count == 1
            ? QStringLiteral("确定要从索引中删除《%1》吗？\n"
                             "删除后其内容不再被检索命中；该操作不可撤销。").arg(ids.first())
            : QStringLiteral("确定要删除选中的 %1 篇文书吗？\n"
                             "删除后其内容不再被检索命中；该操作不可撤销。").arg(count),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    int removedDocs = 0;
    int removedChunks = 0;
    for (const auto& id : ids) {
        const int chunks = retriever_->removeDocument(id.toStdString());
        if (chunks > 0) {
            ++removedDocs;
            removedChunks += chunks;
        }
    }

    refresh();
    if (hint_) {
        hint_->setText(QStringLiteral("✓ 已删除 %1 篇文书（释放 %2 个文本块）")
                           .arg(removedDocs).arg(removedChunks));
    }
    emit libraryChanged();
}

void LibraryPage::onClearAll() {
    if (!retriever_) return;
    if (retriever_->documentCount() == 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("文档库已经是空的。"));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("确认清空重建"),
        QStringLiteral("确定要清空全部 %1 篇文书并删除落盘索引吗？\n"
                       "清空后需要重新导入文档；此操作不可撤销。")
            .arg(retriever_->documentCount()),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    retriever_->clearAll(/*alsoDeletePersistedFile=*/true);
    refresh();
    if (hint_) {
        hint_->setText(QStringLiteral("✓ 已清空文档库与落盘索引"));
    }
    emit libraryChanged();
}
