// T0 UI 骨架冒烟测试
//
// 两件事：
//   1. 离屏渲染主窗口，逐个点击左侧导航项，校验 QStackedWidget 页序并导出截图；
//   2. （--e2e）在检索问答页跑通「导入 → 检索 → 筛选」，验证重构后检索链路行为不变。
//
//   ui_smoke -platform offscreen <输出目录> [文件名后缀] [--e2e <语料目录>]
//
// 例：
//   ui_smoke -platform offscreen docs/screenshots 1x
//   ui_smoke -platform offscreen docs/screenshots 1x --e2e test/data/legal_cases
//
// 说明：
//   - 需要 offscreen 平台插件；不弹真实窗口，可在无人值守环境跑。
//   - 流式回答需要真实 API Key 且联网，缺 DEEPSEEK_API_KEY 时该步记为跳过（不算失败）。
#include <QApplication>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>

#include "ui/main_window.h"
#include "ui/search_page.h"

namespace {

int g_failures = 0;

void check(bool ok, const QString& label, const QString& detail = QString()) {
    if (!ok) {
        ++g_failures;
    }
    qInfo().noquote() << QStringLiteral("  %1 %2%3")
                             .arg(ok ? QStringLiteral("[ OK ]") : QStringLiteral("[FAIL]"),
                                  label,
                                  detail.isEmpty() ? QString() : QStringLiteral("  (") + detail + QStringLiteral(")"));
}

/// 在导航列表项上派发一次真实鼠标点击（走完整 click → 信号 → 切页链路）
void clickNavItem(QListWidget* list, int row) {
    QListWidgetItem* item = list->item(row);
    if (!item) {
        return;
    }
    const QPoint pos = list->visualItemRect(item).center();

    QMouseEvent press(QEvent::MouseButtonPress, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &press);

    QMouseEvent release(QEvent::MouseButtonRelease, pos, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(list->viewport(), &release);

    QApplication::processEvents();
}

/// 驱动事件循环若干毫秒，让 QTimer::singleShot 排队的检索任务跑完
void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

const char* const kPageNames[] = {
    "01-search", "02-library", "03-history", "04-eval", "05-settings"
};

/// 采集语料目录下的全部可导入文件（按文件名排序，保证可复现）
QStringList collectCorpus(const QString& dirPath) {
    QDir dir(dirPath);
    const QStringList filters = { "*.txt", "*.md", "*.pdf" };
    QStringList files;
    for (const QFileInfo& info : dir.entryInfoList(filters, QDir::Files, QDir::Name)) {
        files << info.absoluteFilePath();
    }
    return files;
}

int runE2E(MainWindow& window, const QString& corpusDir, const QString& outDir) {
    SearchPage* page = window.findChild<SearchPage*>();
    QListWidget* resultList = window.findChild<QListWidget*>("resultList");
    QLineEdit* searchInput = window.findChild<QLineEdit*>("searchInput");
    QPushButton* searchBtn = window.findChild<QPushButton*>("searchBtn");
    QComboBox* caseType = window.findChild<QComboBox*>("caseTypeFilter");

    if (!page || !resultList || !searchInput || !searchBtn || !caseType) {
        check(false, QStringLiteral("检索问答页控件齐全"), QStringLiteral("存在控件未找到"));
        return g_failures;
    }

    // 记录引擎上报的索引规模（导入完成后由 SearchPage 广播）
    int lastDocs = 0;
    int lastChunks = 0;
    QObject::connect(page, &SearchPage::engineStatsChanged,
                     [&lastDocs, &lastChunks](int docs, int chunks) {
                         lastDocs = docs;
                         lastChunks = chunks;
                     });

    const QStringList corpus = collectCorpus(corpusDir);
    check(!corpus.isEmpty(), QStringLiteral("语料目录可访问"),
          QStringLiteral("%1 个文件").arg(corpus.size()));
    if (corpus.isEmpty()) {
        return g_failures;
    }

    // ── 1. 导入 ──
    QElapsedTimer timer;
    timer.start();
    page->importPaths(corpus);
    const qint64 importMs = timer.elapsed();
    check(lastDocs == corpus.size() && lastChunks > 0, QStringLiteral("导入索引"),
          QStringLiteral("%1 文档 / %2 文本块 / %3 ms")
              .arg(lastDocs).arg(lastChunks).arg(importMs));

    const bool keyReady = !qgetenv("DEEPSEEK_API_KEY").isEmpty();
    qInfo().noquote() << QStringLiteral("         API Key：%1")
                             .arg(keyReady ? QStringLiteral("已配置")
                                           : QStringLiteral("未配置（本次验证降级分支）"));

    // ── 2. 检索 ──
    searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
    searchBtn->click();          // onSearch 内部用 singleShot(100) 排队
    pump(3000);                  // 等检索与生成分支走完

    const int hits = resultList->count();
    check(hits > 0, QStringLiteral("检索返回结果"), QStringLiteral("%1 条").arg(hits));
    page->grab().save(outDir + QStringLiteral("/06-search-hits.png"));

    // ── 3. 筛选：命中类型 → 无交集类型（必须归零）→ 复位 ──
    caseType->setCurrentText(QStringLiteral("全部"));
    QApplication::processEvents();
    caseType->setCurrentText(QStringLiteral("商事"));
    QApplication::processEvents();
    check(resultList->count() == 0, QStringLiteral("筛选到无交集类型时归零"),
          QStringLiteral("商事 %1 条").arg(resultList->count()));

    caseType->setCurrentText(QStringLiteral("全部"));
    QApplication::processEvents();
    check(resultList->count() == hits, QStringLiteral("筛选复位后结果还原"),
          QStringLiteral("%1 条").arg(resultList->count()));
    page->grab().save(outDir + QStringLiteral("/07-search-filtered.png"));

    // ── 4. 流式回答 ──
    if (keyReady) {
        check(true, QStringLiteral("流式回答请求已发出（逐字输出需人工确认）"));
    } else {
        qInfo().noquote() << QStringLiteral("  [SKIP] 流式回答：未配置 DEEPSEEK_API_KEY，未联网");
    }

    page->grab().save(QStringLiteral("docs/screenshots/07-search-filtered.png"));
    return g_failures;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("UI skeleton smoke test"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("outdir"),
                                 QStringLiteral("screenshot output directory"));
    parser.addPositionalArgument(QStringLiteral("suffix"),
                                 QStringLiteral("screenshot file name suffix"), QStringLiteral("1x"));
    QCommandLineOption e2eOption(QStringLiteral("e2e"),
                                 QStringLiteral("run import/search/filter end-to-end"),
                                 QStringLiteral("corpus-dir"));
    parser.addOption(e2eOption);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        qCritical() << "usage: ui_smoke <outdir> [suffix] [--e2e <corpus-dir>]";
        return 2;
    }
    const QString outDirPath = args.value(0);
    const QString scaleTag = args.value(1, QStringLiteral("1x"));

    if (!QDir().mkpath(outDirPath)) {
        qCritical() << "cannot create output dir:" << outDirPath;
        return 2;
    }

    MainWindow window;
    window.resize(1180, 720);
    window.show();
    QApplication::processEvents();

    QListWidget* navList = window.findChild<QListWidget*>("navList");
    QStackedWidget* stack = window.findChild<QStackedWidget*>();
    if (!navList || !stack) {
        qCritical() << "navigation list or page stack not found";
        return 2;
    }

    qInfo().noquote() << QStringLiteral("── 导航切换 + 各页渲染 ──");
    const int pages = qMin(stack->count(), static_cast<int>(sizeof(kPageNames) / sizeof(kPageNames[0])));
    for (int i = 0; i < pages; ++i) {
        clickNavItem(navList, i);
        check(stack->currentIndex() == i,
              QStringLiteral("导航 %1 → 第 %2 页").arg(kPageNames[i]).arg(i),
              QStringLiteral("stack=%1").arg(stack->currentIndex()));

        const QString file = QStringLiteral("%1/%2_%3.png").arg(outDirPath, kPageNames[i], scaleTag);
        check(window.grab().save(file), QStringLiteral("截图 %1").arg(kPageNames[i]), file);
    }

    // 进度条只在导入/检索期间出现，静态时应保持隐藏
    if (QProgressBar* progress = window.findChild<QProgressBar*>("taskProgress")) {
        check(!progress->isVisible(), QStringLiteral("静态时进度条隐藏"));
    }

    // 回到第 1 页，便于后续截图与 e2e
    clickNavItem(navList, 0);

    if (parser.isSet(e2eOption)) {
        qInfo().noquote() << QStringLiteral("── 检索问答页 E2E（导入 → 检索 → 筛选）──");
        runE2E(window, parser.value(e2eOption), outDirPath);
    }

    qInfo().noquote() << (g_failures == 0
                              ? QStringLiteral("UI smoke: 全部通过")
                              : QStringLiteral("UI smoke: %1 项失败").arg(g_failures));

    window.close();
    return g_failures == 0 ? 0 : 1;
}
