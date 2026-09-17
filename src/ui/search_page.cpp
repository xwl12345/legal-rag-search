#include "ui/search_page.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "ui/app_theme.h"

namespace {

/// 给按钮打 role 属性，具体配色由 QSS 的属性选择器负责。
void setButtonRole(QPushButton* button, const char* role) {
    button->setProperty("role", QString::fromUtf8(role));
}

}  // namespace

SearchPage::SearchPage(QWidget* parent)
    : QWidget(parent)
    , retriever_(std::make_unique<rag::Retriever>())
    , generator_(std::make_unique<rag::Generator>())
{
    setupUi();
    loadApiKey();
    emitEngineStats();
}

SearchPage::~SearchPage() = default;

// ── UI 搭建 ──
void SearchPage::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 16, 22, 14);
    root->setSpacing(10);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    auto* titleLabel = new QLabel(QStringLiteral("检索问答"), this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    auto* subtitleLabel = new QLabel(QStringLiteral("关键词 + 语义双路混合检索"), this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(titleLabel);
    head->addWidget(subtitleLabel);
    head->addStretch();
    root->addLayout(head);

    // ── 搜索栏 ──
    auto* searchLayout = new QHBoxLayout();
    searchLayout->setSpacing(10);

    searchInput_ = new QLineEdit(this);
    searchInput_->setObjectName(QStringLiteral("searchInput"));
    searchInput_->setPlaceholderText(
        QStringLiteral("输入你的问题，AI 将从已导入的文档中检索并回答..."));
    searchInput_->setMinimumHeight(46);

    searchBtn_ = new QPushButton(QStringLiteral("检 索"), this);
    searchBtn_->setObjectName(QStringLiteral("searchBtn"));
    setButtonRole(searchBtn_, "primary");
    searchBtn_->setMinimumHeight(46);
    searchBtn_->setMinimumWidth(112);

    searchLayout->addWidget(searchInput_, 1);
    searchLayout->addWidget(searchBtn_);
    root->addLayout(searchLayout);

    // ── API Key 输入行 ──
    auto* apiKeyLayout = new QHBoxLayout();
    apiKeyLayout->setSpacing(10);

    auto* apiKeyLabel = new QLabel(QStringLiteral("DeepSeek API Key"), this);
    apiKeyLabel->setObjectName(QStringLiteral("fieldLabel"));

    apiKeyInput_ = new QLineEdit(this);
    apiKeyInput_->setPlaceholderText(QStringLiteral("sk-xxxxxxxxxxxxxxxxxxxxxxxx"));
    apiKeyInput_->setEchoMode(QLineEdit::Password);
    apiKeyInput_->setMinimumHeight(36);

    setApiKeyBtn_ = new QPushButton(QStringLiteral("设置"), this);
    setApiKeyBtn_->setMinimumHeight(36);

    apiKeyStatus_ = new QLabel(this);
    apiKeyStatus_->setObjectName(QStringLiteral("apiKeyStatus"));

    apiKeyLayout->addWidget(apiKeyLabel);
    apiKeyLayout->addWidget(apiKeyInput_, 1);
    apiKeyLayout->addWidget(setApiKeyBtn_);
    apiKeyLayout->addWidget(apiKeyStatus_);
    root->addLayout(apiKeyLayout);

    // ── 筛选栏 ──
    auto* filterLayout = new QHBoxLayout();
    filterLayout->setSpacing(8);

    auto* filterLabel = new QLabel(QStringLiteral("筛选"), this);
    filterLabel->setObjectName(QStringLiteral("fieldLabel"));

    auto* typeLabel = new QLabel(QStringLiteral("案件类型"), this);
    typeLabel->setObjectName(QStringLiteral("fieldLabel"));
    caseTypeFilter_ = new QComboBox(this);
    caseTypeFilter_->setObjectName(QStringLiteral("caseTypeFilter"));
    caseTypeFilter_->addItems({QStringLiteral("全部"), QStringLiteral("民事"),
                               QStringLiteral("刑事"), QStringLiteral("行政"),
                               QStringLiteral("知识产权"), QStringLiteral("商事")});
    caseTypeFilter_->setMinimumHeight(32);

    auto* courtLabel = new QLabel(QStringLiteral("法院级别"), this);
    courtLabel->setObjectName(QStringLiteral("fieldLabel"));
    courtLevelFilter_ = new QComboBox(this);
    courtLevelFilter_->setObjectName(QStringLiteral("courtLevelFilter"));
    courtLevelFilter_->addItems({QStringLiteral("全部"), QStringLiteral("最高人民法院"),
                                 QStringLiteral("高级人民法院"), QStringLiteral("中级人民法院"),
                                 QStringLiteral("基层人民法院")});
    courtLevelFilter_->setMinimumHeight(32);

    auto* yearLabel = new QLabel(QStringLiteral("年份"), this);
    yearLabel->setObjectName(QStringLiteral("fieldLabel"));
    yearFilter_ = new QComboBox(this);
    yearFilter_->addItem(QStringLiteral("全部"));
    yearFilter_->setMinimumHeight(32);

    filterLayout->addWidget(filterLabel);
    filterLayout->addSpacing(6);
    filterLayout->addWidget(typeLabel);
    filterLayout->addWidget(caseTypeFilter_);
    filterLayout->addWidget(courtLabel);
    filterLayout->addWidget(courtLevelFilter_);
    filterLayout->addWidget(yearLabel);
    filterLayout->addWidget(yearFilter_);
    filterLayout->addStretch();
    root->addLayout(filterLayout);

    // ── 工具栏按钮 ──
    auto* toolLayout = new QHBoxLayout();
    toolLayout->setSpacing(10);

    importBtn_ = new QPushButton(QStringLiteral("＋ 导入文档"), this);
    setButtonRole(importBtn_, "primary");

    clearBtn_ = new QPushButton(QStringLiteral("清空索引"), this);
    setButtonRole(clearBtn_, "danger");

    statusLabel_ = new QLabel(QStringLiteral("就绪，请先导入文档"), this);
    statusLabel_->setObjectName(QStringLiteral("hint"));

    toolLayout->addWidget(importBtn_);
    toolLayout->addWidget(clearBtn_);
    toolLayout->addSpacing(8);
    toolLayout->addWidget(statusLabel_, 1);
    root->addLayout(toolLayout);

    // ── 主内容区（左右卡片）──
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);

    // 左侧：检索结果列表
    auto* leftCard = new QFrame(this);
    leftCard->setObjectName(QStringLiteral("card"));
    auto* leftLayout = new QVBoxLayout(leftCard);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    auto* resultHeader = new QLabel(QStringLiteral("检索结果（按相关度排序）"), leftCard);
    resultHeader->setObjectName(QStringLiteral("cardTitle"));
    leftLayout->addWidget(resultHeader);

    resultList_ = new QListWidget(leftCard);
    resultList_->setObjectName(QStringLiteral("resultList"));
    resultList_->setWordWrap(true);
    leftLayout->addWidget(resultList_, 1);

    split->addWidget(leftCard);

    // 右侧：AI 回答区
    auto* rightCard = new QFrame(this);
    rightCard->setObjectName(QStringLiteral("card"));
    auto* rightLayout = new QVBoxLayout(rightCard);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    auto* answerHeader = new QLabel(QStringLiteral("AI 分析回答"), rightCard);
    answerHeader->setObjectName(QStringLiteral("cardTitle"));
    rightLayout->addWidget(answerHeader);

    aiAnswerArea_ = new QTextEdit(rightCard);
    aiAnswerArea_->setObjectName(QStringLiteral("aiAnswer"));
    aiAnswerArea_->setReadOnly(true);
    aiAnswerArea_->setPlaceholderText(
        QStringLiteral("AI 生成的答案将在这里实时显示..."));
    rightLayout->addWidget(aiAnswerArea_, 1);

    split->addWidget(rightCard);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 6);

    root->addWidget(split, 1);

    // ── 底部进度条 ──
    progressBar_ = new QProgressBar(this);
    progressBar_->setObjectName(QStringLiteral("taskProgress"));
    progressBar_->setTextVisible(false);
    progressBar_->setMaximumHeight(3);
    progressBar_->hide();
    root->addWidget(progressBar_);

    // ── 信号连接 ──
    connect(searchBtn_, &QPushButton::clicked, this, &SearchPage::onSearch);
    connect(searchInput_, &QLineEdit::returnPressed, this, &SearchPage::onSearch);
    connect(importBtn_, &QPushButton::clicked, this, &SearchPage::onPickImportFiles);
    connect(clearBtn_, &QPushButton::clicked, this, &SearchPage::onClearIndex);
    connect(setApiKeyBtn_, &QPushButton::clicked, this, &SearchPage::onSetApiKey);
    connect(apiKeyInput_, &QLineEdit::returnPressed, this, &SearchPage::onSetApiKey);

    // 筛选条件变化时重新过滤结果
    connect(caseTypeFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchPage::onFilterChanged);
    connect(courtLevelFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchPage::onFilterChanged);
    connect(yearFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchPage::onFilterChanged);

    // 点击结果列表中的项 → 查看全文片段
    connect(resultList_, &QListWidget::itemClicked, [this](QListWidgetItem* item) {
        const QString text = item->data(Qt::UserRole).toString();
        if (!text.isEmpty()) {
            aiAnswerArea_->setPlainText(text);
        }
    });
}

