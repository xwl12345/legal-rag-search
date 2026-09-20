#pragma once
#include <QSqlDatabase>
#include <QString>
#include <string>
#include <vector>

#include "history/history_record.h"

namespace history {

/// 问答历史的 SQLite 存储层（Qt SQL，零额外部署 —— SQLite 驱动随 Qt 分发）。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本层属于「核心数据链路」，不得 include 任何 ui/ 头、不得持有页面指针；
///   页面对它是**单向调用**（读列表 / 删记录 / 导出）， MainWindow 是唯一持有者。
///   删除问答历史页对存储层零影响；反过来删掉本文件，页面编译不过 —— 依赖方向单一。
///
/// 每个实例独占一个具名连接（connectionName 自增生成），析构时归还，
/// 便于单元测试里并存多个实例（分别指向不同的测试库）。
class HistoryStore {
public:
    /// @param dbPath 库文件路径；默认 config::HISTORY_DB（工作目录）
    explicit HistoryStore(std::string dbPath);
    HistoryStore();
    ~HistoryStore();

    HistoryStore(const HistoryStore&) = delete;
    HistoryStore& operator=(const HistoryStore&) = delete;

    /// 打开数据库并建表（幂等，可重复调用）。失败原因见 lastError()。
    bool open();
    bool isOpen() const { return opened_; }
    std::string lastError() const;

    /// 写入一条记录，成功返回自增 id（>0），失败返回 0
    long long append(const HistoryRecord& record);

    /// 按时间倒序取最近 limit 条（新在上）
    std::vector<HistoryRecord> recent(int limit = 200) const;

    /// 关键词检索：命中「问题」或「回答」即入选。
    /// %、_、\ 会被转义 —— 输入 "%" 不应该变成"匹配全部"。
    std::vector<HistoryRecord> search(const std::string& keyword, int limit = 200) const;

    /// 按主键取单条；不存在返回 false
    bool get(long long id, HistoryRecord& out) const;

    /// 删除单条；不存在返回 false（幂等）
    bool remove(long long id);

    /// 清空全部记录，返回被删除的行数
    int removeAll();

    /// 总记录数
    int count() const;

    const std::string& dbPath() const { return dbPath_; }

    // ── Markdown 导出（UTF-8）──

    /// 单条记录 → Markdown 文本
    static std::string toMarkdown(const HistoryRecord& record);

    /// 多条记录 → 一个 Markdown 文档（每条一个二级标题）
    static std::string toMarkdownAll(const std::vector<HistoryRecord>& records);

    /// 把 Markdown 写成 .md 文件（UTF-8 带 BOM，兼容 Windows 记事本）。
    /// 失败返回 false 并把原因写进 errorOut。
    static bool writeMarkdownFile(const QString& path, const std::string& markdown,
                                  QString* errorOut = nullptr);

private:
    std::string dbPath_;
    QString connectionName_;
    bool opened_ = false;
};

}  // namespace history
