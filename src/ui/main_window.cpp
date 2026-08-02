#include "ui/main_window.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QApplication>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QScrollBar>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <QRegularExpression>
#include <set>
#include <unordered_set>

// ── 样式表 ──
static const char* STYLE_SHEET = R"(
QMainWindow {
    background-color: #f8f9fa;
}
QLineEdit {
    padding: 10px 16px;
    border: 2px solid #dee2e6;
    border-radius: 8px;
    font-size: 14px;
    background: white;
}
QLineEdit:focus {
    border-color: #4dabf7;
}
QPushButton {
    padding: 8px 20px;
    border: none;
    border-radius: 6px;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#searchBtn {
    background-color: #228be6;
    color: white;
    padding: 10px 28px;
    font-size: 14px;
}
QPushButton#searchBtn:hover {
    background-color: #1c7ed6;
}
QPushButton#importBtn {
    background-color: #f8f9fa;
    color: #495057;
    border: 1px solid #dee2e6;
}
QPushButton#importBtn:hover {
    background-color: #e9ecef;
}
QPushButton#clearBtn {
    background-color: transparent;
    color: #e03131;
}
QPushButton#clearBtn:hover {
    background-color: #fff5f5;
}
QListWidget {
    border: 1px solid #dee2e6;
    border-radius: 8px;
    background: white;
    font-size: 13px;
}
QListWidget::item {
    padding: 12px;
    border-bottom: 1px solid #f1f3f5;
}
QListWidget::item:hover {
    background-color: #f8f9fa;
}
QListWidget::item:selected {
    background-color: #e7f5ff;
    color: #1864ab;
}
QTextEdit {
    border: 1px solid #dee2e6;
    border-radius: 8px;
    background: white;
    font-size: 14px;
    line-height: 1.6;
    padding: 12px;
}
QLabel#statusLabel {
    color: #868e96;
    font-size: 12px;
}
QPushButton#setApiKeyBtn {
    background-color: #228be6;
    color: white;
    padding: 8px 16px;
    font-size: 12px;
}
QPushButton#setApiKeyBtn:hover {
    background-color: #1c7ed6;
}
QLabel#apiKeyStatus {
    font-size: 12px;
    font-weight: 600;
}
)";

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , retriever_(std::make_unique<rag::Retriever>())
    , generator_(std::make_unique<rag::Generator>())
{
    setupUi();
    setupStyle();
    loadApiKey();
}

MainWindow::~MainWindow() = default;

