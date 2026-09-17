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
#include "rag/retriever.h"
#include "rag/generator.h"

/// 检索问答页：保留重构前 MainWindow 的全部检索逻辑
/// （导入 → 混合检索 → 元数据筛选 → SSE 流式回答），
/// MainWindow 回收为纯骨架后由本页承载。
class SearchPage : public QWidget {
    Q_OBJECT

public:
    explicit SearchPage(QWidget* parent = nullptr);
    ~SearchPage() override;

    /// 当前缓存（最近一次检索）的结果条数，供自动化测试断言
    int lastResultCount() const { return static_cast<int>(cachedResults_.size()); }

public slots:
    /// 导入给定路径的文档。与「导入文档」按钮走同一条代码路径，
    /// 区别仅在于文件由调用方给出（按钮内部弹 QFileDialog）。
    void importPaths(const QStringList& files);

signals:
    /// 索引规模变化（文档数 / 文本块数），供主窗口状态栏显示
    void engineStatsChanged(int docCount, int chunkCount);

    /// 生成服务可用性变化（是否配置了 API Key）
    void apiKeyStateChanged(bool ready);

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

    /// 从结果中收集可用年份
    void populateYearFilter(const std::vector<rag::SearchResult>& results);

    /// 构建元数据摘要（供 AI prompt 使用）
    std::string buildMetadataSummary(const std::vector<rag::SearchResult>& results);

    /// 向主窗口广播当前索引规模
    void emitEngineStats();

    // ── 核心引擎 ──
    std::unique_ptr<rag::Retriever> retriever_;
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

    // ── 筛选控件 ──
    QComboBox* caseTypeFilter_ = nullptr;
    QComboBox* courtLevelFilter_ = nullptr;
    QComboBox* yearFilter_ = nullptr;

    // ── 缓存当前搜索结果（用于筛选）──
    std::vector<rag::SearchResult> cachedResults_;
    QString currentQuery_;
};
