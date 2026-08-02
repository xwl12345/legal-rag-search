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

private:
    void setupUi();
    void setupStyle();
    void displayResults(const std::vector<rag::SearchResult>& results);
    void appendAiAnswer(const QString& text);
    void loadApiKey();

    // ── 核心引擎 ──
    std::unique_ptr<rag::Retriever> retriever_;
    std::unique_ptr<rag::Generator> generator_;

    // ── UI 组件 ──
    QLineEdit* searchInput_ = nullptr;
    QPushButton* searchBtn_ = nullptr;
    QPushButton* importBtn_ = nullptr;
    QPushButton* clearBtn_ = nullptr;

    QListWidget* resultList_ = nullptr;

    QTextEdit* aiAnswerArea_ = nullptr;

    QLabel* statusLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
};