// ── UI 搭建 ──
void MainWindow::setupUi() {
    setWindowTitle("🚀 RAG 智能检索引擎");
    resize(1100, 700);

    auto* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(24, 16, 24, 16);
    mainLayout->setSpacing(16);

    // ── 顶部标题 ──
    auto* titleLabel = new QLabel("📚 本地智能文档检索系统");
    titleLabel->setStyleSheet("font-size: 22px; font-weight: 800; color: #212529;");
    mainLayout->addWidget(titleLabel);

    // ── 搜索栏 ──
    auto* searchLayout = new QHBoxLayout();
    searchInput_ = new QLineEdit();
    searchInput_->setPlaceholderText("输入你的问题，AI 将从已导入的文档中检索并回答...");
    searchInput_->setMinimumHeight(44);

    searchBtn_ = new QPushButton("🔍 搜索");
    searchBtn_->setObjectName("searchBtn");
    searchBtn_->setMinimumHeight(44);

    searchLayout->addWidget(searchInput_, 1);
    searchLayout->addWidget(searchBtn_);
    mainLayout->addLayout(searchLayout);

    // ── API Key 输入行 ──
    auto* apiKeyLayout = new QHBoxLayout();
    auto* apiKeyLabel = new QLabel("🔑 DeepSeek API Key:");
    apiKeyLabel->setStyleSheet("font-size: 12px; font-weight: 600; color: #495057;");

    apiKeyInput_ = new QLineEdit();
    apiKeyInput_->setPlaceholderText("sk-xxxxxxxxxxxxxxxxxxxxxxxx");
    apiKeyInput_->setEchoMode(QLineEdit::Password);
    apiKeyInput_->setMinimumHeight(36);
    apiKeyInput_->setStyleSheet(
        "QLineEdit { padding: 8px 12px; border: 2px solid #dee2e6; border-radius: 6px; font-size: 13px; }"
    );

    setApiKeyBtn_ = new QPushButton("设置");
    setApiKeyBtn_->setObjectName("setApiKeyBtn");
    setApiKeyBtn_->setMinimumHeight(36);

    apiKeyStatus_ = new QLabel();
    apiKeyStatus_->setObjectName("apiKeyStatus");

    apiKeyLayout->addWidget(apiKeyLabel);
    apiKeyLayout->addWidget(apiKeyInput_, 1);
    apiKeyLayout->addWidget(setApiKeyBtn_);
    apiKeyLayout->addWidget(apiKeyStatus_);
    mainLayout->addLayout(apiKeyLayout);

    // ── 筛选栏 ──
    auto* filterLayout = new QHBoxLayout();
    filterLayout->setSpacing(12);

    auto* filterLabel = new QLabel("🔍 筛选:");
    filterLabel->setStyleSheet("font-size: 12px; font-weight: 600; color: #495057;");

    // 案件类型
    auto* typeLabel = new QLabel("案件类型");
    typeLabel->setStyleSheet("font-size: 11px; color: #868e96;");
    caseTypeFilter_ = new QComboBox();
    caseTypeFilter_->addItems({"全部", "民事", "刑事", "行政", "知识产权", "商事"});
    caseTypeFilter_->setMinimumHeight(30);
    caseTypeFilter_->setStyleSheet(
        "QComboBox { padding: 4px 8px; border: 1px solid #dee2e6; border-radius: 4px; font-size: 12px; background: white; }"
        "QComboBox:hover { border-color: #adb5bd; }"
        "QComboBox::drop-down { border: none; width: 20px; }"
    );

    // 法院级别
    auto* courtLabel = new QLabel("法院级别");
    courtLabel->setStyleSheet("font-size: 11px; color: #868e96;");
    courtLevelFilter_ = new QComboBox();
    courtLevelFilter_->addItems({"全部", "最高人民法院", "高级人民法院", "中级人民法院", "基层人民法院"});
    courtLevelFilter_->setMinimumHeight(30);
    courtLevelFilter_->setStyleSheet(caseTypeFilter_->styleSheet());

    // 年份
    auto* yearLabel = new QLabel("年份");
    yearLabel->setStyleSheet("font-size: 11px; color: #868e96;");
    yearFilter_ = new QComboBox();
    yearFilter_->addItem("全部");
    yearFilter_->setMinimumHeight(30);
    yearFilter_->setStyleSheet(caseTypeFilter_->styleSheet());

    filterLayout->addWidget(filterLabel);
    filterLayout->addWidget(typeLabel);
    filterLayout->addWidget(caseTypeFilter_);
    filterLayout->addWidget(courtLabel);
    filterLayout->addWidget(courtLevelFilter_);
    filterLayout->addWidget(yearLabel);
    filterLayout->addWidget(yearFilter_);
    filterLayout->addStretch();
    mainLayout->addLayout(filterLayout);

    // ── 工具栏按钮 ──
    auto* toolLayout = new QHBoxLayout();
    importBtn_ = new QPushButton("📂 导入文档");
    importBtn_->setObjectName("importBtn");
    clearBtn_ = new QPushButton("🗑 清空索引");
    clearBtn_->setObjectName("clearBtn");

    statusLabel_ = new QLabel("就绪，请先导入文档");
    statusLabel_->setObjectName("statusLabel");

    toolLayout->addWidget(importBtn_);
    toolLayout->addWidget(clearBtn_);
    toolLayout->addStretch();
    toolLayout->addWidget(statusLabel_);
    mainLayout->addLayout(toolLayout);

    // ── 主内容区（左右分栏）──
    auto* splitter = new QSplitter(Qt::Horizontal);

    // 左侧：检索结果列表
    auto* leftPanel = new QWidget();
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto* resultHeader = new QLabel("📋 检索结果");
    resultHeader->setStyleSheet("font-size: 14px; font-weight: 600; color: #495057;");
    leftLayout->addWidget(resultHeader);

    resultList_ = new QListWidget();
    resultList_->setWordWrap(true);
    leftLayout->addWidget(resultList_);

    splitter->addWidget(leftPanel);

    // 右侧：AI 回答区
    auto* rightPanel = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    auto* answerHeader = new QLabel("🤖 AI 回答");
    answerHeader->setStyleSheet("font-size: 14px; font-weight: 600; color: #495057;");
    rightLayout->addWidget(answerHeader);

    aiAnswerArea_ = new QTextEdit();
    aiAnswerArea_->setReadOnly(true);
    aiAnswerArea_->setPlaceholderText("AI 生成的答案将在这里实时显示...");
    rightLayout->addWidget(aiAnswerArea_);

    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 6);

    mainLayout->addWidget(splitter, 1);

    // ── 底部进度条 ──
    progressBar_ = new QProgressBar();
    progressBar_->setVisible(false);
    progressBar_->setTextVisible(false);
    progressBar_->setMaximumHeight(3);
    progressBar_->setStyleSheet("QProgressBar { border: none; background: #e9ecef; border-radius: 2px; } QProgressBar::chunk { background: #228be6; border-radius: 2px; }");
    mainLayout->addWidget(progressBar_);

    // ── 信号连接 ──
    connect(searchBtn_, &QPushButton::clicked, this, &MainWindow::onSearch);
    connect(searchInput_, &QLineEdit::returnPressed, this, &MainWindow::onSearch);
    connect(importBtn_, &QPushButton::clicked, this, &MainWindow::onImportFiles);
    connect(clearBtn_, &QPushButton::clicked, this, &MainWindow::onClearIndex);
    connect(setApiKeyBtn_, &QPushButton::clicked, this, &MainWindow::onSetApiKey);
    connect(apiKeyInput_, &QLineEdit::returnPressed, this, &MainWindow::onSetApiKey);

    // 筛选条件变化时重新过滤结果
    connect(caseTypeFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFilterChanged);
    connect(courtLevelFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFilterChanged);
    connect(yearFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFilterChanged);

    // 点击结果列表中的项 → 高亮对应内容
    connect(resultList_, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
        QString text = item->data(Qt::UserRole).toString();
        if (!text.isEmpty()) {
            aiAnswerArea_->clear();
            aiAnswerArea_->setPlainText(text);
        }
    });
}

