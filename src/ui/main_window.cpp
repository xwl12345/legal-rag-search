#include "ui/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ui/app_theme.h"
#include "ui/navigation_bar.h"
#include "ui/placeholder_page.h"
#include "ui/search_page.h"
#include "ui/library_page.h"
#include "ui/history_page.h"
#include "ui/quality_page.h"
#include "ui/settings_page.h"
#include "config/app_config.h"
#include "history/history_store.h"
#include "rag/retriever.h"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("法律文档智能检索系统"));
    resize(1180, 720);

    setupStatusBar();
    setupUi();

    // 首次布局完成后再套用主题，确保 QSS 落到所有控件上
    AppTheme::apply(uiScale_);

    // 全局监听滚轮事件，实现 Ctrl+滚轮 缩放界面字体
    QApplication::instance()->installEventFilter(this);

    // 各页已就位，尝试用落盘索引秒级恢复上次的文档库
    restoreIndexOnStartup();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 左侧导航 ──
    navBar_ = new NavigationBar(central);
    root->addWidget(navBar_);

    // ── 中央多页容器 ──
    pageStack_ = new QStackedWidget(central);
    buildPages();
    root->addWidget(pageStack_, 1);

    connect(navBar_, &NavigationBar::currentIndexChanged,
            this, &MainWindow::onNavIndexChanged);

    navBar_->setCurrentIndex(0);
}

void MainWindow::buildPages() {
    // ── 引擎实例：全应用唯一，集中在这里创建 ──
    // 多页共享同一份索引，所以检索页导入的文档，文档库页立刻就看得见；
    // 反过来文档库删掉的文档，检索页也不会再命中。
    retriever_ = std::make_unique<rag::Retriever>();

    // ── 配置中心（T3）：先读参数再建页，引擎带着用户参数起步 ──
    // 文件缺失/损坏时 load 已回落默认值，不会读到 0。
    loadSettingsAndApply();

    // ── 问答历史存储：同样集中在这里创建并打开，历史页只借用指针 ──
    historyStore_ = std::make_unique<history::HistoryStore>();
    if (!historyStore_->open()) {
        // 打不开就如实说：历史页会显示"历史库不可用"（共 0 条也不会误导），
        // 检索问答等其它功能不受影响 —— 不会因为落库失败连累核心链路。
        qWarning("问答历史库打开失败：%s", historyStore_->lastError().c_str());
    }

    // ── 第 1 页：检索问答 ──
    searchPage_ = new SearchPage(retriever_.get(), pageStack_);
    pageStack_->addWidget(searchPage_);
    connect(searchPage_, &SearchPage::engineStatsChanged,
            this, &MainWindow::onEngineStatsChanged);
    connect(searchPage_, &SearchPage::apiKeyStateChanged,
            this, &MainWindow::onApiKeyStateChanged);
    // 一个问答回合结束 → 本窗口负责落库（两页互不认识）
    connect(searchPage_, &SearchPage::answerFinished,
            this, &MainWindow::onAnswerRecorded);
    // T3：启动时把配置里的 TopK / temperature 下发给检索页
    searchPage_->applySettings(appSettings_);

    // ── 第 2 页：文档库（T1）──
    // 插件式接入：只注入引擎裸指针，删掉本页只需去掉这两行 + 删页面文件。
    libraryPage_ = new LibraryPage(retriever_.get(), pageStack_);
    pageStack_->addWidget(libraryPage_);
    connect(libraryPage_, &LibraryPage::libraryChanged,
            this, &MainWindow::onLibraryChanged);

    // ── 第 3 页：问答历史（T2）──
    // 同样是插件式接入：只注入存储层指针，本页不知道回答是谁产生的，
    // 检索页也不知道历史页存在。删掉本页 = 去掉这几行 + 删页面文件。
    historyPage_ = new HistoryPage(historyStore_.get(), pageStack_);
    pageStack_->addWidget(historyPage_);

    // ── 第 4 页：检索质量分析（T4）──
    // 维护者看板：对引擎只读调用（searchWithMode），不写索引不发指令。
    // 删掉本页 = 去掉这几行 + 删页面文件，检索功能零影响。
    qualityPage_ = new QualityPage(retriever_.get(), pageStack_);
    pageStack_->addWidget(qualityPage_);

    // ── 第 5 页：设置（T3）──
    // 插件式接入：本页只读写配置层，不碰 Retriever——保存后发 settingsChanged，
    // 由本窗口中转给引擎（热更新）与检索页（检索宽度/温度），两页互不引用。
    settingsPage_ = new SettingsPage(pageStack_);
    pageStack_->addWidget(settingsPage_);
    connect(settingsPage_, &SettingsPage::settingsChanged,
            this, &MainWindow::onSettingsChanged);
}

