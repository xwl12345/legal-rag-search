#include "ui/main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ui/app_theme.h"
#include "ui/engine_worker.h"
#include "ui/navigation_bar.h"
#include "ui/placeholder_page.h"
#include "ui/search_page.h"
#include "ui/library_page.h"
#include "ui/history_page.h"
#include "ui/quality_page.h"
#include "ui/favorites_page.h"
#include "ui/law_page.h"
#include "ui/settings_page.h"
#include "ui/status_page.h"
#include "config/app_config.h"
#include "history/history_store.h"
#include "law/law_store.h"
#include "favorite/favorite_store.h"
#include "rag/retriever.h"

#include <cstdlib>  // std::getenv（E2E 索引隔离通道）

#include <QThread>

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

MainWindow::~MainWindow() {
    // P1 退出安全：先停引擎线程（忙碌门已保证此时无进行中任务），
    // 再走基类析构销毁页面树——杜绝"页面已析构而引擎任务还在跑"的悬挂。
    if (engineThread_) {
        engineThread_->quit();
        engineThread_->wait(5000);
    }
}

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
    // ── 引擎线程与 worker（P1）：重操作全部队列化到引擎线程串行执行 ──
    // Retriever 在 EngineWorker 构造时创建（UI 线程，词典加载与旧行为一致），
    // 页面拿到的是它的轻只读指针；worker 无 parent（跨线程对象不挂窗口树）。
    engineWorker_ = new EngineWorker();
    engineThread_ = new QThread(this);
    engineWorker_->moveToThread(engineThread_);
    connect(engineThread_, &QThread::finished, engineWorker_, &QObject::deleteLater);
    engineThread_->start();
    retriever_ = engineWorker_->retriever();

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

    // ── 法条速查（T9）：法条库只读打开 + 独立第二语料库检索实例 ──
    // 打不开（库文件缺失/损坏）时法条页如实显示不可用，不影响其它页面。
    lawStore_ = std::make_unique<law::LawStore>();
    if (lawStore_->open()) {
        lawRetriever_ = std::make_unique<rag::Retriever>();
        // docId = 法律名 + "#" + 条号；法条页按此映射回 LawStore 取全文
        for (const auto& article : lawStore_->allArticles()) {
            lawRetriever_->addText(
                article.text,
                article.lawName + "#" + article.articleNo);
        }
        qInfo("法条库已加载：%d 部 / %d 条（关键词检索实例就绪）",
              lawStore_->lawCount(), lawStore_->articleCount());
    } else {
        qWarning("法条库打开失败：%s（法条速查页将显示不可用）",
                 lawStore_->lastError().c_str());
    }

    // ── 案例收藏库（T11）：集中创建并打开，收藏页与检索页只借用指针 ──
    // 打不开（文件损坏/被占用）时收藏页如实显示不可用，其它功能不受影响。
    favoriteStore_ = std::make_unique<favorite::FavoriteStore>();
    if (!favoriteStore_->open()) {
        qWarning("案例收藏库打开失败：%s（案例收藏页将显示不可用）",
                 favoriteStore_->lastError().c_str());
    }

    // ── 第 1 页：检索问答 ──
    searchPage_ = new SearchPage(retriever_, favoriteStore_.get(), pageStack_);
    pageStack_->addWidget(searchPage_);
    connect(searchPage_, &SearchPage::engineStatsChanged,
            this, &MainWindow::onEngineStatsChanged);
    connect(searchPage_, &SearchPage::apiKeyStateChanged,
            this, &MainWindow::onApiKeyStateChanged);
    // 一个问答回合结束 → 本窗口负责落库（两页互不认识）
    connect(searchPage_, &SearchPage::answerFinished,
            this, &MainWindow::onAnswerRecorded);
    // T11：检索页写入收藏 → 本窗口中转刷新收藏页（两页互不引用）
    connect(searchPage_, &SearchPage::favoriteChanged,
            this, &MainWindow::onFavoriteChanged);
    // T3：启动时把配置里的 TopK / temperature 下发给检索页
    searchPage_->applySettings(appSettings_);

    // ── P1：页面请求 → 引擎线程（队列化）；引擎结果 → 页面（队列化）──
    connect(searchPage_, &SearchPage::searchRequested,
            engineWorker_, &EngineWorker::search);
    connect(searchPage_, &SearchPage::aggregateSearchRequested,
            engineWorker_, &EngineWorker::searchAggregate);
    connect(searchPage_, &SearchPage::generationRequested,
            engineWorker_, &EngineWorker::generateAnswer);
    connect(searchPage_, &SearchPage::generationCancelRequested,
            engineWorker_, &EngineWorker::cancelGeneration);
    connect(searchPage_, &SearchPage::importRequested,
            engineWorker_, &EngineWorker::importDocuments);
    connect(searchPage_, &SearchPage::llmApiKeyChanged,
            engineWorker_, &EngineWorker::setLlmApiKey);
    connect(engineWorker_, &EngineWorker::searchFinished,
            searchPage_, &SearchPage::onSearchFinished);
    connect(engineWorker_, &EngineWorker::generationDelta,
            searchPage_, &SearchPage::onGenerationDelta);
    connect(engineWorker_, &EngineWorker::generationFinished,
            searchPage_, &SearchPage::onGenerationFinished);
    connect(engineWorker_, &EngineWorker::importProgress,
            searchPage_, &SearchPage::onImportProgress);
    connect(engineWorker_, &EngineWorker::importOcrPage,
            searchPage_, &SearchPage::onImportOcrPage);
    connect(engineWorker_, &EngineWorker::importFinished,
            searchPage_, &SearchPage::onImportFinished);

    // ── 第 2 页：文档库（T1）──
    // 插件式接入：只注入引擎裸指针，删掉本页只需去掉这两行 + 删页面文件。
    // T11：另注入收藏库指针——删除文档后联动标记收藏失效（本页内完成并提示）。
    libraryPage_ = new LibraryPage(retriever_, favoriteStore_.get(), pageStack_);
    pageStack_->addWidget(libraryPage_);
    connect(libraryPage_, &LibraryPage::libraryChanged,
            this, &MainWindow::onLibraryChanged);
    // T13 知识库管理：切换/另存/删除请求经 MainWindow 中转（页面不碰引擎线程）
    connect(libraryPage_, &LibraryPage::kbSwitchRequested,
            this, &MainWindow::switchKb);
    connect(libraryPage_, &LibraryPage::kbSaveAsRequested,
            this, &MainWindow::onKbSaveAsRequested);
    connect(libraryPage_, &LibraryPage::kbDeleteRequested,
            this, &MainWindow::onKbDeleteRequested);

    // ── 第 3 页：问答历史（T2）──
    // 同样是插件式接入：只注入存储层指针，本页不知道回答是谁产生的，
    // 检索页也不知道历史页存在。删掉本页 = 去掉这几行 + 删页面文件。
    historyPage_ = new HistoryPage(historyStore_.get(), pageStack_);
    pageStack_->addWidget(historyPage_);

    // ── 第 4 页：检索质量分析（T4）──
    // 维护者看板：对引擎只读调用（searchWithMode），不写索引不发指令。
    // 删掉本页 = 去掉这几行 + 删页面文件，检索功能零影响。
    qualityPage_ = new QualityPage(retriever_, pageStack_);
    pageStack_->addWidget(qualityPage_);
    connect(qualityPage_, &QualityPage::compareRequested,
            engineWorker_, &EngineWorker::compareModes);
    connect(qualityPage_, &QualityPage::evalRequested,
            engineWorker_, &EngineWorker::runBatchEval);
    connect(engineWorker_, &EngineWorker::compareFinished,
            qualityPage_, &QualityPage::onCompareFinished);
    connect(engineWorker_, &EngineWorker::evalProgress,
            qualityPage_, &QualityPage::onEvalProgress);
    connect(engineWorker_, &EngineWorker::evalRow,
            qualityPage_, &QualityPage::onEvalRow);
    connect(engineWorker_, &EngineWorker::evalFinished,
            qualityPage_, &QualityPage::onEvalFinished);

    // ── 第 5 页：案例收藏（T11；T10 起另注入主引擎指针供「打开全文」只读取全文）──
    // 插件式接入：只注入收藏库 + 引擎指针，本页不知道收藏从哪来（检索页写入），
    // 也不知道文档被谁删掉（文档库页标记失效）——刷新统一由本窗口中转。
    // 删掉本页 = 去掉这几行 + 删页面文件，检索与文档库功能零影响。
    favoritesPage_ = new FavoritesPage(favoriteStore_.get(), retriever_, pageStack_);
    pageStack_->addWidget(favoritesPage_);

    // ── 第 6 页：法条速查（T9）──
    // 插件式接入：注入法条库指针 + 独立法条检索实例指针，两者都只读。
    // 法条检索实例与主引擎物理隔离，本页不经过 EngineWorker（语料小，
    // 同步调用毫秒级）；删掉本页 = 去掉这几行 + 删页面文件。
    lawPage_ = new LawPage(lawStore_.get(), lawRetriever_.get(), pageStack_);
    pageStack_->addWidget(lawPage_);

    // ── 第 7 页：运行状态（T8）──
    // 插件式接入：本页不持引擎——统计/服务状态由组合根经 refreshStatusPage
    // 注入，操作流水由本窗口在各信号点 appendRunLog 后全量下发快照。
    statusPage_ = new StatusPage(pageStack_);
    pageStack_->addWidget(statusPage_);
    connect(statusPage_, &StatusPage::clearLogRequested,
            this, &MainWindow::onClearRunLog);

    // ── 第 8 页：设置（T3）──
    // 插件式接入：本页只读写配置层，不碰 Retriever——保存后发 settingsChanged，
    // 由本窗口中转给引擎（热更新）与检索页（检索宽度/温度），两页互不引用。
    settingsPage_ = new SettingsPage(appSettings_, pageStack_);   // P3：注入启动配置
    pageStack_->addWidget(settingsPage_);
    connect(settingsPage_, &SettingsPage::settingsChanged,
            this, &MainWindow::onSettingsChanged);
    // T7：同义词表热更新（设置页保存成功后队列化下发引擎）
    connect(settingsPage_, &SettingsPage::synonymsChanged,
            this, &MainWindow::onSynonymsChanged);

    // T7 启动加载：synonyms.json（缺文件/损坏 → 引擎保持构造时的内置默认映射），
    // 无论加载成功与否都回填表格——表格始终反映引擎当前生效的映射。
    {
        rag::synonym::SynonymMap synonyms;
        if (!rag::synonym::load(config::dataFilePath(config::SYNONYMS_FILE), synonyms)) {
            synonyms = rag::synonym::SynonymMap::defaults();   // 引擎构造时已用 defaults，此处对齐回填
        }
        settingsPage_->populateSynonyms(synonyms);
        onSynonymsChanged(std::move(synonyms));   // 与引擎构造值对齐（幂等）
    }

    // 引擎配置应用完成后刷新状态栏服务区（Embedding 可能刚配上 Key）
    connect(engineWorker_, &EngineWorker::settingsApplied,
            this, &MainWindow::refreshServiceStatus);

    // ── T13 多知识库：切换/另存结果回传（写盘与加载在引擎线程）──
    connect(engineWorker_, &EngineWorker::indexSwitched,
            this, &MainWindow::onIndexSwitched);
    connect(engineWorker_, &EngineWorker::indexSaved,
            this, &MainWindow::onIndexSaved);

    // ── P0-2 跨页忙碌互斥：任一页面开始引擎任务 → 其余页面禁用引擎动作 ──
    // 页面之间零互相引用，广播经本窗口中转（与 answerFinished 同款模式）。
    connect(searchPage_, &SearchPage::engineBusyChanged,
            this, &MainWindow::forwardEngineBusy);
    connect(libraryPage_, &LibraryPage::engineBusyChanged,
            this, &MainWindow::forwardEngineBusy);
    connect(qualityPage_, &QualityPage::engineBusyChanged,
            this, &MainWindow::forwardEngineBusy);

    // 初始 LLM Key 推送（队列化：与引擎线程上的一切访问串行）
    QMetaObject::invokeMethod(engineWorker_, "setLlmApiKey", Qt::QueuedConnection,
                              Q_ARG(QString, searchPage_->llmApiKey()));
}

