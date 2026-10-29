#pragma once
#include <QWidget>
#include <QVector>

#include "log/run_log.h"

class QLabel;
class QPushButton;
class QPlainTextEdit;

/// 运行状态页（T8 状态日志面板）：服务状态 / 统计 / 操作流水 三分区。
///
/// 解耦约定（同 T3 设置页）：本页**不持有、不调用 Retriever**——
/// 统计与服务状态由 MainWindow 组合根收集后经 refreshStats 注入；
/// 操作流水由 MainWindow 在各信号点 append 到其持有的 runlog::RunLog，
/// 再经 refreshLog 把快照注入本页展示。删掉本页 = 去掉注册几行，
/// 检索功能零影响。
class StatusPage : public QWidget {
    Q_OBJECT

public:
    explicit StatusPage(QWidget* parent = nullptr);

    /// 统计与服务状态快照（MainWindow 收集，字段全部为现成读数）
    struct Stats {
        int documents = 0;
        int chunks = 0;
        int historyCount = 0;
        int favoriteCount = 0;
        long long indexLoadMs = -1;     // -1 = 本次启动未从落盘恢复
        QString configLine;             // 配置快照一行（k1/b/权重/TopK）
        QString embeddingStatus;        // 同状态栏口径
        QString llmStatus;
    };

    /// 整页刷新（打开页时 / 引擎统计变化时由 MainWindow 调用）
    void refreshStats(const Stats& stats);

    /// 全量重填日志列表（MainWindow 传入 RunLog 快照，正序旧在前）
    void refreshLog(const QVector<runlog::RunLogEntry>& entries);

signals:
    /// 用户点击「清空日志」；RunLog 实例在 MainWindow，经此信号中转
    void clearLogRequested();

private slots:
    void onExportLog();

private:
    void setupUi();
    /// 快照导出文本（与 refreshLog 展示同格式："[HH:mm:ss] [级别] 内容"）
    QString toPlainTextOfLog() const;

    // ── 服务状态卡 ──
    QLabel* embeddingLabel_ = nullptr;
    QLabel* llmLabel_ = nullptr;

    // ── 统计卡 ──
    QLabel* statsLine_ = nullptr;      // 文档/块/历史/收藏 一行
    QLabel* loadLine_ = nullptr;       // 索引恢复耗时
    QLabel* configLine_ = nullptr;     // 配置快照

    // ── 日志卡 ──
    QPlainTextEdit* logView_ = nullptr;
    QPushButton* exportBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QVector<runlog::RunLogEntry> logEntries_;   // 最近一次快照（导出用）
};