void MainWindow::restoreIndexOnStartup() {
    if (!retriever_) {
        return;
    }

    // 只有在落盘文件存在时才有必要尝试恢复；否则直接按空库起步，
    // 免得在状态栏留下一条"恢复失败"的噪音。
    if (!retriever_->hasPersistedIndex()) {
        refreshEngineStats();
        return;
    }

    const auto result = retriever_->loadIndex();
    if (result.ok && retriever_->documentCount() > 0) {
        statusEngine_->setText(
            QStringLiteral("检索引擎已恢复 · %1 文档 / %2 块（%3 ms）")
                .arg(retriever_->documentCount())
                .arg(retriever_->chunkCount())
                .arg(retriever_->lastLoadMs()));
        docCount_ = retriever_->documentCount();
        chunkCount_ = retriever_->chunkCount();
    } else {
        // 恢复失败：如实说明，并提示已按空索引启动（不静默吞掉）
        refreshEngineStats();
        statusEngine_->setText(
            statusEngine_->text()
            + QStringLiteral("（索引恢复失败，已按空库启动）"));
    }

    // 各页在构造时可能已按空索引渲染过一遍，此处补一次刷新
    if (libraryPage_) {
        libraryPage_->refresh();
    }
}

void MainWindow::refreshEngineStats() {
    if (!retriever_) {
        docCount_ = 0;
        chunkCount_ = 0;
    } else {
        docCount_ = retriever_->documentCount();
        chunkCount_ = retriever_->chunkCount();
    }
    statusEngine_->setText(QStringLiteral("检索引擎就绪 · %1 文档 / %2 块")
                               .arg(docCount_)
                               .arg(chunkCount_));
}

void MainWindow::setupStatusBar() {
    auto* bar = statusBar();
    bar->setSizeGripEnabled(false);

    auto* dot = new QLabel(QStringLiteral("●"), this);
    dot->setObjectName(QStringLiteral("statusDotOk"));

    statusEngine_ = new QLabel(this);
    statusEmbedding_ = new QLabel(this);
    statusLlm_ = new QLabel(this);

    bar->addWidget(dot);
    bar->addWidget(statusEngine_);
    bar->addWidget(statusEmbedding_);
    bar->addWidget(statusLlm_);

    statusVersion_ = new QLabel(QStringLiteral("v2.0"), this);
    statusVersion_->setObjectName(QStringLiteral("statusVersion"));
    bar->addPermanentWidget(statusVersion_);

    onEngineStatsChanged(0, 0);
    onApiKeyStateChanged(false);
}

// ── 导航切换 ──
void MainWindow::onNavIndexChanged(int index) {
    if (index < 0 || index >= pageStack_->count()) {
        return;
    }
    pageStack_->setCurrentIndex(index);
}

// ── 状态栏汇总 ──
void MainWindow::onEngineStatsChanged(int docCount, int chunkCount) {
    docCount_ = docCount;
    chunkCount_ = chunkCount;
    QString text = QStringLiteral("检索引擎就绪 · %1 文档 / %2 块")
                       .arg(docCount_)
                       .arg(chunkCount_);
    if (retriever_ && retriever_->restoredFromDisk()) {
        text += QStringLiteral("（本次启动自磁盘恢复）");
    }
    statusEngine_->setText(text);
}

// ── 文档库变更（删除 / 清空）──
void MainWindow::onLibraryChanged() {
    // 本页与检索页共用同一份引擎，所以只需把检索页的陈旧缓存作废，
    // 再按引擎真实状态刷新状态栏即可 —— 无需重新导入或重建索引。
    if (searchPage_) {
        searchPage_->invalidateIndexCache();
    }
    refreshEngineStats();
}

// ── 问答回合结束 → 落库（T2）──
// 检索页只发信号、历史页只读列表，"谁把它写进库"由本窗口负责 ——
// 页面之间因此不需要互相 include。
void MainWindow::onAnswerRecorded(const history::HistoryRecord& record) {
    if (!historyStore_ || !historyStore_->isOpen()) {
        return;
    }

    const long long id = historyStore_->append(record);
    if (id <= 0) {
        // 落库失败不弹窗打断用户（回答已经显示在界面上了），
        // 但必须留下可追查的痕迹。
        qWarning("问答历史落库失败：%s", historyStore_->lastError().c_str());
        return;
    }

    if (historyPage_) {
        historyPage_->refresh();
    }
}

