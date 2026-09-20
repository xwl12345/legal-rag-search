#pragma once
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <memory>
#include <vector>
#include "history/history_record.h"
#include "rag/retriever.h"
#include "rag/generator.h"

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
    /// @param retriever 引擎实例（非拥有；为空时本页自建，仅供独立测试）
    explicit SearchPage(rag::Retriever* retriever = nullptr, QWidget* parent = nullptr);
    ~SearchPage() override;

    /// 当前缓存（最近一次检索）的结果条数，供自动化测试断言
    int lastResultCount() const { return static_cast<int>(cachedResults_.size()); }

public slots:
    /// 导入给定路径的文档。与「导入文档」按钮走同一条代码路径，
    /// 区别仅在于文件由调用方给出（按钮内部弹 QFileDialog）。
    void importPaths(const QStringList& files);

    /// 引擎内的文档集合被外部改动（如文档库页删除）后调用：
    /// 清掉已失效的检索缓存与筛选器，避免展示已被删除文档的片段。
    void invalidateIndexCache();

    /// ⚠️ 测试钩子（仅供 ui_smoke 的问答历史 E2E 使用，生产逻辑不会调用）。
    ///
    /// 无 API Key 的环境下无法产生真实回答，但"回合结束 → 落库 → 历史页出现记录"
    /// 这条链路必须能被自动验证。本钩子把一段给定的回答文本按正常流程走完：
    /// 界面显示 → 累计进 answerBuffer_ → finishAnswerRound() → answerFinished()，
    /// 来源则取当前一轮**真实检索**命中结果（调用方需先检索）。
    /// 换言之，假的只是"回答从哪来"，其余全是生产代码路径。
    void simulateAnswer(const QString& query, const QString& answer, bool interrupted = false);

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

    // ── 核心引擎（retriever_ 非拥有；仅独立测试时才由本页自持）──
    rag::Retriever* retriever_ = nullptr;
    std::unique_ptr<rag::Retriever> ownedRetriever_;
    std::unique_ptr<rag::Generator> generator_;

    // ── UI 组件 ──
    QLineEdit* searchInput_ = nullptr;
    QPushButton* searchBtn_ = nullptr;
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

    // ── 缓存当前搜索结果（用于筛选）──
    std::vector<rag::SearchResult> cachedResults_;
    QString currentQuery_;
};
