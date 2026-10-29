#include "log/run_log.h"

#include <QDateTime>

namespace runlog {

void RunLog::append(const QString& level, const QString& message) {
    RunLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("HH:mm:ss"));
    entry.level = level.isEmpty() ? QStringLiteral("信息") : level;
    entry.message = message;
    entries_.push_back(std::move(entry));
    while (static_cast<int>(entries_.size()) > kMaxEntries) {
        entries_.pop_front();   // 环形：丢最旧
    }
}

QVector<RunLogEntry> RunLog::entries() const {
    return QVector<RunLogEntry>(entries_.cbegin(), entries_.cend());
}

int RunLog::count() const {
    return static_cast<int>(entries_.size());
}

void RunLog::clear() {
    entries_.clear();
}

QString RunLog::toPlainText() const {
    QString out;
    for (const auto& entry : entries_) {
        out += QStringLiteral("[%1] [%2] %3\n")
                   .arg(entry.timestamp, entry.level, entry.message);
    }
    return out;
}

} // namespace runlog
