#include "ui/main_window.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ui/app_theme.h"
#include "ui/navigation_bar.h"
#include "ui/placeholder_page.h"
#include "ui/search_page.h"

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
    // ── 第 1 页：检索问答（T0 迁移自旧 MainWindow）──
    searchPage_ = new SearchPage(pageStack_);
    pageStack_->addWidget(searchPage_);
    connect(searchPage_, &SearchPage::engineStatsChanged,
            this, &MainWindow::onEngineStatsChanged);
    connect(searchPage_, &SearchPage::apiKeyStateChanged,
            this, &MainWindow::onApiKeyStateChanged);

    // ── 第 2–5 页：占位（T1–T4 逐个替换）──
    pageStack_->addWidget(new PlaceholderPage(
        QStringLiteral("文档库"),
        QStringLiteral("索引持久化 · 启动秒级恢复"),
        QStringLiteral("T1"),
        QStringLiteral("文档列表（文件名 / 类型 / 文本块 / 案件类型 / 裁判结果倾向 / 导入时间）、"
                       "查看元数据、单个与批量删除、清空重建，以及重启后索引自动恢复。"),
        pageStack_));

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
    statusEngine_->setText(QStringLiteral("检索引擎就绪 · %1 文档 / %2 块")
                               .arg(docCount_)
                               .arg(chunkCount_));
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