void SearchPage::emitEngineStats() {
    // 注意：Retriever::docCount() 走的是 InvertedIndex::totalDocs()，
    // 而 totalDocs_ 是按 (docId, chunkIndex) 逐块累加的 —— 它实际是「文本块数」。
    // 状态栏要显示真实文档数，须用 allDocIds().size()。
    const int docs = static_cast<int>(retriever_->allDocIds().size());
    emit engineStatsChanged(docs, retriever_->chunkCount());
}

// ── API Key ──
void SearchPage::loadApiKey() {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key && std::strlen(key) > 0) {
        const QString qkey = QString::fromStdString(key);
        retriever_->setApiKey(key);
        generator_->setApiKey(key);
        apiKeyInput_->setText(qkey);
        updateApiKeyStatus(true, QStringLiteral("● 已从环境变量加载"));
    } else {
        updateApiKeyStatus(false, QStringLiteral("● 未设置"));
    }
    emit apiKeyStateChanged(generator_->isReady());
}

void SearchPage::onSetApiKey() {
    const QString key = apiKeyInput_->text().trimmed();
    if (key.isEmpty()) {
        // 清空 API Key
        retriever_->setApiKey("");
        generator_->setApiKey("");
        updateApiKeyStatus(false, QStringLiteral("● 未设置"));
        emit apiKeyStateChanged(false);
        return;
    }

    QString errorMsg;
    if (!validateApiKeyFormat(key, &errorMsg)) {
        updateApiKeyStatus(false, errorMsg);
        emit apiKeyStateChanged(false);
        return;
    }

    const std::string keyStr = key.toStdString();
    retriever_->setApiKey(keyStr);
    generator_->setApiKey(keyStr);
    updateApiKeyStatus(true, QStringLiteral("● 已设置"));
    emit apiKeyStateChanged(true);
}

