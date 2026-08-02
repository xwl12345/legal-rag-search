#pragma once
#include <QMainWindow>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QSplitter>
#include <QVBoxLayout>
#include <QComboBox>
#include <memory>
#include "rag/retriever.h"
#include "rag/generator.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSearch();
    void onImportFiles();
    void onClearIndex();
    void onSetApiKey();
    void onFilterChanged();

private:
    void setupUi();
    void setupStyle();
    void displayResults(const std::vector<rag::SearchResult>& results);
    void appendAiAnswer(const QString& text);
    void loadApiKey();

    /// 校验 API Key 格式：sk- 开头，长度 ≥ 20
    static bool validateApiKeyFormat(const QString& key, QString* errorMsg = nullptr);

    /// 更新 API Key 状态指示（图标 + 文字）
    void updateApiKeyStatus(bool valid, const QString& message);

    /// 应用筛选条件，过滤并重新显示结果
    std::vector<rag::SearchResult> applyFiltersAndDisplay();
    std::vector<rag::SearchResult> getFilteredResults();

    /// 从结果中收集可用年份
    void populateYearFilter(const std::vector<rag::SearchResult>& results);

    /// 构建元数据摘要（供 AI prompt 使用）
    std::string buildMetadataSummary(const std::vector<rag::SearchResult>& results);

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
