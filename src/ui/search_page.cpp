#include "ui/search_page.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
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

/// 结果倾向下拉项的显示文案（Unknown 显示"全部"以外的"—"，不显示空洞）
QString tendencyFilterText(document::ResultTendency tendency) {
    if (tendency == document::ResultTendency::Unknown) {
        return QStringLiteral("—");
    }
    return QString::fromUtf8(document::resultTendencyLabel(tendency));
}

/// 历史记录里来源片段的预览长度（原则：够回溯即可，不做全文副本）
constexpr int kRecordSnippetChars = 160;

}  // namespace

SearchPage::SearchPage(rag::Retriever* retriever, QWidget* parent)
    : QWidget(parent)
    , retriever_(retriever)
{
    // P2：引擎实例必须由 MainWindow 注入（组合根唯一），自建分支已删除
    Q_ASSERT(retriever_ && "SearchPage 必须注入 Retriever");

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

    // P1：生成中的「停止」按钮（默认隐藏；点击请求引擎线程中断当前生成）
    stopGenBtn_ = new QPushButton(QStringLiteral("■ 停止"), this);
    stopGenBtn_->setObjectName(QStringLiteral("stopGenBtn"));
    setButtonRole(stopGenBtn_, "danger");
    stopGenBtn_->setMinimumHeight(46);
    stopGenBtn_->setVisible(false);

    searchLayout->addWidget(searchInput_, 1);
    searchLayout->addWidget(searchBtn_);
    searchLayout->addWidget(stopGenBtn_);
    root->addLayout(searchLayout);

    // ── API Key 输入行 ──
    auto* apiKeyLayout = new QHBoxLayout();
    apiKeyLayout->setSpacing(10);

    auto* apiKeyLabel = new QLabel(QStringLiteral("LLM API Key（DeepSeek，仅用于 AI 回答）"), this);
    apiKeyLabel->setObjectName(QStringLiteral("fieldLabel"));

    apiKeyInput_ = new QLineEdit(this);
    apiKeyInput_->setObjectName(QStringLiteral("apiKeyInput"));
    apiKeyInput_->setPlaceholderText(QStringLiteral("sk-xxxxxxxxxxxxxxxxxxxxxxxx"));
    apiKeyInput_->setEchoMode(QLineEdit::Password);
    apiKeyInput_->setMinimumHeight(36);

    setApiKeyBtn_ = new QPushButton(QStringLiteral("设置"), this);
    setApiKeyBtn_->setObjectName(QStringLiteral("setApiKeyBtn"));
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
    yearFilter_->setObjectName(QStringLiteral("yearFilter"));
    yearFilter_->addItem(QStringLiteral("全部"));
    yearFilter_->setMinimumHeight(32);

    // 第四维：裁判结果倾向（第 8 类元数据）。
    // 选项顺序与 ResultTendency 枚举一致，靠 itemData 存枚举值比对，
    // 避免拿界面上的中文标签做字符串匹配（改文案即坏筛选）。
    auto* tendencyLabel = new QLabel(QStringLiteral("结果倾向"), this);
    tendencyLabel->setObjectName(QStringLiteral("fieldLabel"));
    tendencyFilter_ = new QComboBox(this);
    tendencyFilter_->setObjectName(QStringLiteral("tendencyFilter"));
    tendencyFilter_->addItem(QStringLiteral("全部"), QVariant::fromValue(-1));
    for (int i = static_cast<int>(document::ResultTendency::Unknown);
         i <= static_cast<int>(document::ResultTendency::Other); ++i) {
        tendencyFilter_->addItem(
            tendencyFilterText(static_cast<document::ResultTendency>(i)),
            QVariant::fromValue(i));
    }
    tendencyFilter_->setMinimumHeight(32);

    // T5：「只看本院认为」——证据效力分级过滤。
    // 勾选后仅保留角色 = 法院认定（经审理查明 / 本院查明 / 本院认为）的块。
    courtOnlyFilter_ = new QCheckBox(QStringLiteral("只看本院认为"), this);
    courtOnlyFilter_->setObjectName(QStringLiteral("courtOnlyFilter"));
    courtOnlyFilter_->setMinimumHeight(32);

    filterLayout->addWidget(filterLabel);
    filterLayout->addSpacing(6);
    filterLayout->addWidget(typeLabel);
    filterLayout->addWidget(caseTypeFilter_);
    filterLayout->addWidget(courtLabel);
    filterLayout->addWidget(courtLevelFilter_);
    filterLayout->addWidget(yearLabel);
    filterLayout->addWidget(yearFilter_);
    filterLayout->addWidget(tendencyLabel);
    filterLayout->addWidget(tendencyFilter_);
    filterLayout->addWidget(courtOnlyFilter_);
    filterLayout->addStretch();
    root->addLayout(filterLayout);

    // ── 工具栏按钮 ──
    auto* toolLayout = new QHBoxLayout();
    toolLayout->setSpacing(10);

    importBtn_ = new QPushButton(QStringLiteral("＋ 导入文档"), this);
    importBtn_->setObjectName(QStringLiteral("importBtn"));
    setButtonRole(importBtn_, "primary");

    clearBtn_ = new QPushButton(QStringLiteral("清空索引"), this);
    clearBtn_->setObjectName(QStringLiteral("clearBtn"));
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
    connect(stopGenBtn_, &QPushButton::clicked, this, &SearchPage::generationCancelRequested);
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
    connect(tendencyFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchPage::onFilterChanged);
    connect(courtOnlyFilter_, &QCheckBox::toggled,
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
    // 状态栏要显示真实文档数，须用 documentCount()（等价于 allDocIds().size()）。
    emit engineStatsChanged(retriever_->documentCount(), retriever_->chunkCount());
}

// ── API Key ──
// ⚠️ 本页的 Key 只管 LLM 生成（生成在引擎线程的 Generator 上执行）。
// Embedding 与 LLM 是两路独立服务，T4 起不再"一个 Key 灌两处"——embedding_ 的
// Key 由 MainWindow 从设置页配置（env DEEPSEEK_API_KEY 兜底）统一下发。
// P1 起 Key 的实际生效点在 EngineWorker：本页存副本 + 发 llmApiKeyChanged，
// 初始 Key 由 MainWindow 接线完成后主动推送一次（构造期信号还没接线，发不得）。
void SearchPage::loadApiKey() {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (key && std::strlen(key) > 0) {
        llmKey_ = QString::fromStdString(key);
        apiKeyInput_->setText(llmKey_);
        updateApiKeyStatus(true, QStringLiteral("● 已从环境变量加载"));
    } else {
        updateApiKeyStatus(false, QStringLiteral("● 未设置"));
    }
    emit apiKeyStateChanged(!llmKey_.isEmpty());
}

void SearchPage::onSetApiKey() {
    const QString key = apiKeyInput_->text().trimmed();
    if (key.isEmpty()) {
        // 清空 API Key（仅 LLM 路；Embedding 归设置页管，此处不碰）
        llmKey_.clear();
        emit llmApiKeyChanged(QString());
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

    llmKey_ = key;
    emit llmApiKeyChanged(key);
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

// ── P0-2 忙碌状态机 ──
// 引擎动作按钮（检索/导入/清空）只在「本页空闲且其他页面也空闲」时可点。
// 跨页互斥由 MainWindow 监听 engineBusyChanged 后回灌 setExternalBusy 实现。
void SearchPage::refreshActionButtons() {
    const bool enabled = !busySelf_ && !busyExternal_;
    searchBtn_->setEnabled(enabled);
    importBtn_->setEnabled(enabled);
    clearBtn_->setEnabled(enabled);
}

SearchPage::OverwriteChoice SearchPage::defaultConfirmOverwrite(
    const QString& docId, const QString& existingPath, const QString& newPath) const {
    // 走到这里的必是「文档库已有同名 docId，但来源路径不同」——
    // 同一路径重复导入（更新场景）已在调用侧静默放行。
    QMessageBox box(QMessageBox::Warning, QStringLiteral("发现同名文档"),
                    QStringLiteral("文档库中已存在同名文档「%1」\n\n"
                                   "  已有来源：%2\n"
                                   "  本次导入：%3\n\n"
                                   "覆盖将删除原文档及其索引，如何处理？")
                        .arg(docId, existingPath.isEmpty() ? QStringLiteral("（未知）") : existingPath,
                             newPath),
                    QMessageBox::NoButton, const_cast<SearchPage*>(this));
    QPushButton* overwriteBtn = box.addButton(QStringLiteral("覆盖"), QMessageBox::YesRole);
    QPushButton* skipBtn = box.addButton(QStringLiteral("跳过该文件"), QMessageBox::NoRole);
    box.addButton(QStringLiteral("取消剩余导入"), QMessageBox::RejectRole);
    box.exec();

    QAbstractButton* clicked = box.clickedButton();
    if (clicked == overwriteBtn) return OverwriteChoice::Overwrite;
    if (clicked == skipBtn) return OverwriteChoice::Skip;
    return OverwriteChoice::CancelAll;
}

// ── 搜索（P1 异步链）──
// UI 线程只做入队与渲染：检索在引擎线程（EngineWorker::search），结果经
// searchFinished 队列化回来；聚合型问题再追加一次宽检索；最后入队生成。
// 从检索到生成结束全程 busySelf_（P0-2），按钮禁用 + 跨页互斥不变。
void SearchPage::onSearch() {
    const QString query = searchInput_->text().trimmed();
    if (query.isEmpty()) return;
    if (busySelf_ || busyExternal_) {
        statusLabel_->setText(QStringLiteral("引擎忙碌中，请等待当前任务完成"));
        return;
    }

    if (retriever_->documentCount() == 0) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先导入文档再搜索！"));
        return;
    }

    beginEngineTask();
    stage_ = SearchStage::MainSearch;
    progressBar_->setVisible(true);
    progressBar_->setRange(0, 0);  // 不确定模式

    aiAnswerArea_->clear();
    aiAnswerArea_->setHtml(QStringLiteral("<i style='color:#64748B'>正在检索...</i>"));

    emit searchRequested(query, searchWidth_);
}

void SearchPage::onSearchFinished(const std::vector<rag::SearchResult>& results,
                                  const QString& query) {
    if (stage_ == SearchStage::Idle) return;   // 迟到的陈旧结果，忽略

    if (stage_ == SearchStage::WideSearch) {
        // ── 聚合模式第二跳（P2：宽检索 + per-doc 去重已下沉引擎层）──
        if (!results.empty()) {
            displayResults(results);
        }

        // 收集全部文档元数据注入 AI（P2：摘要拼装已下沉 Retriever::metadataSummary）
        auto allIds = retriever_->allDocIds();
        std::string allMeta = retriever_->metadataSummary(allIds);
        if (!allMeta.empty()) {
            allMeta += "\n（以上为全部 " + std::to_string(allIds.size())
                     + " 个已导入文档的元数据汇总）\n";
        }
        // 聚合模式来源 = 去重后的宽检索结果（空则沿用主检索的聚焦结果兜底）
        auto sources = std::move(pendingSources_);
        startGeneration(std::move(sources), allMeta);
        return;
    }

    // ── 主检索完成（stage_ == MainSearch）──
    cachedResults_ = results;
    currentQuery_ = query;

    // 填充年份筛选器
    populateYearFilter(results);

    // 应用筛选并显示
    auto filtered = applyFiltersAndDisplay();

    // AI 生成答案（使用筛选后的结果构建上下文）
    if (!llmKey_.isEmpty() && !filtered.empty()) {
        // ── 聚合型问题检测（P2：规则已下沉 Retriever::isAggregateQuery）──
        if (rag::Retriever::isAggregateQuery(query.toStdString())) {
            // 聚合模式：先记下聚焦结果作为兜底来源，再追加一次引擎聚合检索
            pendingSources_ = std::move(filtered);
            stage_ = SearchStage::WideSearch;
            emit aggregateSearchRequested(query, wideSearchWidth_);
            return;
        }

        // ── 聚焦模式：保持原始排序，不做去重 ──
        auto metaSummary = buildMetadataSummary(filtered);
        startGeneration(std::move(filtered), metaSummary);
        return;
    }

    // 无生成路径：与历史行为一致的提示分支
    if (filtered.empty() && !cachedResults_.empty()) {
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
    } else if (llmKey_.isEmpty()) {
        aiAnswerArea_->setHtml(
            QStringLiteral("<p style='color:#B7791F; font-weight:600;'>🔑 未配置 API Key</p>"
                           "<p style='color:#64748B;'>请在上方输入框中填写 LLM API Key（sk- 开头），</p>"
                           "<p style='color:#64748B;'>点击「设置」后即可启用 AI 智能回答功能。</p>"
                           "<p style='color:#94A3B8; font-size:11px;'>获取 Key："
                           "<a href='https://platform.deepseek.com'>platform.deepseek.com</a></p>"));
    }

    pendingSources_ = std::move(filtered);   // 供 finishSearchRound 的状态计数
    finishSearchRound();
}

void SearchPage::startGeneration(std::vector<rag::SearchResult> sources,
                                 const std::string& metaSummary) {
    pendingSources_ = std::move(sources);
    const std::string context = retriever_->buildContext(pendingSources_, 2000);

    aiAnswerArea_->clear();
    aiAnswerArea_->setHtml(
        QStringLiteral("<b style='color:#14213D'>AI 正在生成回答...</b><br><br>"));

    // 本回合的累计状态复位：流式增量与失败提示都算"用户实际看到的内容"，
    // 落库时以它为准（见 T2 任务卡的「带状态保存」决策）。
    answerBuffer_.clear();
    answerInterrupted_ = false;
    answerNote_.clear();

    stage_ = SearchStage::Generating;
    stopGenBtn_->setVisible(true);
    emit generationRequested(currentQuery_,
                             QString::fromStdString(context),
                             QString::fromStdString(metaSummary),
                             temperature_);
}

void SearchPage::onGenerationDelta(const QString& text) {
    // 剥离模型输出中的 Markdown 标记（** 加粗、行首 # 标题），
    // 纯文本区不渲染这些符号
    QString cleaned = text;
    cleaned.remove(QStringLiteral("**"));
    cleaned.replace(QRegularExpression(QStringLiteral("(^|\\n)#{1,6}\\s+")),
                    QStringLiteral("\\1"));
    answerBuffer_ += cleaned;
    aiAnswerArea_->moveCursor(QTextCursor::End);
    aiAnswerArea_->insertPlainText(cleaned);
    aiAnswerArea_->moveCursor(QTextCursor::End);
}

void SearchPage::onGenerationFinished(bool interrupted, const QString& errorText) {
    if (stage_ != SearchStage::Generating) return;

    stopGenBtn_->setVisible(false);
    stage_ = SearchStage::Idle;

    if (interrupted) {
        const std::string errStr = errorText.toStdString();
        // 生成异常终止：已有 content 仍要留痕，但要标清楚"这不是完整回答"
        answerInterrupted_ = true;
        if (errStr.find("no response") != std::string::npos ||
            errStr.find("timeout") != std::string::npos ||
            errStr.find("connection") != std::string::npos) {
            answerNote_ = QStringLiteral("网络连接异常或超时，回答未完成");
            appendAiAnswer(
                QStringLiteral("\n\n✕ 无法连接到 DeepSeek API\n\n"
                               "可能原因：\n"
                               "• 网络连接异常，请检查是否能访问 api.deepseek.com\n"
                               "• API Key 无效或已过期\n"
                               "• 请求超时，请稍后重试\n\n"
                               "技术细节：") + errorText);
        } else {
            answerNote_ = QStringLiteral("AI 生成失败，回答未完成");
            appendAiAnswer(QStringLiteral("\n\n✕ AI 生成失败：") + errorText);
        }
    }

    // 本回合结束 → 是否落库由 finishAnswerRound 判定并广播；
    // 本页只发信号，不碰任何存储层（页面解耦第 8 条）。
    finishAnswerRound(currentQuery_, pendingSources_);
    finishSearchRound();
}

void SearchPage::finishSearchRound() {
    progressBar_->setVisible(false);
    endEngineTask();
    statusLabel_->setText(QStringLiteral("检索完成，找到 %1 条结果（筛选后 %2 条）")
                              .arg(static_cast<int>(cachedResults_.size()))
                              .arg(static_cast<int>(pendingSources_.size())));
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
    if (busySelf_ || busyExternal_) {
        statusLabel_->setText(QStringLiteral("引擎忙碌中，请等待当前任务完成"));
        return;
    }

    // ── P0-7 同名冲突预检（在进度对话框出现前问清，避免模态叠模态）──
    // docId = 文件名；同名但来源路径不同的文件若直接导入会静默覆盖原文档。
    // 同一路径重复导入（更新场景）静默放行。
    QStringList accepted;
    int skipped = 0;
    for (const QString& file : files) {
        const QString docId = QFileInfo(file).fileName();
        rag::DocumentInfo existing;
        const bool known = retriever_->getDocumentInfo(docId.toStdString(), existing);
        const bool sameSource = known
            && !existing.sourcePath.empty()
            && QFileInfo(QString::fromStdString(existing.sourcePath)).canonicalFilePath()
                   == QFileInfo(file).canonicalFilePath();
        if (known && !sameSource) {
            const OverwriteChoice choice = confirmOverwrite_
                ? confirmOverwrite_(docId,
                                    QString::fromStdString(existing.sourcePath),
                                    QFileInfo(file).absoluteFilePath())
                : defaultConfirmOverwrite(docId,
                                          QString::fromStdString(existing.sourcePath),
                                          QFileInfo(file).absoluteFilePath());
            if (choice == OverwriteChoice::Skip) {
                ++skipped;
                continue;
            }
            if (choice == OverwriteChoice::CancelAll) {
                statusLabel_->setText(
                    QStringLiteral("已取消导入（同名文档「%1」，未做任何更改）").arg(docId));
                return;
            }
        }
        accepted.append(file);
    }

    if (accepted.isEmpty()) {
        statusLabel_->setText(
            QStringLiteral("未导入任何文件（%1 个同名文档被跳过）").arg(skipped));
        return;
    }

    // 二次检查：冲突确认弹窗泵事件期间，用户可能已发起检索——此时放弃导入
    if (busySelf_ || busyExternal_) {
        statusLabel_->setText(QStringLiteral("引擎忙碌中，导入已取消"));
        return;
    }

    beginEngineTask();
    importSkipped_ = skipped;
    progressBar_->setVisible(true);
    progressBar_->setRange(0, accepted.size());
    statusLabel_->setText(
        QStringLiteral("正在导入文档...（扫描件 OCR 逐页识别，可能需要数分钟，请耐心等待）"));

    // ── P1：导入在引擎线程执行，UI 保持可交互 ──
    // 进度对话框改为非模态（旧实现的 WindowModal 是为同步执行堵重入的；
    // 现在忙碌状态机 + 按钮禁用已经挡住了其他引擎动作，无需再锁整个窗口）。
    // 取消经原子令牌传递给引擎线程（跨线程不能直接读对话框状态）。
    importCancel_ = std::make_shared<std::atomic_bool>(false);
    importProgress_ = std::make_unique<QProgressDialog>(
        QStringLiteral("准备导入…"), QStringLiteral("取消"), 0, 0, this);
    importProgress_->setWindowTitle(QStringLiteral("导入文档"));
    importProgress_->setWindowModality(Qt::NonModal);
    importProgress_->setMinimumDuration(0);
    importProgress_->show();
    connect(importProgress_.get(), &QProgressDialog::canceled, this, [this]() {
        if (importCancel_) {
            importCancel_->store(true);
        }
    });

    emit importRequested(accepted);
}

void SearchPage::onImportProgress(int done, int total, const QString& fileName) {
    if (importProgress_) {
        importProgress_->setLabelText(
            QStringLiteral("正在导入（%1 / %2）：\n%3").arg(done).arg(total).arg(fileName));
    }
    progressBar_->setValue(done);
}

void SearchPage::onImportOcrPage(int page, int total) {
    if (importProgress_) {
        importProgress_->setLabelText(
            QStringLiteral("OCR 逐页识别中：第 %1 / %2 页（每页约 5-10 秒）…")
                .arg(page).arg(total));
    }
}

void SearchPage::onImportFinished(const ui_engine::ImportSummary& summary) {
    importProgress_->reset();
    importProgress_.reset();
    importCancel_.reset();

    progressBar_->setVisible(false);
    endEngineTask();
    const int skipped = importSkipped_;
    importSkipped_ = 0;

    if (summary.userCancelled) {
        QString message = QStringLiteral("已取消导入：成功 %1 个文档，新增 %2 个文本块")
                              .arg(summary.imported).arg(summary.chunksAdded);
        if (summary.imported == 0) {
            // 说明"为什么是 0"：扫描件按整篇入库，识别中途取消 = 整份未入库
            message += QStringLiteral("。OCR 需整份文档全部页识别完成后才会建立索引，"
                                      "取消的文件不会入库，已识别的页不保留，"
                                      "重新导入时将从第一页重新识别");
        }
        if (summary.remaining > 0) {
            message += QStringLiteral("；剩余 %1 个文件未处理").arg(summary.remaining);
        }
        statusLabel_->setText(message);
    } else if (summary.errors.isEmpty()) {
        QString message = QStringLiteral("✓ 已导入 %1 个文档，新增 %2 个文本块")
                              .arg(summary.imported)
                              .arg(summary.chunksAdded);
        if (summary.ocrImported > 0) {
            message += QStringLiteral("（其中 %1 个通过 OCR 识别）").arg(summary.ocrImported);
        }
        if (skipped > 0) {
            message += QStringLiteral("；同名跳过 %1 个").arg(skipped);
        }
        statusLabel_->setText(message);
    } else {
        const QString text = summary.imported > 0
            ? QStringLiteral("⚠ 导入完成：%1 个成功，%2 个失败，新增 %3 个文本块")
                  .arg(summary.imported).arg(summary.errors.size()).arg(summary.chunksAdded)
            : QStringLiteral("⚠ 未导入任何文档：%1 个文件未能提取可检索文本")
                  .arg(summary.errors.size());
        statusLabel_->setText(text);
        QMessageBox::warning(this, QStringLiteral("文档导入提示"),
                             text + QStringLiteral("\n\n失败原因：\n")
                                 + summary.errors.join(QStringLiteral("\n")));
    }

    emitEngineStats();
}

// ── 清空索引 ──
void SearchPage::onClearIndex() {
    if (busySelf_ || busyExternal_) {
        statusLabel_->setText(QStringLiteral("引擎忙碌中，请等待当前任务完成"));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("确认清空"),
        QStringLiteral("确定要清空所有已导入的文档索引吗？此操作不可恢复。\n"
                       "（落盘索引文件也会一并删除）"),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        // 引擎实例由 MainWindow 持有、多页共享，这里只能清内容不能换对象。
        // 同时删除落盘文件，避免下次启动把刚清掉的索引又恢复回来。
        retriever_->clearAll(/*alsoDeletePersistedFile=*/true);

        cachedResults_.clear();
        resultList_->clear();
        aiAnswerArea_->clear();
        statusLabel_->setText(QStringLiteral("索引已清空"));

        emitEngineStats();
    }
}

// ── 索引被外部改动（文档库页删除/清空）──
void SearchPage::invalidateIndexCache() {
    // 缓存里的 SearchResult 对应的文本块可能已被删除，
    // 此时若用户切回本页并改动筛选条件，会渲染出"已删文档的片段"。
    cachedResults_.clear();
    currentQuery_.clear();
    if (resultList_) {
        resultList_->clear();
    }
    if (statusLabel_) {
        statusLabel_->setText(QStringLiteral("文档库已变更，请重新检索"));
    }
}

// ── 显示检索结果 ──
void SearchPage::displayResults(const std::vector<rag::SearchResult>& results) {
    resultList_->clear();

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::ostringstream oss;
        oss << "【" << (i + 1) << "】";
        // T5：结构段角色标签（Unknown 不显示），支撑证据效力分级
        if (r.role != document::ChunkRole::Unknown) {
            oss << "[" << document::chunkRoleLabel(r.role) << "] ";
        }
        oss << r.docId
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

    // 第四维用 itemData 里的枚举值判断，"全部"为 -1
    const int tendencyValue = tendencyFilter_->currentData().toInt();
    const bool tendencyFiltering = (tendencyValue >= 0);
    const auto wantedTendency =
        static_cast<document::ResultTendency>(tendencyValue);

    // T5：「只看本院认为」——仅保留法院认定块（查明事实 + 说理）
    const bool courtOnly = courtOnlyFilter_->isChecked();

    // 无筛选条件，直接返回全部
    if (caseType == QStringLiteral("全部") && courtLevel == QStringLiteral("全部")
        && year == QStringLiteral("全部") && !tendencyFiltering && !courtOnly) {
        return cachedResults_;
    }

    std::vector<rag::SearchResult> filtered;
    for (const auto& r : cachedResults_) {
        const auto meta = retriever_->metadataOf(r.docId);

        // T5：角色过滤（位标志：跨段块同时归属多段，按位匹配不丢内容）
        if (courtOnly
            && (static_cast<int>(r.role)
                & static_cast<int>(document::ChunkRole::CourtOpinion)) == 0) {
            continue;
        }

        // 案件类型筛选
        if (caseType != QStringLiteral("全部")) {
            if (!meta || meta->caseType != caseType.toStdString()) {
                continue;
            }
        }

        // 法院级别筛选（P2：层级判定下沉 document::courtLevelOf，含单测）
        if (courtLevel != QStringLiteral("全部")) {
            const auto level = document::courtLevelOf(meta ? *meta : document::DocMetadata{});
            const bool match =
                (courtLevel == QStringLiteral("最高人民法院") && level == document::CourtLevel::Supreme) ||
                (courtLevel == QStringLiteral("高级人民法院") && level == document::CourtLevel::High) ||
                (courtLevel == QStringLiteral("中级人民法院") && level == document::CourtLevel::Intermediate) ||
                (courtLevel == QStringLiteral("基层人民法院") && level == document::CourtLevel::Basic);
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

        // 结果倾向筛选（第 8 类）：取该文档的判定值精确比对。
        // 选"—"即筛 Unknown（未能判定主文段的文书），便于人工复核。
        if (tendencyFiltering) {
            const auto docTendency = meta ? meta->tendency
                                          : document::ResultTendency::Unknown;
            if (docTendency != wantedTendency) {
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
        const auto meta = retriever_->metadataOf(id);
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
    // P2：字段拼装下沉 Retriever::metadataSummary，这里只按命中顺序收集 docId（去重）
    std::vector<std::string> orderedIds;
    std::set<std::string> seenIds;
    for (const auto& r : results) {
        if (seenIds.insert(r.docId).second) {
            orderedIds.push_back(r.docId);
        }
    }
    return retriever_->metadataSummary(orderedIds);
}

void SearchPage::appendAiAnswer(const QString& text) {
    // 界面上显示什么，历史里就记什么 —— 失败提示语也要留痕，
    // 否则复盘时看不到"这次为什么没答出来"。
    answerBuffer_ += text;
    aiAnswerArea_->moveCursor(QTextCursor::End);
    aiAnswerArea_->insertPlainText(text);
    aiAnswerArea_->moveCursor(QTextCursor::End);
}

history::HistoryRecord SearchPage::buildHistoryRecord(
    const QString& query,
    const std::vector<rag::SearchResult>& sources,
    const QString& answer,
    bool interrupted,
    const QString& note) const
{
    history::HistoryRecord record;
    record.createdAt = QDateTime::currentDateTime()
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                           .toStdString();
    record.query = query.toStdString();
    record.answer = answer.toStdString();
    record.interrupted = interrupted;
    record.note = note.toStdString();

    record.sources.reserve(sources.size());
    for (const auto& hit : sources) {
        history::SourceItem item;
        item.docId = hit.docId;
        item.chunkIndex = hit.chunkIndex;
        item.finalScore = hit.finalScore;

        QString snippet = QString::fromStdString(hit.content);
        snippet.replace(QLatin1Char('\n'), QLatin1Char(' '));
        snippet.replace(QLatin1Char('\r'), QLatin1Char(' '));
        if (snippet.size() > kRecordSnippetChars) {
            snippet = snippet.left(kRecordSnippetChars) + QStringLiteral("…");
        }
        item.snippet = snippet.toStdString();
        record.sources.push_back(std::move(item));
    }
    return record;
}

void SearchPage::applySettings(const config::AppSettings& settings) {
    // 检索宽度与聚合宽检索按 2.5 倍联动：默认 20 → 50，与历史行为一致
    searchWidth_ = std::max(1, settings.topK);
    wideSearchWidth_ = std::max(searchWidth_, searchWidth_ * 5 / 2);
    temperature_ = settings.temperature;   // P1：随生成请求下发引擎线程的 Generator
}

void SearchPage::finishAnswerRound(const QString& query,
                                   const std::vector<rag::SearchResult>& sources) {
    // 空内容回合不入库：一个字都没吐出来的记录不含任何信息，
    // 塞进历史只会稀释真正有价值的记录 —— 这条规则写进 T2 任务卡的决策点。
    if (answerBuffer_.trimmed().isEmpty()) {
        return;
    }
    emit answerFinished(
        buildHistoryRecord(query, sources, answerBuffer_, answerInterrupted_, answerNote_));
}
