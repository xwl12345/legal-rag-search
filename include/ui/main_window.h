#pragma once
#include <QMainWindow>

#include <QFont>
#include <QString>

class QLabel;
class QStackedWidget;

class NavigationBar;
class SearchPage;

/// 主窗口骨架：左侧导航 + 中央多页容器 + 底部状态栏。
///
/// T0 之后 MainWindow 不再持有任何业务逻辑，仅负责：
///   1. 把导航切换映射到 QStackedWidget；
///   2. 汇总各页上报的状态到底部 QStatusBar；
///   3. Ctrl + 滚轮调整全局界面字体缩放。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

protected:
    /// 全局 Ctrl+滚轮：调整界面字体缩放
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onNavIndexChanged(int index);
    void onEngineStatsChanged(int docCount, int chunkCount);
    void onApiKeyStateChanged(bool ready);

private:
    void setupUi();
    void setupStatusBar();
    void buildPages();

    /// 应用当前缩放比例（字体 + 全局 QSS）
    void applyUiScale();

    // ── 骨架三件套 ──
    NavigationBar* navBar_ = nullptr;
    QStackedWidget* pageStack_ = nullptr;

    // ── 业务页（占位页在 buildPages 里就地构造后交给 stack 托管）──
    SearchPage* searchPage_ = nullptr;

    // ── 状态栏 ──
    QLabel* statusEngine_ = nullptr;
    QLabel* statusEmbedding_ = nullptr;
    QLabel* statusLlm_ = nullptr;
    QLabel* statusVersion_ = nullptr;

    // ── 界面缩放 ──
    int uiScale_ = 100;   // 缩放百分比（60–250）
    int docCount_ = 0;
    int chunkCount_ = 0;
    bool apiReady_ = false;
};
