#include "ui/status_page.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include "ui/app_theme.h"

StatusPage::StatusPage(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void StatusPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(10);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    auto* titleLabel = new QLabel(QStringLiteral("运行状态"), this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(
        QStringLiteral("服务状态 · 运行统计 · 操作流水（最近 500 条）"), this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(titleLabel);
    head->addWidget(subtitleLabel);
    head->addStretch();
    root->addLayout(head);

    // ── 卡片一：服务状态 ──
    auto* statusCard = new QFrame(this);
    statusCard->setObjectName(QStringLiteral("card"));
    auto* statusLayout = new QVBoxLayout(statusCard);
    statusLayout->setContentsMargins(16, 12, 16, 12);
    auto* statusTitle = new QLabel(QStringLiteral("服务状态"), statusCard);
    statusTitle->setObjectName(QStringLiteral("cardTitle"));
    statusLayout->addWidget(statusTitle);
    embeddingLabel_ = new QLabel(statusCard);
    embeddingLabel_->setObjectName(QStringLiteral("statusEmbedding"));
    llmLabel_ = new QLabel(statusCard);
    llmLabel_->setObjectName(QStringLiteral("statusLlm"));
    statusLayout->addWidget(embeddingLabel_);
    statusLayout->addWidget(llmLabel_);
    root->addWidget(statusCard);

    // ── 卡片二：运行统计 ──
    auto* statsCard = new QFrame(this);
    statsCard->setObjectName(QStringLiteral("card"));
    auto* statsLayout = new QVBoxLayout(statsCard);
    statsLayout->setContentsMargins(16, 12, 16, 12);
    auto* statsTitle = new QLabel(QStringLiteral("运行统计"), statsCard);
    statsTitle->setObjectName(QStringLiteral("cardTitle"));
    statsLayout->addWidget(statsTitle);
    statsLine_ = new QLabel(statsCard);
    statsLine_->setObjectName(QStringLiteral("statusStatsLine"));
    loadLine_ = new QLabel(statsCard);
    configLine_ = new QLabel(statsCard);
    configLine_->setWordWrap(true);
    statsLayout->addWidget(statsLine_);
    statsLayout->addWidget(loadLine_);
    statsLayout->addWidget(configLine_);
    root->addWidget(statsCard);

    // ── 卡片三：操作流水 ──
    auto* logCard = new QFrame(this);
    logCard->setObjectName(QStringLiteral("card"));
    auto* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(16, 12, 16, 12);
    auto* logTitle = new QLabel(QStringLiteral("操作流水"), logCard);
    logTitle->setObjectName(QStringLiteral("cardTitle"));
    logLayout->addWidget(logTitle);

    logView_ = new QPlainTextEdit(logCard);
    logView_->setObjectName(QStringLiteral("statusLogView"));
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(260);
    logLayout->addWidget(logView_);

    auto* actions = new QHBoxLayout();
    exportBtn_ = new QPushButton(QStringLiteral("导出日志 (.txt)"), this);
    exportBtn_->setObjectName(QStringLiteral("statusExportBtn"));
    clearBtn_ = new QPushButton(QStringLiteral("清空日志"), this);
    clearBtn_->setObjectName(QStringLiteral("statusClearBtn"));
    actions->addWidget(exportBtn_);
    actions->addWidget(clearBtn_);
    actions->addStretch();
    logLayout->addLayout(actions);
    root->addWidget(logCard, 1);

    connect(exportBtn_, &QPushButton::clicked,
            this, &StatusPage::onExportLog);
    connect(clearBtn_, &QPushButton::clicked,
            this, &StatusPage::clearLogRequested);
}

void StatusPage::refreshStats(const Stats& stats) {
    embeddingLabel_->setText(stats.embeddingStatus);
    llmLabel_->setText(stats.llmStatus);

    statsLine_->setText(QStringLiteral("文档库：%1 篇文书 / %2 个文本块　　"
                                       "问答历史：%3 条　　案例收藏：%4 条")
                            .arg(stats.documents)
                            .arg(stats.chunks)
                            .arg(stats.historyCount)
                            .arg(stats.favoriteCount));

    loadLine_->setText(stats.indexLoadMs >= 0
        ? QStringLiteral("本次启动索引恢复耗时：%1 ms").arg(stats.indexLoadMs)
        : QStringLiteral("本次启动未从落盘恢复索引"));

    configLine_->setText(QStringLiteral("当前配置：%1").arg(stats.configLine));
}

void StatusPage::refreshLog(const QVector<runlog::RunLogEntry>& entries) {
    logEntries_ = entries;
    QString text;
    for (const auto& entry : entries) {
        text += QStringLiteral("[%1] [%2] %3\n")
                    .arg(entry.timestamp, entry.level, entry.message);
    }
    logView_->setPlainText(text);
    // 贴底跟随：最新事件永远可见
    QScrollBar* bar = logView_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void StatusPage::onExportLog() {
    const QString defaultName = QStringLiteral("运行日志_%1.txt")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出运行日志"), defaultName,
        QStringLiteral("文本文件 (*.txt)"));
    if (path.isEmpty()) {
        return;   // 用户取消，不视为错误
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;   // 页面层不打断流程；导出失败如实静默（快照仍在界面上）
    }
    file.write(toPlainTextOfLog().toUtf8());
    file.close();
}

QString StatusPage::toPlainTextOfLog() const {
    QString out;
    for (const auto& entry : logEntries_) {
        out += QStringLiteral("[%1] [%2] %3\n")
                   .arg(entry.timestamp, entry.level, entry.message);
    }
    return out;
}