void MainWindow::setupStyle() {
    setStyleSheet(STYLE_SHEET);
}

// ── API Key ──
void MainWindow::loadApiKey() {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key && strlen(key) > 0) {
        QString qkey = QString::fromStdString(key);
        retriever_->setApiKey(key);
        generator_->setApiKey(key);
        apiKeyInput_->setText(qkey);
        updateApiKeyStatus(true, "✅ 已从环境变量加载");
    } else {
        updateApiKeyStatus(false, "⚠️ 未设置");
    }
}

void MainWindow::onSetApiKey() {
    QString key = apiKeyInput_->text().trimmed();
    if (key.isEmpty()) {
        // 清空 API Key
        retriever_->setApiKey("");
        generator_->setApiKey("");
        updateApiKeyStatus(false, "⚠️ 未设置");
        return;
    }

    QString errorMsg;
    if (!validateApiKeyFormat(key, &errorMsg)) {
        updateApiKeyStatus(false, errorMsg);
        return;
    }

    std::string keyStr = key.toStdString();
    retriever_->setApiKey(keyStr);
    generator_->setApiKey(keyStr);
    updateApiKeyStatus(true, "✅ 已设置");
}

bool MainWindow::validateApiKeyFormat(const QString& key, QString* errorMsg) {
    // DeepSeek API Key 格式：sk- 开头，总长度 ≥ 20 字符
    if (!key.startsWith("sk-")) {
        if (errorMsg) *errorMsg = "❌ 格式错误：必须以 sk- 开头";
        return false;
    }
    if (key.length() < 20) {
        if (errorMsg) *errorMsg = "❌ 格式错误：Key 长度不足（至少 20 字符）";
        return false;
    }
    // 检查是否只包含合法字符（字母、数字、-）
    static QRegularExpression validChars("^[a-zA-Z0-9\\-]+$");
    if (!validChars.match(key).hasMatch()) {
        if (errorMsg) *errorMsg = "❌ 格式错误：包含非法字符";
        return false;
    }
    return true;
}

