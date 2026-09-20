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

    // ── 第 1 页：检索问答 ──
    searchPage_ = new SearchPage(retriever_.get(), pageStack_);
    pageStack_->addWidget(searchPage_);
    connect(searchPage_, &SearchPage::engineStatsChanged,
            this, &MainWindow::onEngineStatsChanged);
    connect(searchPage_, &SearchPage::apiKeyStateChanged,
            this, &MainWindow::onApiKeyStateChanged);

    // ── 第 2 页：文档库（T1）──
    // 插件式接入：只注入引擎裸指针，删掉本页只需去掉这两行 + 删页面文件。
    libraryPage_ = new LibraryPage(retriever_.get(), pageStack_);
    pageStack_->addWidget(libraryPage_);
    connect(libraryPage_, &LibraryPage::libraryChanged,
            this, &MainWindow::onLibraryChanged);

    // ── 第 3–5 页：占位（T2–T4 逐个替换）──
    pageStack_->addWidget(new PlaceholderPage(
        QStringLiteral("问答历史"),
        QStringLiteral("全程可回溯的检索问答记录"),
        QStringLiteral("T2"),
        QStringLiteral("每次回答自动落库（问题 / 回答 / 命中来源 / 命中块数），"
                       "支持关键词检索、详情查看、删除与 Markdown 导出。"),
        pageStack_));

    pageStack_->addWidget(new PlaceholderPage(
        QStringLiteral("检索质量分析"),
        QStringLiteral("同一查询 · 四路并列对比 · 可靠性验证"),
        QStringLiteral("T4"),
        QStringLiteral("面向系统维护者的检索质量看板：同一查询并列跑 "
                       "BM25 单路 / 向量单路 / 加权融合 / RRF 融合，"
                       "输出 Hit@5、R@10、MRR 指标卡——语料或模型变更后重新验证检索可靠性。"),
        pageStack_));

    pageStack_->addWidget(new PlaceholderPage(
        QStringLiteral("设置"),
        QStringLiteral("检索参数与 Embedding 服务地址"),
        QStringLiteral("T3"),
        QStringLiteral("BM25 k1 / b、融合权重、TopK、分块参数、temperature "
                       "以及 Embedding 服务地址与模型名均可配置并持久化。"),
        pageStack_));
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
    apiReady_ = ready;
    statusEmbedding_->setText(apiReady_
        ? QStringLiteral("Embedding：DeepSeek text-embedding-3-small")
        : QStringLiteral("Embedding：未配置（自动降级纯 BM25）"));
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