bool SearchPage::validateApiKeyFormat(const QString& key, QString* errorMsg) {
    // DeepSeek API Key 格式：sk- 开头，总长度 ≥ 20 字符
    if (!key.startsWith(QStringLiteral("sk-"))) {
        if (errorMsg) *errorMsg = QStringLiteral("✕ 格式错误：必须以 sk- 开头");
        return false;
    }
    if (key.length() < 20) {
        if (errorMsg) *errorMsg = QStringLiteral("✕ 格式错误：Key 长度不足（至少 20 字符）");
        return false;
    }
    // 检查是否只包含合法字符（字母、数字、-）
    static const QRegularExpression validChars(QStringLiteral("^[a-zA-Z0-9\\-]+$"));
    if (!validChars.match(key).hasMatch()) {
        if (errorMsg) *errorMsg = QStringLiteral("✕ 格式错误：包含非法字符");
        return false;
    }
    return true;
}

void SearchPage::updateApiKeyStatus(bool valid, const QString& message) {
    if (!apiKeyStatus_) {
        return;
    }
    apiKeyStatus_->setText(message);
    AppTheme::setStatusFlag(apiKeyStatus_, valid);
}

// ── 搜索 ──
void SearchPage::onSearch() {
    const QString query = searchInput_->text().trimmed();
    if (query.isEmpty()) return;

    if (retriever_->docCount() == 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先导入文档再搜索！"));
        return;
    }

    searchBtn_->setEnabled(false);
    progressBar_->setVisible(true);
    progressBar_->setRange(0, 0);  // 不确定模式

    aiAnswerArea_->clear();
    aiAnswerArea_->setHtml(QStringLiteral("<i style='color:#64748B'>正在检索...</i>"));

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
            const std::string qstr = query.toStdString();
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

            const std::string context = retriever_->buildContext(filtered, 2000);

            aiAnswerArea_->clear();
            aiAnswerArea_->setHtml(
                QStringLiteral("<b style='color:#14213D'>AI 正在生成回答...</b><br><br>"));

            try {
                generator_->generate(
                    query.toStdString(),
                    context,
                    [this](const std::string& delta) {
                        QMetaObject::invokeMethod(this, [this, text = QString::fromStdString(delta)]() {
                            // 剥离模型输出中的 Markdown 标记（** 加粗、行首 # 标题），
                            // 纯文本区不渲染这些符号
                            QString cleaned = text;
                            cleaned.remove(QStringLiteral("**"));
                            cleaned.replace(QRegularExpression(QStringLiteral("(^|\\n)#{1,6}\\s+")),
                                            QStringLiteral("\\1"));
                            aiAnswerArea_->moveCursor(QTextCursor::End);
                            aiAnswerArea_->insertPlainText(cleaned);
                            aiAnswerArea_->moveCursor(QTextCursor::End);
                        }, Qt::QueuedConnection);
                    }
                );
            } catch (const std::exception& e) {
                const std::string errStr = e.what();
                if (errStr.find("no response") != std::string::npos ||
                    errStr.find("timeout") != std::string::npos ||
                    errStr.find("connection") != std::string::npos) {
                    appendAiAnswer(
                        QStringLiteral("\n\n✕ 无法连接到 DeepSeek API\n\n"
                                       "可能原因：\n"
                                       "• 网络连接异常，请检查是否能访问 api.deepseek.com\n"
                                       "• API Key 无效或已过期\n"
                                       "• 请求超时，请稍后重试\n\n"
                                       "技术细节：") + QString::fromStdString(errStr));
                } else {
                    appendAiAnswer(QStringLiteral("\n\n✕ AI 生成失败：")
                                   + QString::fromStdString(errStr));
                }
            }
        } else if (filtered.empty() && !cachedResults_.empty()) {
            aiAnswerArea_->setHtml(
                QStringLiteral("<p style='color:#B7791F; font-weight:600;'>⚠ 筛选后无结果</p>"
                               "<p style='color:#64748B;'>当前筛选条件下没有匹配的文档（原始搜索找到 ")
                + QString::number(static_cast<int>(cachedResults_.size()))
                + QStringLiteral(" 条结果）。请尝试放宽筛选条件。</p>"));
        } else if (cachedResults_.empty()) {
            aiAnswerArea_->setHtml(
                QStringLiteral("<p style='color:#C62828; font-weight:600;'>⚠ 未找到相关文档</p>"
                               "<p style='color:#64748B;'>你的问题未能匹配到已导入文档中的内容。建议：</p>"
                               "<ul style='color:#64748B;'>"
                               "<li>确认已导入相关文档（点击「＋ 导入文档」）</li>"
                               "<li>尝试用文档中出现过的关键词搜索</li>"
                               "<li>查看左下角状态栏确认已导入的文档数量</li>"
                               "</ul>"));
        } else if (!generator_->isReady()) {
            aiAnswerArea_->setHtml(
                QStringLiteral("<p style='color:#B7791F; font-weight:600;'>🔑 未配置 API Key</p>"
                               "<p style='color:#64748B;'>请在上方输入框中填写 DeepSeek API Key（sk- 开头），</p>"
                               "<p style='color:#64748B;'>点击「设置」后即可启用 AI 智能回答功能。</p>"
                               "<p style='color:#94A3B8; font-size:11px;'>获取 Key："
                               "<a href='https://platform.deepseek.com'>platform.deepseek.com</a></p>"));
        }

        progressBar_->setVisible(false);
        searchBtn_->setEnabled(true);
        statusLabel_->setText(QStringLiteral("检索完成，找到 %1 条结果（筛选后 %2 条）")
                                  .arg(static_cast<int>(cachedResults_.size()))
                                  .arg(static_cast<int>(filtered.size())));
    });
}