void MainWindow::updateApiKeyStatus(bool valid, const QString& message) {
    if (apiKeyStatus_) {
        apiKeyStatus_->setText(message);
        if (valid) {
            apiKeyStatus_->setStyleSheet("color: #2f9e44; font-size: 12px;");
        } else {
            apiKeyStatus_->setStyleSheet("color: #e03131; font-size: 12px;");
        }
    }
}

// ── 搜索 ──
void MainWindow::onSearch() {
    QString query = searchInput_->text().trimmed();
    if (query.isEmpty()) return;

    if (retriever_->docCount() == 0) {
        QMessageBox::information(this, "提示", "请先导入文档再搜索！");
        return;
    }

    searchBtn_->setEnabled(false);
    progressBar_->setVisible(true);
    progressBar_->setRange(0, 0);  // 不确定模式

    aiAnswerArea_->clear();
    aiAnswerArea_->setHtml("<i style='color:#868e96'>正在检索...</i>");

    // 异步执行检索 + 生成
    QTimer::singleShot(100, this, [this, query]() {
        // Step 1: 检索（多取一些结果用于筛选）
        auto results = retriever_->search(query.toStdString(), 20);
        cachedResults_ = results;
        currentQuery_ = query;

        // 填充年份筛选器
        populateYearFilter(results);

        // 应用筛选并显示
        auto filtered = applyFiltersAndDisplay();

        // Step 2: AI 生成答案（使用筛选后的结果构建上下文）
        if (generator_->isReady() && !filtered.empty()) {
            // ── 检测聚合型问题（跨文档查询）──
            // 仅用明确指向「全部文档」的短语，避免误判聚焦型查询
            static const std::vector<std::string> AGGREGATE_MARKERS = {
                "这些案件", "所有案件", "全部案件", "各案件", "各个案件", "每个案件",
                "这些文档", "所有文档", "全部文档", "各文档", "这些文件", "所有文件",
                "哪些案件", "汇总", "统计", "总共", "一共"
            };
            bool isAggregate = false;
            std::string qstr = query.toStdString();
            for (const auto& marker : AGGREGATE_MARKERS) {
                if (qstr.find(marker) != std::string::npos) {
                    isAggregate = true;
                    break;
                }
            }

            if (isAggregate) {
                // ── 聚合模式 ──
                // 1. 用更大的 topK 重新检索，并在 UI 层做文档去重
                auto wideResults = retriever_->search(qstr, 50);
                std::vector<rag::SearchResult> deduped;
                constexpr int perDocLimit = 2;
                std::unordered_map<std::string, int> docCount;
                for (auto& r : wideResults) {
                    int& cnt = docCount[r.docId];
                    if (cnt >= perDocLimit) continue;
                    ++cnt;
                    deduped.push_back(std::move(r));
                }
                if (!deduped.empty()) {
                    filtered = deduped;
                    displayResults(filtered);
                }

                // 2. 收集全部文档元数据注入 AI
                auto allIds = retriever_->allDocIds();
                std::ostringstream allMeta;
                for (size_t i = 0; i < allIds.size(); ++i) {
                    const auto* meta = retriever_->getMetadata(allIds[i]);
                    if (meta && !meta->isEmpty()) {
                        allMeta << "- " << allIds[i];
                        if (!meta->caseNumber.empty()) allMeta << " | 案号: " << meta->caseNumber;
                        if (!meta->court.empty()) allMeta << " | 法院: " << meta->court;
                        if (!meta->caseType.empty()) allMeta << " | 类型: " << meta->caseType;
                        if (!meta->date.empty()) allMeta << " | 日期: " << meta->date;
                        if (!meta->procedure.empty()) allMeta << " | 程序: " << meta->procedure;
                        if (!meta->litigants.empty()) allMeta << " | 当事人: " << meta->litigants;
                        allMeta << "\n";
                    }
                }
                if (allMeta.tellp() > 0) {
                    allMeta << "\n（以上为全部 " << allIds.size() << " 个已导入文档的元数据汇总）\n";
                }
                generator_->setMetadataContext(allMeta.str());
            } else {
                // ── 聚焦模式：保持原始排序，不做去重 ──
                generator_->setMetadataContext(buildMetadataSummary(filtered));
            }

            std::string context = retriever_->buildContext(filtered, 2000);

            aiAnswerArea_->clear();
            aiAnswerArea_->setHtml("<b style='color:#495057'>🤖 AI 正在生成回答...</b><br><br>");

            try {
                generator_->generate(
                    query.toStdString(),
                    context,
                    [this](const std::string& delta) {
                        QMetaObject::invokeMethod(this, [this, text = QString::fromStdString(delta)]() {
                            aiAnswerArea_->moveCursor(QTextCursor::End);
                            aiAnswerArea_->insertPlainText(text);
                            aiAnswerArea_->moveCursor(QTextCursor::End);
                        }, Qt::QueuedConnection);
                    }
                );
            } catch (const std::exception& e) {
                std::string errStr = e.what();
                if (errStr.find("no response") != std::string::npos ||
                    errStr.find("timeout") != std::string::npos ||
                    errStr.find("connection") != std::string::npos) {
                    appendAiAnswer(
                        "\n\n❌ 无法连接到 DeepSeek API\n\n"
                        "可能原因：\n"
                        "• 网络连接异常，请检查是否能访问 api.deepseek.com\n"
                        "• API Key 无效或已过期\n"
                        "• 请求超时，请稍后重试\n\n"
                        "技术细节：" + QString::fromStdString(errStr));
                } else {
                    appendAiAnswer("\n\n❌ AI 生成失败：" + QString::fromStdString(errStr));
                }
            }
        } else if (filtered.empty() && !cachedResults_.empty()) {
            aiAnswerArea_->clear();
            aiAnswerArea_->setHtml(
                "<p style='color:#e8590c; font-weight:600;'>⚠️ 筛选后无结果</p>"
                "<p style='color:#868e96;'>当前筛选条件下没有匹配的文档（原始搜索找到 "
                + QString::number(static_cast<int>(cachedResults_.size())) + " 条结果）。"
                "请尝试放宽筛选条件。</p>"
            );
        } else if (cachedResults_.empty()) {
            aiAnswerArea_->clear();
            aiAnswerArea_->setHtml(
                "<p style='color:#e03131; font-weight:600;'>⚠️ 未找到相关文档</p>"
                "<p style='color:#868e96;'>你的问题未能匹配到已导入文档中的内容。建议：</p>"
                "<ul style='color:#868e96;'>"
                "<li>确认已导入相关文档（点击「📂 导入文档」）</li>"
                "<li>尝试用文档中出现过的关键词搜索</li>"
                "<li>查看左侧状态栏确认已导入的文档数量</li>"
                "</ul>"
            );
        } else if (!generator_->isReady()) {
            aiAnswerArea_->clear();
            aiAnswerArea_->setHtml(
                "<p style='color:#e8590c; font-weight:600;'>🔑 未配置 API Key</p>"
                "<p style='color:#868e96;'>请在搜索栏上方的输入框中填写 DeepSeek API Key（sk- 开头），</p>"
                "<p style='color:#868e96;'>点击「设置」后即可启用 AI 智能回答功能。</p>"
                "<p style='color:#adb5bd; font-size:12px;'>获取 Key：<a href='https://platform.deepseek.com'>platform.deepseek.com</a></p>"
            );
        }

        progressBar_->setVisible(false);
        searchBtn_->setEnabled(true);
        statusLabel_->setText(QString("检索完成，找到 %1 条结果（筛选后 %2 条）")
                              .arg(static_cast<int>(cachedResults_.size()))
                              .arg(static_cast<int>(filtered.size())));
    });
}