void MainWindow::restoreIndexOnStartup() {
    if (!retriever_) {
        return;
    }

    // E2E 隔离通道（T10.1 期修复，案例 C-9）：ui_smoke 设置此环境变量后，
    // 全部窗口（共享 + 各分组自建）的索引恢复/落盘都走隔离文件——
    // ① 套件不再吃到启动时的残留或用户真实索引（导入组曾因此 21+1=22 假失败）；
    // ② 套件 closeEvent 不再把测试语料写回用户真实 rag_index.dat
    //（此前每次跑 E2E 都会覆盖用户索引，2026-10-20 实测暴露）。
    // 普通用户不会设置该变量，默认路径行为完全不变。
    if (const char* isolated = std::getenv("RAG_SMOKE_INDEX_FILE")) {
        if (isolated[0] != '\0') {
            retriever_->setIndexFilePath(isolated);
        }
    } else if (!appSettings_.currentIndexFile.empty()) {
        // T13 多知识库：恢复上次使用的库（相对 exe 目录）。
        // 优先级最低：E2E 隔离通道 > 该记忆 > 默认主库（老用户零感知）。
        const std::string remembered =
            config::dataFilePath(appSettings_.currentIndexFile.c_str());
        if (QFile::exists(QString::fromStdString(remembered))) {
            retriever_->setIndexFilePath(remembered);
            currentKbRel_ = QString::fromStdString(appSettings_.currentIndexFile);
            if (libraryPage_) {
                libraryPage_->setCurrentKb(currentKbRel_);   // 下拉选中记忆库
            }
        }
        // 文件不存在（被手删/换机器）：静默回落主库，状态栏如实显示
    }

    // 只有在落盘文件存在时才有必要尝试恢复；否则直接按空库起步，
    // 免得在状态栏留下一条"恢复失败"的噪音。
    if (!retriever_->hasPersistedIndex()) {
        refreshEngineStats();
        appendRunLog(QStringLiteral("信息"),
                     QStringLiteral("无落盘索引，按空库启动"));
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
        appendRunLog(QStringLiteral("信息"),
                     QStringLiteral("索引已从落盘恢复：%1 篇 / %2 块（%3 ms）")
                         .arg(docCount_)
                         .arg(chunkCount_)
                         .arg(retriever_->lastLoadMs()));
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

    statusDot_ = new QLabel(QStringLiteral("●"), this);
    // 初始灰点；实际颜色由 refreshServiceStatus() 按服务真实状态决定（P0-8，
    // 旧实现写死 statusDotOk，QSS 里的 statusDotOff 是永不生效的死规则）

    statusEngine_ = new QLabel(this);
    statusEmbedding_ = new QLabel(this);
    statusLlm_ = new QLabel(this);

    bar->addWidget(statusDot_);
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
    // T8：切到「运行状态」页时按需刷新（导航第 7 项），保证看到最新快照
    if (index == pageStack_->indexOf(statusPage_)) {
        refreshStatusPage();
        statusPage_->refreshLog(runLog_.entries());
    }
}

// ── T8 运行状态页 ──

void MainWindow::appendRunLog(const QString& level, const QString& message) {
    runLog_.append(level, message);
    if (statusPage_) {
        statusPage_->refreshLog(runLog_.entries());
    }
}

void MainWindow::refreshStatusPage() {
    if (!statusPage_) {
        return;
    }
    StatusPage::Stats stats;
    stats.documents = docCount_;
    stats.chunks = chunkCount_;
    stats.historyCount = historyStore_ ? historyStore_->count() : 0;
    stats.favoriteCount = favoriteStore_ ? favoriteStore_->caseCount() : 0;
    stats.indexLoadMs = retriever_ ? retriever_->lastLoadMs() : -1;
    stats.configLine = QStringLiteral(
        "BM25 k1=%1 / b=%2，融合权重 %3（BM25）: %4（向量），TopK=%5")
                           .arg(appSettings_.k1)
                           .arg(appSettings_.b)
                           .arg(appSettings_.bm25Weight)
                           .arg(appSettings_.vectorWeight)
                           .arg(appSettings_.topK);
    stats.embeddingStatus = statusEmbedding_ ? statusEmbedding_->text()
                                             : QStringLiteral("未知");
    stats.llmStatus = statusLlm_ ? statusLlm_->text() : QStringLiteral("未知");
    statusPage_->refreshStats(stats);
}

void MainWindow::onClearRunLog() {
    runLog_.clear();
    if (statusPage_) {
        statusPage_->refreshLog(runLog_.entries());
    }
    appendRunLog(QStringLiteral("信息"), QStringLiteral("操作流水已清空"));
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
    appendRunLog(QStringLiteral("信息"),
                 QStringLiteral("文档库更新：%1 篇 / %2 块").arg(docCount_).arg(chunkCount_));
}

// ── 文档库变更（删除 / 清空）──
void MainWindow::onLibraryChanged() {
    // 本页与检索页共用同一份引擎，所以只需把检索页的陈旧缓存作废，
    // 再按引擎真实状态刷新状态栏即可 —— 无需重新导入或重建索引。
    if (searchPage_) {
        searchPage_->invalidateIndexCache();
    }
    // T11：文档增删会影响收藏项的有效性展示（删除已在文档库页标记失效），
    // 这里统一刷新一次收藏页，让状态列与计数保持最新。
    if (favoritesPage_) {
        favoritesPage_->refresh();
    }
    refreshEngineStats();
}

// ── T11：检索页写入收藏 → 刷新收藏页 ──
// 检索页只发信号、收藏页只读存储层，"谁触发刷新"由本窗口负责 ——
// 与 answerFinished → onAnswerRecorded 同款模式，两页因此不需要互相 include。
void MainWindow::onFavoriteChanged() {
    if (favoritesPage_) {
        favoritesPage_->refresh();
    }
    appendRunLog(QStringLiteral("信息"), QStringLiteral("案例收藏集变更"));
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
    appendRunLog(QStringLiteral("信息"),
                 QStringLiteral("新问答记录 #%1：%2")
                     .arg(id)
                     .arg(QString::fromStdString(record.query).left(24)));
}

// ── T3 配置中心 ──

void MainWindow::loadSettingsAndApply() {
    const bool loaded = config::AppSettings::load(config::dataFilePath(config::SETTINGS_FILE), appSettings_);
    if (!loaded) {
        // 首次运行（文件不存在）或配置被改坏：静默用默认值，不打扰启动流程。
        // AppSettings::load 已保证 out 处于默认值状态，不读到 0。
        qInfo("检索参数配置未加载（首次运行或文件损坏），使用默认值");
    }
    applySettingsToEngine();
}

void MainWindow::applySettingsToEngine() {
    if (!engineWorker_) {
        return;
    }
    // P1：配置热更新队列化到引擎线程——setSearchParams/Endpoint/ApiKey 都会
    // 触碰引擎线程正在使用的状态，必须与其串行（写入方与读方同线程）。
    QMetaObject::invokeMethod(engineWorker_, "applySettings", Qt::QueuedConnection,
                              Q_ARG(config::AppSettings, appSettings_));
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
    appendRunLog(QStringLiteral("信息"),
                 QStringLiteral("检索配置保存并生效（k1=%1，TopK=%2）")
                     .arg(settings.k1)
                     .arg(settings.topK));
    refreshStatusPage();
}

// ── T7 同义词归一化 ──

void MainWindow::onSynonymsChanged(rag::synonym::SynonymMap map) {
    if (!engineWorker_) {
        return;
    }
    // P1：与配置热更新同款——队列化到引擎线程，与检索任务串行
    QMetaObject::invokeMethod(engineWorker_, "setSynonymMap", Qt::QueuedConnection,
                              Q_ARG(rag::synonym::SynonymMap, map));
    appendRunLog(QStringLiteral("信息"),
                 QStringLiteral("同义词映射更新（%1 条），下一次检索生效")
                     .arg(static_cast<int>(map.entries.size())));
}

// ── T13 多知识库 ──

void MainWindow::switchKb(const QString& relPath) {
    if (!engineWorker_ || engineBusy()) {
        return;   // 引擎任务进行中拒绝切换（按钮侧已禁用，这里兜底）
    }
    pendingKbRel_ = relPath;
    const QString absPath = relPath.isEmpty()
        ? QString::fromStdString(config::dataFilePath(config::INDEX_FILE))
        : QString::fromStdString(config::dataFilePath(
              relPath.toStdString().c_str()));
    QMetaObject::invokeMethod(engineWorker_, "switchIndex", Qt::QueuedConnection,
                              Q_ARG(QString, absPath));
}

void MainWindow::onIndexSwitched(int docCount, int chunkCount, qint64 loadMs,
                                 const QString& path) {
    if (docCount <= 0 && chunkCount <= 0) {
        // 加载失败：回滚意图，状态栏与流水如实提示（引擎内部索引已清空，
        // 伪装成功只会让后续检索静默空转——如实报失败引导用户重切）
        pendingKbRel_.clear();
        statusEngine_->setText(QStringLiteral("知识库切换失败（见操作流水）"));
        appendRunLog(QStringLiteral("警告"),
                     QStringLiteral("知识库切换失败：%1").arg(path));
        return;
    }
    currentKbRel_ = pendingKbRel_;
    pendingKbRel_.clear();
    docCount_ = docCount;
    chunkCount_ = chunkCount;
    statusEngine_->setText(
        QStringLiteral("知识库已切换 · %1 文档 / %2 块（%3 ms）")
            .arg(docCount).arg(chunkCount).arg(loadMs));
    appendRunLog(QStringLiteral("信息"),
                 QStringLiteral("切换知识库：%1（%2 文档 / %3 块）")
                     .arg(currentKbRel_.isEmpty() ? QStringLiteral("默认主库")
                                                  : currentKbRel_)
                     .arg(docCount).arg(chunkCount));
    // 持久化记忆（主库记空串 = 旧行为）；写盘失败不阻断，仅流水提示
    appSettings_.currentIndexFile = currentKbRel_.toStdString();
    std::string error;
    if (!config::AppSettings::save(
            config::dataFilePath(config::SETTINGS_FILE), appSettings_, &error)) {
        appendRunLog(QStringLiteral("警告"),
                     QStringLiteral("知识库记忆写盘失败：%1")
                         .arg(QString::fromStdString(error)));
    }
    // 联动刷新：文档库列表（裸指针直读 retriever，内容已换）、状态页统计
    if (libraryPage_) {
        libraryPage_->setCurrentKb(currentKbRel_);   // 先同步当前库再刷新
        libraryPage_->refresh();
    }
    refreshStatusPage();
}

void MainWindow::onIndexSaved(bool ok, const QString& path,
                              const QString& diagnostic) {
    if (ok) {
        appendRunLog(QStringLiteral("信息"),
                     QStringLiteral("已另存知识库：%1").arg(path));
        if (libraryPage_) {
            libraryPage_->refresh();   // 另存成功 → 下拉立即出现新库
        }
    } else {
        appendRunLog(QStringLiteral("警告"),
                     QStringLiteral("另存知识库失败：%1（%2）").arg(path, diagnostic));
    }
}

void MainWindow::onKbSaveAsRequested(const QString& relPath) {
    if (!engineWorker_ || engineBusy()) {
        appendRunLog(QStringLiteral("警告"),
                     QStringLiteral("引擎任务进行中，另存请求已忽略"));
        return;
    }
    const QString absPath = QString::fromStdString(
        config::dataFilePath(relPath.toStdString().c_str()));
    QMetaObject::invokeMethod(engineWorker_, "saveIndexAs", Qt::QueuedConnection,
                              Q_ARG(QString, absPath));
}

void MainWindow::onKbDeleteRequested(const QString& relPath) {
    if (relPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("删除知识库"),
                             QStringLiteral("默认主库不可删除"));
        return;
    }
    if (relPath == currentKbRel_) {
        QMessageBox::warning(
            this, QStringLiteral("删除知识库"),
            QStringLiteral("该库正在使用中——先切换到其他库再删除"));
        return;
    }
    const QString absPath = QString::fromStdString(
        config::dataFilePath(relPath.toStdString().c_str()));
    if (!QFile::remove(absPath)) {
        QMessageBox::warning(this, QStringLiteral("删除知识库"),
                             QStringLiteral("删除失败（文件被占用或不存在）：\n%1")
                                 .arg(absPath));
        return;
    }
    appendRunLog(QStringLiteral("信息"),
                 QStringLiteral("已删除知识库：%1").arg(relPath));
    if (libraryPage_) {
        libraryPage_->refresh();   // 重建下拉列表
    }
}

