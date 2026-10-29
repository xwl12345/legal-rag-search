#pragma once
#include <QString>
#include <QVector>
#include <deque>

namespace runlog {

/// 单条运行流水（T8 状态日志面板）
struct RunLogEntry {
    QString timestamp;   // 人类可读时间（HH:mm:ss）
    QString level;       // "信息" / "警告"
    QString message;
};

/// 操作流水环形缓冲（T8）。
/// 设计约定：
///   - 只在 UI 线程 append（全部来源都是 MainWindow 中转的信号槽），无锁；
///   - 容量上限 maxEntries，超限丢弃最旧条目（环形语义）；
///   - entries() 返回**按时间正序**的快照（旧在前，列表展示/导出直接用）。
/// 纯数据类，不含 UI——run_tests 可独立单测（阶段 2 规则：优先纯函数核心层）。
class RunLog {
public:
    /// 上限（环形截断阈值）；超出后保留最新 maxEntries 条
    static constexpr int kMaxEntries = 500;

    /// 追加一条流水（时间戳取当前时刻，level/message 原样保存）
    void append(const QString& level, const QString& message);

    /// 快照：按时间正序（旧在前）
    QVector<RunLogEntry> entries() const;

    /// 当前条数（≤ kMaxEntries）
    int count() const;

    /// 清空
    void clear();

    /// 导出为纯文本（每行 "[HH:mm:ss] [级别] 内容"）；空日志返回空串
    QString toPlainText() const;

private:
    std::deque<RunLogEntry> entries_;   // 正序存放（旧在前，新 push_back）
};

} // namespace runlog