// ── 导入文档 ──
void MainWindow::onImportFiles() {
    QStringList files = QFileDialog::getOpenFileNames(
        this,
        "选择文档",
        QString(),
        "文档文件 (*.txt *.md *.pdf *.csv *.json *.xml);;文本文件 (*.txt *.md *.csv *.json *.xml);;PDF 文件 (*.pdf);;所有文件 (*)"
    );

    if (files.isEmpty()) return;

    progressBar_->setVisible(true);
    progressBar_->setRange(0, files.size());
    statusLabel_->setText("正在导入文档...");

    int imported = 0;
    QStringList errors;
    for (int i = 0; i < files.size(); ++i) {
        try {
            retriever_->addDocument(files[i].toStdString());
            imported++;
        } catch (const std::exception& e) {
            errors.append(QString::fromStdString(e.what()));
        }
        progressBar_->setValue(i + 1);
        QApplication::processEvents();
    }

    progressBar_->setVisible(false);
    if (!errors.isEmpty()) {
        statusLabel_->setText(QString("⚠️ 导入完成，%1 个成功，%2 个失败：%3")
                              .arg(imported)
                              .arg(errors.size())
                              .arg(errors.first()));
    } else {
        statusLabel_->setText(QString("✅ 已导入 %1 个文档，共 %2 个文本块")
                              .arg(imported)
                              .arg(retriever_->docCount()));
    }
}