// ── P0-2 跨页忙碌互斥 ──
// busy 由发起页广播；这里回灌到全部动作页面（含发起页——发起页自身按钮
// 已由其 busySelf_ 禁用，回灌把 busyExternal_ 同步为同一值，状态保持一致）。
void MainWindow::forwardEngineBusy(bool busy) {
    if (searchPage_) {
        searchPage_->setExternalBusy(busy);
    }
    if (libraryPage_) {
        libraryPage_->setExternalBusy(busy);
    }
    if (qualityPage_) {
        qualityPage_->setExternalBusy(busy);
    }
    appendRunLog(QStringLiteral("信息"), busy
        ? QStringLiteral("引擎任务开始")
        : QStringLiteral("引擎任务完成"));
    if (!busy) {
        refreshStatusPage();   // 任务结束后统计可能变化（导入/删除/历史落库）
    }
}

bool MainWindow::engineBusy() const {
    return (searchPage_ && searchPage_->isBusy())
        || (libraryPage_ && libraryPage_->isBusy())
        || (qualityPage_ && qualityPage_->isBusy());
}

// ── 关闭前落盘 ──
void MainWindow::closeEvent(QCloseEvent* event) {
    // P0-2：任务进行中禁止退出。同步等待引擎任务自然收尾（生成可经「■ 停止」
    // 或此处的取消请求中断；导入可经进度对话框取消）。
    // 注意只做非模态提示：closeEvent 可能发生在嵌套事件循环内，
    // 在其中再弹模态对话框有重入风险。
    if (engineBusy()) {
        // 尽力请求中断当前生成（取消令牌经引擎线程事件循环送达活动应答）
        if (engineWorker_) {
            QMetaObject::invokeMethod(engineWorker_, "cancelGeneration",
                                      Qt::QueuedConnection);
        }
        statusBar()->showMessage(
            QStringLiteral("任务进行中已请求取消；请等任务结束（或取消导入）后再次关闭。"),
            6000);
        event->ignore();
        return;
    }

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
    appendRunLog(QStringLiteral("信息"), ready
        ? QStringLiteral("LLM 服务 Key 已配置（生成路就绪）")
        : QStringLiteral("LLM 服务 Key 已清除（AI 回答不可用，检索不受影响）"));
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

    // P0-8：圆点 = 至少一路 AI 服务就绪；不再永远是死绿灯
    setStatusDot((retriever_ && retriever_->embeddingReady()) || apiReady_);
}

void MainWindow::setStatusDot(bool ok) {
    if (!statusDot_) {
        return;
    }
    const QString want = ok ? QStringLiteral("statusDotOk") : QStringLiteral("statusDotOff");
    if (statusDot_->objectName() != want) {
        statusDot_->setObjectName(want);
        // 动态属性/objectName 变更后必须重新跑一遍样式匹配，QSS 才会换色
        statusDot_->style()->unpolish(statusDot_);
        statusDot_->style()->polish(statusDot_);
        statusDot_->update();
    }
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
