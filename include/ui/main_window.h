#pragma once
#include <QMainWindow>

#include <QFont>
#include <QString>

#include <memory>

#include "config/app_settings.h"
#include "log/run_log.h"
#include "rag/synonym.h"

class QLabel;
class QStackedWidget;
class QThread;

class NavigationBar;
class EngineWorker;
class SearchPage;
class LibraryPage;
class HistoryPage;
class QualityPage;
class LawPage;
class FavoritesPage;
class SettingsPage;
class StatusPage;

namespace rag {
class Retriever;
}

namespace history {
struct HistoryRecord;
class HistoryStore;
}

namespace law {
class LawStore;
}

namespace favorite {
class FavoriteStore;
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

    /// 法条库（供 ui_smoke 做法条 E2E 断言；同 historyStore() 的注入约定）
    law::LawStore* lawStore() const { return lawStore_.get(); }

    /// 案例收藏库（供 ui_smoke 做收藏 E2E 断言；同 historyStore() 的注入约定）
    favorite::FavoriteStore* favoriteStore() const { return favoriteStore_.get(); }

    /// 引擎工作线程宿主（供 ui_smoke 等待引擎空闲；生产代码不经此直调重操作）
    EngineWorker* engineWorker() const { return engineWorker_; }

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
    /// T11：检索问答页写入一条收藏 → 刷新案例收藏页（两页经本窗口中转）
    void onFavoriteChanged();
    /// 检索问答页一个回合结束：落盘到问答历史库（T2）
    void onAnswerRecorded(const history::HistoryRecord& record);
    /// 设置页保存并生效（T3）：写盘已由页面完成，这里转发给引擎与各页
    void onSettingsChanged(const config::AppSettings& settings);
    /// 同义词表保存成功（T7）：队列化转发引擎热更新（写盘已由页面完成）
    void onSynonymsChanged(rag::synonym::SynonymMap map);
    /// T13：请求切换知识库（relPath 相对 exe 目录，空串 = 默认主库）。
    /// 队列化到引擎线程与检索/导入串行；成功与否经 onIndexSwitched 回调。
    void switchKb(const QString& relPath);
    /// T13：引擎切换完成回调——成功则持久化记忆并联动刷新，失败回滚提示
    void onIndexSwitched(int docCount, int chunkCount, qint64 loadMs,
                         const QString& path);
    /// T13：另存当前库完成回调（写盘在引擎线程，结果经信号回传）
    void onIndexSaved(bool ok, const QString& path, const QString& diagnostic);
    /// T13：文档库页请求另存（relPath 相对路径）→ 队列化引擎线程写盘
    void onKbSaveAsRequested(const QString& relPath);
    /// T13：文档库页请求删库——主库与当前占用库在此拒绝，其余删文件
    void onKbDeleteRequested(const QString& relPath);

private:
    void setupUi();
    void setupStatusBar();
    void buildPages();

    /// ── P0-2 跨页忙碌互斥（中枢）──
    /// 任一页面报告引擎任务开始/结束后，把忙碌状态回灌到全部动作页面
    /// （含发起页，保持其按钮禁用一致），并记录到 engineBusy_ 供 closeEvent 判断。
    void forwardEngineBusy(bool busy);
    /// 是否有页面正在执行引擎任务（检索/生成/导入/删除/清空/对比/评测）
    bool engineBusy() const;

    /// 启动时加载配置文件并把参数下发到引擎（缺文件/损坏回落默认值）
    void loadSettingsAndApply();
    /// 把 appSettings_ 应用到引擎热更新 setter（供启动与设置变更共用）
    void applySettingsToEngine();
    /// 按引擎真实状态刷新状态栏两行服务标签（Embedding 行读 retriever，
    /// LLM 行读 apiReady_ —— 两条服务的 Key 来源不同，不能共用一个 ready）
    void refreshServiceStatus();
    /// 状态圆点着色（P0-8）：至少一路 AI 服务就绪 = 绿，否则灰（QSS statusDotOff）
    void setStatusDot(bool ok);

    /// ── T8 运行状态页 ──
    /// 操作流水统一入口：写 runLog_ 并同步到状态页（UI 线程独占，无锁）
    void appendRunLog(const QString& level, const QString& message);
    /// 组合根收集统计与服务状态 → 注入状态页（打开页 / 统计变化时调用）
    void refreshStatusPage();
    /// 状态页请求清空日志（clearLogRequested 信号中转）
    void onClearRunLog();

    /// 启动时尝试从磁盘恢复索引，并把结果写进状态栏
    void restoreIndexOnStartup();

    /// 应用当前缩放比例（字体 + 全局 QSS）
    void applyUiScale();

    /// 按引擎当前真实状态刷新状态栏（文档数走 allDocIds，块数走 chunkCount）
    void refreshEngineStats();

    // ── 骨架三件套 ──
    NavigationBar* navBar_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;

    // ── 引擎实例（P1：由 EngineWorker 持有并在引擎线程上执行重操作；
    // retriever_ 是其轻只读指针，供状态栏与页面注入）──
    rag::Retriever* retriever_ = nullptr;
    EngineWorker* engineWorker_ = nullptr;   // 引擎线程宿主（无 parent，跨线程）
    QThread* engineThread_ = nullptr;

    // ── 问答历史存储实例（T2：同样集中在本窗口创建，历史页只借用）──
    std::unique_ptr<history::HistoryStore> historyStore_;

    // ── 法条速查（T9）：法条库 + 独立法条检索实例，均集中在本窗口创建。
    //    独立 Retriever 与主引擎（retriever_）物理隔离：法条语料不进主
    //    索引，T4/T6 指标基线零漂移由构造保证；语料小（数百条）启动时
    //    addText 毫秒级，无需持久化也无需引擎线程。
    std::unique_ptr<law::LawStore> lawStore_;
    std::unique_ptr<rag::Retriever> lawRetriever_;

    // ── 案例收藏（T11）：收藏库同样集中在本窗口创建，收藏页与检索页只借用指针 ──
    std::unique_ptr<favorite::FavoriteStore> favoriteStore_;

    // ── 业务页（占位页在 buildPages 里就地构造后交给 stack 托管）──
    SearchPage* searchPage_ = nullptr;
    LibraryPage* libraryPage_ = nullptr;
    HistoryPage* historyPage_ = nullptr;
    QualityPage* qualityPage_ = nullptr;
    FavoritesPage* favoritesPage_ = nullptr;
    LawPage* lawPage_ = nullptr;
    SettingsPage* settingsPage_ = nullptr;
    StatusPage* statusPage_ = nullptr;
    runlog::RunLog runLog_;   // T8：操作流水（UI 线程独占）
    QString currentKbRel_;    // T13：当前知识库相对路径，空 = 默认主库
    QString pendingKbRel_;    // T13：切换意图（发起时记录，回调成功后落为 currentKbRel_）

    // ── 检索参数配置（T3：启动时从 rag_settings.json 加载，设置页保存后更新）──
    config::AppSettings appSettings_;

    // ── 状态栏 ──
    QLabel* statusDot_ = nullptr;
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