// ── 清空索引 ──
void MainWindow::onClearIndex() {
    auto reply = QMessageBox::question(
        this,
        "确认清空",
        "确定要清空所有已导入的文档索引吗？此操作不可恢复。",
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        // 重建 retriever，保留当前 API Key
        QString currentKey = apiKeyInput_->text().trimmed();
        retriever_ = std::make_unique<rag::Retriever>();
        if (generator_->isReady()) {
            retriever_->setApiKey(currentKey.toStdString());
        }

        resultList_->clear();
        aiAnswerArea_->clear();
        statusLabel_->setText("索引已清空");
    }
}

// ── 显示检索结果 ──
void MainWindow::displayResults(const std::vector<rag::SearchResult>& results) {
    resultList_->clear();

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::ostringstream oss;
        oss << "【" << (i + 1) << "】" << r.docId
            << "  相关度: " << std::fixed << std::setprecision(2) << r.finalScore
            << "  (BM25: " << r.bm25Score << " | 向量: " << r.vectorScore << ")";

        auto* item = new QListWidgetItem(QString::fromStdString(oss.str()));
        item->setData(Qt::UserRole, QString::fromStdString(r.content));

        // 截取内容预览
        QString preview = QString::fromStdString(r.content).left(200);
        if (r.content.size() > 200) preview += "...";

        item->setToolTip(preview);
        resultList_->addItem(item);
    }
}

