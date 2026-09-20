#pragma once
#include <QMainWindow>

#include <QFont>
#include <QString>

#include <memory>

class QLabel;
class QStackedWidget;

class NavigationBar;
class SearchPage;
class LibraryPage;
class HistoryPage;

namespace rag {
class Retriever;
}

namespace history {
struct HistoryRecord;
class HistoryStore;
}

/// 主窗口骨架：左侧导航 + 中央多页容器 + 底部状态栏。
///
/// T0 之后 MainWindow 不再持有任何业务逻辑，仅负责：
///   1. 把导航切换映射到 QStackedWidget；
///   2. 汇总各页上报的状态到底部 QStatusBar；
///   3. Ctrl + 滚轮调整全局界面字体缩放。
///
/// T1 起额外承担「引擎实例的唯一所有者」：
///   在 buildPages() 这一处集中创建 Retriever，再以裸指针注入各展示页。
///   这样多页共享同一份索引（导入后文档库立刻看得到），且页面仍是无状态插件
///   ——删掉任意页面只需删本文件的注册几行（解耦约定见开发工作计划第 8 条）。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 问答历史存储层（供 ui_smoke 构造历史记录做端到端断言；
    /// 生产代码里本窗口之外的调用者不存在，页面一律靠构造参数注入）
    history::HistoryStore* historyStore() const { return historyStore_.get(); }

protected:
    /// 全局 Ctrl+滚轮：调整界面字体缩放
    bool eventFilter(QObject* watched, QEvent* event) override;

    /// 关闭前把索引落盘，下次启动 loadIndex() 秒级恢复
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onNavIndexChanged(int index);
    void onEngineStatsChanged(int docCount, int chunkCount);
    void onApiKeyStateChanged(bool ready);
    /// 文档库发生增删/清空后，同步状态栏与检索页的缓存
    void onLibraryChanged();
    /// 检索问答页一个回合结束：落盘到问答历史库（T2）
    void onAnswerRecorded(const history::HistoryRecord& record);

private:
    void setupUi();
    void setupStatusBar();
    void buildPages();

    /// 启动时尝试从磁盘恢复索引，并把结果写进状态栏
    void restoreIndexOnStartup();

    /// 应用当前缩放比例（字体 + 全局 QSS）
    void applyUiScale();

    /// 按引擎当前真实状态刷新状态栏（文档数走 allDocIds，块数走 chunkCount）
    void refreshEngineStats();

    // ── 骨架三件套 ──
    NavigationBar* navBar_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;

    // ── 引擎实例（全应用唯一，由本窗口持有；页面只持裸指针借用）──
    std::unique_ptr<rag::Retriever> retriever_;

    // ── 问答历史存储实例（T2：同样集中在本窗口创建，历史页只借用）──
    std::unique_ptr<history::HistoryStore> historyStore_;

    // ── 业务页（占位页在 buildPages 里就地构造后交给 stack 托管）──
    SearchPage* searchPage_ = nullptr;
    LibraryPage* libraryPage_ = nullptr;
    HistoryPage* historyPage_ = nullptr;

    // ── 状态栏 ──
    QLabel* statusEngine_ = nullptr;
    QLabel* statusEmbedding_ = nullptr;
    QLabel* statusLlm_ = nullptr;
    QLabel* statusVersion_ = nullptr;

    // ── 界面缩放 ──
    int uiScale_ = 100;   // 缩放百分比（60–250）
    int docCount_ = 0;
    int chunkCount_ = 0;
    bool apiReady_ = false;
};
