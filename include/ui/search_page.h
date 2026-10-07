#pragma once
#include <QWidget>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "history/history_record.h"
#include "rag/retriever.h"
#include "ui/engine_worker.h"
#include "config/app_settings.h"

class QProgressDialog;

/// 检索问答页：保留重构前 MainWindow 的全部检索逻辑
/// （导入 → 混合检索 → 元数据筛选 → SSE 流式回答），
/// MainWindow 回收为纯骨架后由本页承载。
///
/// 解耦约定（开发工作计划·全局约束第 8 条）：
///   本页是"插件"，只对 Retriever 做单向调用；Retriever 不感知本页存在。
///   引擎实例由 MainWindow 集中创建并注入，本页**不拥有**它——
///   这样文档库页与检索页看到的是同一份索引，且删除任意页面都不影响核心。
class SearchPage : public QWidget {
    Q_OBJECT

public:
    /// @param retriever 引擎实例（非拥有；P2 起必须注入，自建分支已删除——
    ///                  组合根唯一，独立测试场景由测试方自持实例注入）
    explicit SearchPage(rag::Retriever* retriever, QWidget* parent = nullptr);
    ~SearchPage() override;

    /// 当前缓存（最近一次检索）的结果条数，供自动化测试断言
    int lastResultCount() const { return static_cast<int>(cachedResults_.size()); }

    /// 本页是否有引擎任务进行中（检索+生成 / 批量导入），供 closeEvent 与 E2E 断言
    bool isBusy() const { return busySelf_; }

    /// 当前 LLM API Key（MainWindow 启动接线后推送给 EngineWorker 的生成器用）
    QString llmApiKey() const { return llmKey_; }

    /// ── P0-7 同名异路径覆盖确认钩子 ──
    /// 返回值决定对新文件的处理；默认实现弹 QMessageBox，测试可注入脚本化应答。
    enum class OverwriteChoice { Overwrite, Skip, CancelAll };
    using ConfirmOverwrite = std::function<OverwriteChoice(
        const QString& docId, const QString& existingPath, const QString& newPath)>;
    void setConfirmOverwriteHandler(ConfirmOverwrite handler) {
        confirmOverwrite_ = std::move(handler);
    }

public slots:
    /// 导入给定路径的文档。与「导入文档」按钮走同一条代码路径，
    /// 区别仅在于文件由调用方给出（按钮内部弹 QFileDialog）。
    void importPaths(const QStringList& files);

    /// 引擎内的文档集合被外部改动（如文档库页删除）后调用：
    /// 清掉已失效的检索缓存与筛选器，避免展示已被删除文档的片段。
    void invalidateIndexCache();

    /// 其他页面有引擎任务进行中时，禁用本页全部引擎动作按钮（P0-2 跨页互斥，
    /// 由 MainWindow 中转——页面之间不互相引用）
    void setExternalBusy(bool busy) {
        if (busyExternal_ != busy) {
            busyExternal_ = busy;
            refreshActionButtons();
        }
    }

    // ── P1：引擎结果回传（EngineWorker 信号 → 队列化到 UI 线程；
    //        MainWindow 负责接线，故为 public 槽）──
    void onSearchFinished(const std::vector<rag::SearchResult>& results,
                          const QString& query);
    void onGenerationDelta(const QString& text);
    void onGenerationFinished(bool interrupted, const QString& errorText);
    void onImportProgress(int done, int total, const QString& fileName);
    void onImportOcrPage(int page, int total);
    void onImportFinished(const ui_engine::ImportSummary& summary);

signals:
    /// 索引规模变化（文档数 / 文本块数），供主窗口状态栏显示
    void engineStatsChanged(int docCount, int chunkCount);

    /// 生成服务可用性变化（是否配置了 API Key）
    void apiKeyStateChanged(bool ready);