// ── 导入文档 ──
void SearchPage::onPickImportFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("选择文档"),
        QString(),
        QStringLiteral("文档文件 (*.txt *.md *.pdf *.csv *.json *.xml);;文本文件 (*.txt *.md *.csv *.json *.xml);;PDF 文件 (*.pdf);;所有文件 (*)")
    );

    if (files.isEmpty()) return;

    importPaths(files);
}

void SearchPage::importPaths(const QStringList& files) {
    if (files.isEmpty()) return;

    progressBar_->setVisible(true);
    progressBar_->setRange(0, files.size());
    statusLabel_->setText(
        QStringLiteral("正在导入文档...（扫描件 OCR 逐页识别，可能需要数分钟，请耐心等待）"));

    int imported = 0;
    int chunksAdded = 0;
    int ocrImported = 0;
    QStringList errors;
    for (int i = 0; i < files.size(); ++i) {
        try {
            const auto result = retriever_->addDocument(files[i].toStdString());
            if (result.imported) {
                ++imported;
                chunksAdded += result.chunksAdded;
                if (result.source == document::ParseSource::Ocr) {
                    ++ocrImported;
                }
            } else {
                const QString name = QFileInfo(files[i]).fileName();
                const QString reason = result.diagnostic.empty()
                    ? QStringLiteral("未能提取可检索文本")
                    : QString::fromStdString(result.diagnostic);
                errors.append(name + QStringLiteral("：") + reason);
            }
        } catch (const std::exception& e) {
            errors.append(QFileInfo(files[i]).fileName() + QStringLiteral("：") +
                          QString::fromStdString(e.what()));
        }
        progressBar_->setValue(i + 1);
        QApplication::processEvents();
    }

    progressBar_->setVisible(false);
    if (errors.isEmpty()) {
        QString message = QStringLiteral("✓ 已导入 %1 个文档，新增 %2 个文本块")
                              .arg(imported)
                              .arg(chunksAdded);
        if (ocrImported > 0) {
            message += QStringLiteral("（其中 %1 个通过 OCR 识别）").arg(ocrImported);
        }
        statusLabel_->setText(message);
    } else {
        const QString summary = imported > 0
            ? QStringLiteral("⚠ 导入完成：%1 个成功，%2 个失败，新增 %3 个文本块")
                  .arg(imported).arg(errors.size()).arg(chunksAdded)
            : QStringLiteral("⚠ 未导入任何文档：%1 个文件未能提取可检索文本")
                  .arg(errors.size());
        statusLabel_->setText(summary);
        QMessageBox::warning(this, QStringLiteral("文档导入提示"),
                             summary + QStringLiteral("\n\n失败原因：\n") + errors.join(QStringLiteral("\n")));
    }

    emitEngineStats();
}

