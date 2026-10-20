#include "ui/doc_viewer_dialog.h"

#include <algorithm>
#include <utility>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCharFormat>
#include <QTextEdit>  // QTextEdit::ExtraSelection（不赌 qplaintextedit 的传递包含）
#include <QVBoxLayout>

#include "rag/retriever.h"

namespace {

/// 命中块高亮底色（浅铜金，呼应全局主题 #B7791F）。
/// ExtraSelections 是逐片段富文本格式，QSS 描述不了，只能代码内常量；
/// 这不是绕开「样式只进 app.qss」约定——该约定约束的是 setStyleSheet，
/// 全项目唯一调用点仍在 AppTheme::apply()，此处零新增。
const QColor kHighlightBackground(255, 229, 148);

/// 命中列表条目的正文预览长度（字符）
constexpr int kPreviewChars = 24;

}  // namespace

DocViewerDialog::DocViewerDialog(rag::Retriever* retriever,
                                 const QString& docId,
                                 std::vector<int> hitChunkIndexes,
                                 QWidget* parent)
    : QDialog(parent),
      retriever_(retriever),
      docId_(docId),
      title_(docId) {
    setWindowTitle(QStringLiteral("📖 全文阅读 — %1").arg(title_));
    setObjectName(QStringLiteral("docViewerDialog"));
    setModal(false);  // 非模态：读全文的同时允许回到检索页改关键词
    resize(960, 640);

    // 1) 取全文；取不到（文档已删除等）不崩溃，正文区给出说明
    std::string rawText;
    const bool hasText =
        retriever_ && retriever_->getFullText(docId_.toStdString(), rawText);
    if (hasText) {
        fullText_ = QString::fromStdString(rawText);  // Qt 按 UTF-8 解码
    }

    // 2) 逐块取内容并依序定位（块号升序去重，重复命中只留一处）
    std::sort(hitChunkIndexes.begin(), hitChunkIndexes.end());
    hitChunkIndexes.erase(
        std::unique(hitChunkIndexes.begin(), hitChunkIndexes.end()),
        hitChunkIndexes.end());

    std::vector<std::pair<int, QString>> hits;
    hits.reserve(hitChunkIndexes.size());
    for (int idx : hitChunkIndexes) {
        std::string chunkRaw;
        if (retriever_ && retriever_->getChunk(docId_.toStdString(), idx,
                                               chunkRaw)) {
            hits.emplace_back(idx, QString::fromStdString(chunkRaw));
        }
    }
    marks_ = doc_viewer::locateHits(fullText_, hits);

    setupUi();
    applyHighlights();

    // 默认定位到第一处命中（选中右侧列表首项会触发 onHitSelected 完成跳转）
    if (!marks_.empty() && hitList_) {
        hitList_->setCurrentRow(0);
    }
}

void DocViewerDialog::setupUi() {
    auto* central = new QHBoxLayout(this);

    // ── 左：整篇文书正文 ──
    textView_ = new QPlainTextEdit(this);
    textView_->setObjectName(QStringLiteral("docViewerText"));
    textView_->setReadOnly(true);
    textView_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    if (fullText_.isEmpty()) {
        textView_->setPlainText(
            QStringLiteral("未找到该文书的全文原文（可能已被删除或索引已重建）。"));
    } else {
        textView_->setPlainText(fullText_);
    }
    central->addWidget(textView_, 3);

    // ── 右：命中块列表 + 状态 ──
    auto* rightCard = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightCard);

    statusLabel_ = new QLabel(rightCard);
    statusLabel_->setObjectName(QStringLiteral("docViewerStatus"));
    if (fullText_.isEmpty()) {
        statusLabel_->setText(QStringLiteral("⚠️ 全文不可用"));
    } else {
        statusLabel_->setText(
            QStringLiteral("命中 %1 处").arg(marks_.size()));
    }
    rightLayout->addWidget(statusLabel_);

    hitList_ = new QListWidget(rightCard);
    hitList_->setObjectName(QStringLiteral("docViewerHitList"));
    for (std::size_t i = 0; i < marks_.size(); ++i) {
        const doc_viewer::HitMark& mark = marks_[i];
        const QString preview =
            fullText_.mid(mark.charPos,
                          qMin(kPreviewChars, mark.charLen));
        auto* item = new QListWidgetItem(
            QStringLiteral("第 %1 处（块 %2）：%3…")
                .arg(i + 1)
                .arg(mark.chunkIndex)
                .arg(preview.trimmed()),
            hitList_);
        item->setData(Qt::UserRole, mark.chunkIndex);
    }
    rightLayout->addWidget(hitList_, 1);

    // connect 必须写在 hitList_ 构造之后（项目约定，nullptr 连接会静默失效）
    connect(hitList_, &QListWidget::itemSelectionChanged,
            this, &DocViewerDialog::onHitSelected);

    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), rightCard);
    closeBtn->setObjectName(QStringLiteral("docViewerClose"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    rightLayout->addWidget(closeBtn, 0, Qt::AlignRight);

    central->addWidget(rightCard, 1);
}

void DocViewerDialog::applyHighlights() {
    if (!textView_ || marks_.empty() || fullText_.isEmpty()) {
        return;
    }
    QList<QTextEdit::ExtraSelection> selections;
    for (const doc_viewer::HitMark& mark : marks_) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(textView_->document());
        selection.cursor.setPosition(mark.charPos);
        selection.cursor.setPosition(mark.charPos + mark.charLen,
                                     QTextCursor::KeepAnchor);
        selection.format.setBackground(kHighlightBackground);
        selections.append(selection);
    }
    textView_->setExtraSelections(selections);
}

void DocViewerDialog::onHitSelected() {
    if (!hitList_ || marks_.empty()) {
        return;
    }
    const int row = hitList_->currentRow();
    if (row < 0 || row >= static_cast<int>(marks_.size())) {
        return;
    }
    jumpToMark(marks_[static_cast<std::size_t>(row)]);
}

void DocViewerDialog::jumpToMark(const doc_viewer::HitMark& mark) {
    if (!textView_ || fullText_.isEmpty()) {
        return;
    }
    QTextCursor cursor = textView_->textCursor();
    cursor.setPosition(mark.charPos);
    textView_->setTextCursor(cursor);      // 光标落位
    textView_->centerCursor();             // 视口滚动到光标附近
}
