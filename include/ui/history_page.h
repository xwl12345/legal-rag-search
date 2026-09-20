#pragma once
#include <QWidget>

#include <QStringList>
#include <vector>

#include "history/history_record.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

namespace history {
class HistoryStore;
}

/// 问答历史页（T2 / M2）。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本页是"插件"，只对 HistoryStore 做**单向只读/写入调用**（列表 / 删除 / 导出）；
///   HistoryStore 不感知本页存在，删除本页 = main_window.cpp 删注册几行 + 删本文件。
///   落库也不由本页负责 —— 检索问答页发出 answerFinished 信号，MainWindow 落到存储层，
///   本页只负责"呈现已经存在的记录"，页面之间零互相引用。
///
/// 职责：历史列表（时间/问题/命中块数/状态）、详情查看、关键词搜索、删除、Markdown 导出。
class HistoryPage : public QWidget {
    Q_OBJECT

public:
    /// @param store 存储层实例（非拥有；生命周期由 MainWindow 保证）
    explicit HistoryPage(history::HistoryStore* store, QWidget* parent = nullptr);
    ~HistoryPage() override;

    /// 当前列表行数（供自动化测试断言）
    int rowCount() const;

public slots:
    /// 重新拉取记录并刷新列表、统计与详情
    void refresh();

    /// 按主键删除单条（供自动化测试绕过弹窗直接调用）
    /// @return 是否真的删掉了
    bool removeRecordById(long long id, bool confirm = false);

    /// 按主键导出为 Markdown 文件（供自动化测试绕过文件对话框）
    /// @return 是否写成功；失败时弹窗提示（confirm=false 时不弹）
    bool exportRecordById(long long id, const QString& path, bool confirm = false);

private slots:
    void onKeywordChanged();
    void onExportSelected();
    void onRemoveSelected();
    void onClearAll();
    void onSelectionChanged();
    void onRowActivated(int row, int column);

private:
    void setupUi();
    /// 按当前关键词重建列表
    void rebuildTable();
    void updateSummary();
    /// 选中行的 id
    QStringList selectedIds() const;
    /// 把一条记录渲染到右侧详情区
    void showDetails(long long id);
    void clearDetails();
    /// 从表行取回 id（Qt::UserRole 存 id）
    long long idAtRow(int row) const;

    history::HistoryStore* store_ = nullptr;   // 非拥有

    QLineEdit* keywordInput_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* hint_ = nullptr;
    QPushButton* exportBtn_ = nullptr;
    QPushButton* removeBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;

    QTextEdit* detailArea_ = nullptr;
    QLabel* detailTitle_ = nullptr;
};