// ── T3 配置中心 ──

void MainWindow::loadSettingsAndApply() {
    const bool loaded = config::AppSettings::load(config::SETTINGS_FILE, appSettings_);
    if (!loaded) {
        // 首次运行（文件不存在）或配置被改坏：静默用默认值，不打扰启动流程。
        // AppSettings::load 已保证 out 处于默认值状态，不读到 0。
        qInfo("检索参数配置未加载（首次运行或文件损坏），使用默认值");
    }
    applySettingsToEngine();
}

void MainWindow::applySettingsToEngine() {
    if (!retriever_) {
        return;
    }
    const auto& s = appSettings_;
    // 查询期参数：k1/b/权重，下一次 search 即生效，无需重建索引
    retriever_->setSearchParams(s.k1, s.b, s.bm25Weight, s.vectorWeight);
    // 分块参数：只影响之后导入的文档（设置页与配置头文件均有标注）
    retriever_->setChunkParams(s.chunkSize, s.chunkOverlap);
    // Embedding 服务（T4 消费；此处先转发，向量路未配置 Key 时仍是降级 BM25）
    retriever_->setEmbeddingEndpoint(s.embeddingBaseUrl, s.embeddingModel);
    if (!s.embeddingApiKey.empty()) {
        retriever_->setApiKey(s.embeddingApiKey);
    }
    // 服务状态可能因配置而变（如首次配上 Key），状态栏两行同步刷新
    refreshServiceStatus();
}

void MainWindow::onSettingsChanged(const config::AppSettings& settings) {
    appSettings_ = settings;
    applySettingsToEngine();
    if (searchPage_) {
        searchPage_->applySettings(settings);
    }
    // Embedding 服务可能刚配上 Key，质量页提示行同步
    if (qualityPage_) {
        qualityPage_->refreshServiceHint();
    }
}

// ── 关闭前落盘 ──
void MainWindow::closeEvent(QCloseEvent* event) {
    if (retriever_ && retriever_->documentCount() > 0) {
        const auto result = retriever_->saveIndex();
        if (!result.ok) {
            // 落盘失败不阻断退出，但必须让用户知道下次启动恢复不了
            const auto reply = QMessageBox::warning(
                this,
                QStringLiteral("索引保存失败"),
                QStringLiteral("无法把索引写入磁盘，下次启动需要重新导入文档。\n\n"
                               "技术细节：%1\n\n仍要退出吗？")
                    .arg(QString::fromStdString(result.diagnostic)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (reply != QMessageBox::Yes) {
                event->ignore();
                return;
            }
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::onApiKeyStateChanged(bool ready) {
    // 该信号只携带 LLM（generator）的 Key 状态——T4 起检索页的 Key 只管
    // 生成路，Embedding 的 Key 归设置页管，两条路不能再共用一个 ready。
    apiReady_ = ready;
    refreshServiceStatus();
}

void MainWindow::refreshServiceStatus() {
    if (retriever_ && retriever_->embeddingReady()) {
        statusEmbedding_->setText(QStringLiteral("Embedding：%1@%2")
                                      .arg(QString::fromStdString(retriever_->embeddingModel()),
                                           QString::fromStdString(retriever_->embeddingHost())));
    } else {
        statusEmbedding_->setText(QStringLiteral("Embedding：未配置（自动降级纯 BM25）"));
    }
    statusLlm_->setText(apiReady_
        ? QStringLiteral("LLM：deepseek-chat")
        : QStringLiteral("LLM：未配置"));
}

// ── Ctrl+滚轮 缩放 ──
void MainWindow::applyUiScale() {
    AppTheme::apply(uiScale_);
    setWindowTitle(QStringLiteral("法律文档智能检索系统 — Ctrl+滚轮缩放字体（当前 %1%）")
                       .arg(uiScale_));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            const int dy = wheel->angleDelta().y();
            if (dy != 0) {
                uiScale_ = qBound(60, uiScale_ + (dy > 0 ? 10 : -10), 250);
                applyUiScale();
            }
            return true;  // 已消费：只缩放，不滚动列表
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