    /// 一个问答回合结束，携带完整记录交由上层持久化（T2）。
    ///
    /// 本页**自己不落库**：历史怎么存由 MainWindow 决定，符合"页面不互相引用"——
    /// 检索页不知道历史页是否存在，历史页也不知道回答从哪来。
    /// 落库规则见开发工作计划 T2：有内容就存（含中断的残卷，标 interrupted），
    /// 一个字都没吐出来的回合不入库。
    void answerFinished(const history::HistoryRecord& record);

    /// 引擎任务开始/结束（P0-2）：MainWindow 据此让其他页面禁用引擎动作，
    /// 关闭窗口时据此拦截「任务进行中就退出」
    void engineBusyChanged(bool busy);

    // ── P1 异步引擎请求（MainWindow 接线到 EngineWorker 的队列化槽）──
    /// 请求检索（普通检索与聚合宽检索共用，EngineWorker::search）
    void searchRequested(const QString& query, int topK);
    /// 请求聚合检索（宽检索 + per-doc 去重在引擎内完成，EngineWorker::searchAggregate）
    void aggregateSearchRequested(const QString& query, int width);
    /// 请求生成回答（EngineWorker::generateAnswer，流式增量经 generationDelta 回来）
    void generationRequested(const QString& query, const QString& context,
                             const QString& metaContext, double temperature);
    /// 请求中断当前生成（EngineWorker::cancelGeneration）
    void generationCancelRequested();
    /// LLM Key 变更（EngineWorker::setLlmApiKey；初始 Key 由 MainWindow 启动时推送）
    void llmApiKeyChanged(const QString& key);
    /// 请求批量导入（同名冲突预检已完成，EngineWorker::importDocuments）
    void importRequested(const QStringList& files);

private slots:
    void onSearch();
    void onPickImportFiles();
    void onClearIndex();
    void onSetApiKey();
    void onFilterChanged();

private:
    void setupUi();
    void displayResults(const std::vector<rag::SearchResult>& results);
    void appendAiAnswer(const QString& text);
    void loadApiKey();

    /// 检索完成后的生成阶段入口：构建上下文与元数据摘要并入队生成请求。
    /// sources 作为本回合历史落库的命中来源缓存。
    void startGeneration(std::vector<rag::SearchResult> sources,
                         const std::string& metaSummary);

    /// 生成/检索链结束后的公共收尾（进度条、忙碌解除、状态栏）
    void finishSearchRound();

    /// ── P0-2 忙碌状态机 ──
    /// 进入/退出引擎任务；busyChanged 广播后由 MainWindow 中转给其他页面。
    void beginEngineTask() {
        busySelf_ = true;
        refreshActionButtons();
        emit engineBusyChanged(true);
    }
    void endEngineTask() {
        busySelf_ = false;
        refreshActionButtons();
        emit engineBusyChanged(false);
    }
    /// 按当前忙碌状态刷新本页动作按钮（自忙或他页忙时一律禁用）
    void refreshActionButtons();

    /// P0-7 默认覆盖确认弹窗（可在测试中注入脚本化替换）
    OverwriteChoice defaultConfirmOverwrite(const QString& docId,
                                            const QString& existingPath,
                                            const QString& newPath) const;

public slots:
    /// 应用 T3 设置页下发的配置：检索条数与生成温度（由 MainWindow 中转，
    /// 本页不知道设置页存在）
    void applySettings(const config::AppSettings& settings);

private:
    /// 校验 API Key 格式：sk- 开头，长度 ≥ 20
    static bool validateApiKeyFormat(const QString& key, QString* errorMsg = nullptr);

    /// 更新 API Key 状态指示（配色由 QSS 的 status 属性决定）
    void updateApiKeyStatus(bool valid, const QString& message);

    /// 应用筛选条件，过滤并重新显示结果
    std::vector<rag::SearchResult> applyFiltersAndDisplay();
    std::vector<rag::SearchResult> getFilteredResults();

    /// 从全部已导入文档收集可用年份
    void populateYearFilter(const std::vector<rag::SearchResult>& results);