// ── 筛选逻辑 ──
std::vector<rag::SearchResult> MainWindow::getFilteredResults() {
    if (cachedResults_.empty()) return {};

    QString caseType = caseTypeFilter_->currentText();
    QString courtLevel = courtLevelFilter_->currentText();
    QString year = yearFilter_->currentText();

    // 无筛选条件，直接返回全部
    if (caseType == "全部" && courtLevel == "全部" && year == "全部") {
        return cachedResults_;
    }

    std::vector<rag::SearchResult> filtered;
    for (const auto& r : cachedResults_) {
        const auto* meta = retriever_->getMetadata(r.docId);

        // 案件类型筛选
        if (caseType != "全部") {
            if (!meta || meta->caseType != caseType.toStdString()) {
                continue;
            }
        }

        // 法院级别筛选
        if (courtLevel != "全部") {
            if (!meta || meta->court.empty()) {
                continue;
            }
            std::string level = courtLevel.toStdString();
            bool match = false;
            if (level == "最高人民法院") {
                match = (meta->court.find("最高") != std::string::npos);
            } else if (level == "高级人民法院") {
                match = (meta->court.find("高级") != std::string::npos);
            } else if (level == "中级人民法院") {
                match = (meta->court.find("中级") != std::string::npos);
            } else if (level == "基层人民法院") {
                // 基层法院：不含 最高/高级/中级
                match = (meta->court.find("最高") == std::string::npos &&
                         meta->court.find("高级") == std::string::npos &&
                         meta->court.find("中级") == std::string::npos);
            }
            if (!match) continue;
        }

        // 年份筛选
        if (year != "全部") {
            if (!meta || meta->date.empty()) {
                continue;
            }
            // 日期格式为 YYYY-MM-DD，取前 4 位
            if (meta->date.substr(0, 4) != year.toStdString()) {
                continue;
            }
        }

        filtered.push_back(r);
    }

    return filtered;
}

std::vector<rag::SearchResult> MainWindow::applyFiltersAndDisplay() {
    auto filtered = getFilteredResults();
    displayResults(filtered);
    return filtered;
}

void MainWindow::onFilterChanged() {
    if (cachedResults_.empty()) return;
    applyFiltersAndDisplay();
}

void MainWindow::populateYearFilter(const std::vector<rag::SearchResult>& results) {
    // 收集所有结果中的年份
    std::set<QString> years;
    for (const auto& r : results) {
        const auto* meta = retriever_->getMetadata(r.docId);
        if (meta && meta->date.size() >= 4) {
            years.insert(QString::fromStdString(meta->date.substr(0, 4)));
        }
    }

    // 保留"全部"选项，更新年份列表
    QString currentYear = yearFilter_->currentText();
    yearFilter_->blockSignals(true);
    yearFilter_->clear();
    yearFilter_->addItem("全部");
    for (const auto& y : years) {
        yearFilter_->addItem(y);
    }
    // 恢复之前的选择
    int idx = yearFilter_->findText(currentYear);
    if (idx >= 0) yearFilter_->setCurrentIndex(idx);
    yearFilter_->blockSignals(false);
}

std::string MainWindow::buildMetadataSummary(const std::vector<rag::SearchResult>& results) {
    // 收集所有结果的元数据（去重）
    std::set<std::string> seenIds;
    std::ostringstream oss;

    for (const auto& r : results) {
        if (seenIds.count(r.docId)) continue;
        seenIds.insert(r.docId);

        const auto* meta = retriever_->getMetadata(r.docId);
        if (!meta || meta->isEmpty()) continue;

        oss << "- " << r.docId;
        if (!meta->caseNumber.empty()) oss << " | 案号: " << meta->caseNumber;
        if (!meta->court.empty()) oss << " | 法院: " << meta->court;
        if (!meta->caseType.empty()) oss << " | 类型: " << meta->caseType;
        if (!meta->date.empty()) oss << " | 日期: " << meta->date;
        if (!meta->procedure.empty()) oss << " | 程序: " << meta->procedure;
        if (!meta->litigants.empty()) oss << " | 当事人: " << meta->litigants;
        oss << "\n";
    }

    return oss.str();
}

void MainWindow::appendAiAnswer(const QString& text) {
    aiAnswerArea_->moveCursor(QTextCursor::End);
    aiAnswerArea_->insertPlainText(text);
    aiAnswerArea_->moveCursor(QTextCursor::End);
}