// ── 清空索引 ──
void SearchPage::onClearIndex() {
    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("确认清空"),
        QStringLiteral("确定要清空所有已导入的文档索引吗？此操作不可恢复。"),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        // 重建 retriever，保留当前 API Key
        const QString currentKey = apiKeyInput_->text().trimmed();
        retriever_ = std::make_unique<rag::Retriever>();
        if (generator_->isReady()) {
            retriever_->setApiKey(currentKey.toStdString());
        }

        cachedResults_.clear();
        resultList_->clear();
        aiAnswerArea_->clear();
        statusLabel_->setText(QStringLiteral("索引已清空"));

        emitEngineStats();
    }
}

// ── 显示检索结果 ──
void SearchPage::displayResults(const std::vector<rag::SearchResult>& results) {
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
        if (r.content.size() > 200) preview += QStringLiteral("...");

        item->setToolTip(preview);
        resultList_->addItem(item);
    }
}

// ── 筛选逻辑 ──
std::vector<rag::SearchResult> SearchPage::getFilteredResults() {
    if (cachedResults_.empty()) return {};

    const QString caseType = caseTypeFilter_->currentText();
    const QString courtLevel = courtLevelFilter_->currentText();
    const QString year = yearFilter_->currentText();

    // 无筛选条件，直接返回全部
    if (caseType == QStringLiteral("全部") && courtLevel == QStringLiteral("全部")
        && year == QStringLiteral("全部")) {
        return cachedResults_;
    }

    std::vector<rag::SearchResult> filtered;
    for (const auto& r : cachedResults_) {
        const auto* meta = retriever_->getMetadata(r.docId);

        // 案件类型筛选
        if (caseType != QStringLiteral("全部")) {
            if (!meta || meta->caseType != caseType.toStdString()) {
                continue;
            }
        }

        // 法院级别筛选
        if (courtLevel != QStringLiteral("全部")) {
            if (!meta || meta->court.empty()) {
                continue;
            }
            const std::string level = courtLevel.toStdString();
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
        if (year != QStringLiteral("全部")) {
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

std::vector<rag::SearchResult> SearchPage::applyFiltersAndDisplay() {
    auto filtered = getFilteredResults();
    displayResults(filtered);
    return filtered;
}

void SearchPage::onFilterChanged() {
    if (cachedResults_.empty()) return;

    const auto filtered = applyFiltersAndDisplay();
    statusLabel_->setText(QStringLiteral("筛选生效：%1 / %2 条")
                              .arg(static_cast<int>(filtered.size()))
                              .arg(static_cast<int>(cachedResults_.size())));
}

void SearchPage::populateYearFilter(const std::vector<rag::SearchResult>& results) {
    Q_UNUSED(results);
    // 从【全部已导入文档】的元数据收集年份（原先只从最近一次检索结果收集，
    // 导致年份下拉选项残缺且随搜索词变化）
    std::set<QString> years;
    for (const auto& id : retriever_->allDocIds()) {
        const auto* meta = retriever_->getMetadata(id);
        if (meta && meta->date.size() >= 4) {
            const std::string y = meta->date.substr(0, 4);
            // 过滤明显非法的年份（元数据提取异常时的兜底）
            if (y >= "1900" && y <= "2100") {
                years.insert(QString::fromStdString(y));
            }
        }
    }

    // 保留"全部"选项，更新年份列表
    const QString currentYear = yearFilter_->currentText();
    yearFilter_->blockSignals(true);
    yearFilter_->clear();
    yearFilter_->addItem(QStringLiteral("全部"));
    for (const auto& y : years) {
        yearFilter_->addItem(y);
    }
    // 恢复之前的选择
    const int idx = yearFilter_->findText(currentYear);
    if (idx >= 0) yearFilter_->setCurrentIndex(idx);
    yearFilter_->blockSignals(false);
}

std::string SearchPage::buildMetadataSummary(const std::vector<rag::SearchResult>& results) {
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

void SearchPage::appendAiAnswer(const QString& text) {
    aiAnswerArea_->moveCursor(QTextCursor::End);
    aiAnswerArea_->insertPlainText(text);
    aiAnswerArea_->moveCursor(QTextCursor::End);
}
