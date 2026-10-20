#include "ui/snippet_dialog.h"

#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

SnippetDialog::SnippetDialog(const QString& docId, int chunkIndex,
                             const QString& chunkText, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("片段 · %1（块 %2）").arg(docId).arg(chunkIndex));
    setObjectName(QStringLiteral("snippetDialog"));
    setModal(false);  // 非模态：片段与 AI 回答同屏对照
    resize(560, 420);

    auto* root = new QVBoxLayout(this);

    // ── 块原文（只读）──
    textView_ = new QPlainTextEdit(this);
    textView_->setObjectName(QStringLiteral("snippetText"));
    textView_->setReadOnly(true);
    textView_->setPlainText(chunkText);
    root->addWidget(textView_, 1);

    // ── 动作行：收藏 / 全文 / 关闭 ──
    auto* btnRow = new QHBoxLayout();
    auto* favBtn = new QPushButton(QStringLiteral("⭐ 收藏到案例集…"), this);
    favBtn->setObjectName(QStringLiteral("snippetFavoriteBtn"));
    auto* fullBtn = new QPushButton(QStringLiteral("📖 查看全文"), this);
    fullBtn->setObjectName(QStringLiteral("snippetFullTextBtn"));
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    closeBtn->setObjectName(QStringLiteral("snippetCloseBtn"));
    btnRow->addWidget(favBtn);
    btnRow->addWidget(fullBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    // connect 写在按钮构造之后（项目约定，nullptr 连接会静默失效）；
    // 收藏/全文只发信号——本类不持有 store/retriever，动作由 SearchPage 执行
    connect(favBtn, &QPushButton::clicked,
            this, &SnippetDialog::favoriteRequested);
    connect(fullBtn, &QPushButton::clicked,
            this, &SnippetDialog::fullTextRequested);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

QString SnippetDialog::snippetText() const {
    return textView_ ? textView_->toPlainText() : QString();
}