    /// 构建元数据摘要（供 AI prompt 使用）
    std::string buildMetadataSummary(const std::vector<rag::SearchResult>& results);

    /// 向主窗口广播当前索引规模
    void emitEngineStats();

    /// 把本次回合组装成一条历史记录（问题 / 回答 / 命中来源 / 是否中断）
    history::HistoryRecord buildHistoryRecord(
        const QString& query,
        const std::vector<rag::SearchResult>& sources,
        const QString& answer,
        bool interrupted,
        const QString& note) const;

    /// 回答流收尾的统一出口：决定是否落库并发出 answerFinished
    void finishAnswerRound(const QString& query,
                           const std::vector<rag::SearchResult>& sources);

    // ── 本回合的回答累计状态（流式增量 + 失败提示都算"用户看到的内容"）──
    QString answerBuffer_;
    bool answerInterrupted_ = false;
    QString answerNote_;

    // ── T3 配置中心下发的检索参数 ──
    // 检索宽度默认 20 / 聚合宽检索 50，与历史行为一致；设置页改 TopK 后经
    // MainWindow 转发到这里热更新（宽检索 = 基准宽度的 2.5 倍，同比例联动）。
    int searchWidth_ = 20;
    int wideSearchWidth_ = 50;
    double temperature_ = 0.3;   // 生成温度（随生成请求下发给引擎线程的 Generator）

    // ── 核心引擎（retriever_ 非拥有，P2 起构造必须注入）──
    // P1 起 Generator 归 EngineWorker 所有，本页经信号请求生成。
    rag::Retriever* retriever_ = nullptr;

    // ── UI 组件 ──
    QLineEdit* searchInput_ = nullptr;
    QPushButton* searchBtn_ = nullptr;
    QPushButton* stopGenBtn_ = nullptr;   // P1：生成中显示，点击请求中断
    QPushButton* importBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;

    // API Key 输入
    QLineEdit* apiKeyInput_ = nullptr;
    QPushButton* setApiKeyBtn_ = nullptr;
    QLabel* apiKeyStatus_ = nullptr;

    QListWidget* resultList_ = nullptr;
    QTextEdit* aiAnswerArea_ = nullptr;

    QLabel* statusLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;

    // ── 筛选控件（四维：案件类型 / 法院级别 / 年份 / 结果倾向）──
    QComboBox* caseTypeFilter_ = nullptr;
    QComboBox* courtLevelFilter_ = nullptr;
    QComboBox* yearFilter_ = nullptr;
    QComboBox* tendencyFilter_ = nullptr;
    QCheckBox* courtOnlyFilter_ = nullptr;   // T5：只看本院认为（角色 = 法院认定）

    // ── 缓存当前搜索结果（用于筛选）──
    std::vector<rag::SearchResult> cachedResults_;
    QString currentQuery_;

    // ── P1 异步检索/生成链状态 ──
    enum class SearchStage { Idle, MainSearch, WideSearch, Generating };
    SearchStage stage_ = SearchStage::Idle;
    std::vector<rag::SearchResult> pendingSources_;   // 本回合生成/落库的命中来源
    QString llmKey_;                                  // LLM Key（生成在引擎线程）
    std::unique_ptr<QProgressDialog> importProgress_; // 异步导入期间的进度对话框
    std::shared_ptr<std::atomic_bool> importCancel_;  // 导入取消令牌（worker 轮询）
    int importSkipped_ = 0;                           // 本轮同名跳过数（UI 侧预检产生）

    // ── P0-2 忙碌状态：self = 本页任务进行中；external = 其他页面任务进行中 ──
    bool busySelf_ = false;
    bool busyExternal_ = false;

    // ── P0-7 同名覆盖确认钩子（空 = 默认弹窗；ui_smoke 注入脚本化应答）──
    ConfirmOverwrite confirmOverwrite_;
};
