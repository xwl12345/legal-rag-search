#include "ui/main_window.h"

#include <QApplication>
#include <QCloseEvent>
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
#include "ui/settings_page.h"
#include "config/app_config.h"
#include "history/history_store.h"
#include "rag/retriever.h"

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

    // ── 第 1 页：检索问答 ──
    searchPage_ = new SearchPage(retriever_, pageStack_);
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
    libraryPage_ = new LibraryPage(retriever_, pageStack_);
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

    // ── 第 5 页：设置（T3）──
    // 插件式接入：本页只读写配置层，不碰 Retriever——保存后发 settingsChanged，
    // 由本窗口中转给引擎（热更新）与检索页（检索宽度/温度），两页互不引用。
    settingsPage_ = new SettingsPage(pageStack_);
    pageStack_->addWidget(settingsPage_);
    connect(settingsPage_, &SettingsPage::settingsChanged,
            this, &MainWindow::onSettingsChanged);

    // 引擎配置应用完成后刷新状态栏服务区（Embedding 可能刚配上 Key）
    connect(engineWorker_, &EngineWorker::settingsApplied,
            this, &MainWindow::refreshServiceStatus);

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
