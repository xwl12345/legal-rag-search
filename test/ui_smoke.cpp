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
#include <QCheckBox>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>

#include <cmath>
#include <functional>

#include "ui/main_window.h"
#include "ui/search_page.h"
#include "ui/library_page.h"
#include "ui/history_page.h"
#include "ui/quality_page.h"
#include "ui/favorites_page.h"
#include "ui/doc_viewer_dialog.h"
#include "ui/snippet_dialog.h"
#include "../test/fake_transport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "ui/settings_page.h"
#include "config/app_config.h"
#include "config/app_settings.h"
#include "history/history_store.h"
#include "law/law_store.h"
#include "favorite/favorite_store.h"
#include "rag/retriever.h"

#include <QLabel>
#include <QTreeWidget>

namespace {

int g_failures = 0;

/// T10.1 期（案例 C-9）：E2E 索引隔离文件——main() 通过环境变量
/// RAG_SMOKE_INDEX_FILE 把全部窗口的索引恢复/落盘引到这里，
/// 套件不读不写用户真实 rag_index.dat（旧流程每次跑 E2E 都会覆盖它）。
static const QString kSuiteIndexPath = QStringLiteral("build/e2e_smoke_index.dat");

// P2：注入 MainWindow 的假传输——历史 E2E 用它驱动真实 SSE 生成链路
static std::shared_ptr<FakeTransport> g_fakeTransport;

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

/// P1：引擎任务已队列化到引擎线程，断言前等待其真正收尾（带超时护栏）
void waitUntil(const std::function<bool()>& done, int maxMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < maxMs) {
        pump(30);
    }
}

const char* const kPageNames[] = {
    "01-search", "02-library", "03-history", "04-eval", "05-favorites", "06-law", "07-settings"
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

/// P3-5.2：疑似副本检测（Windows 复制粘贴「 - 副本 / - Copy」、拖拽「(1)」等后缀）。
/// 历史事故：语料目录混入副本后，按 21 篇等值断言的用例被破坏。
/// 现导入断言改为 ≥21 下限，副本只告警不判失败，便于人工清理。
bool looksLikeDuplicate(const QString& fileName) {
    static const QRegularExpression dupPattern(
        QStringLiteral("( - 副本| - copy|\\(\\d+\\))\\.(txt|md|pdf)$"),
        QRegularExpression::CaseInsensitiveOption);
    return dupPattern.match(fileName).hasMatch();
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

    // T6：三态视角选择器——存在性、默认中立、语义值与文案分离（userData）
    QComboBox* perspectiveSelector =
        window.findChild<QComboBox*>(QStringLiteral("perspectiveSelector"));
    check(perspectiveSelector != nullptr, QStringLiteral("T6 视角选择器存在"));
    if (perspectiveSelector) {
        check(perspectiveSelector->currentIndex() == 0 &&
                  perspectiveSelector->currentData().toInt() == 0,
              QStringLiteral("T6 默认档位为中立（语义值 0）"),
              QStringLiteral("实际 index=%1 data=%2")
                  .arg(perspectiveSelector->currentIndex())
                  .arg(perspectiveSelector->currentData().toInt()));
        perspectiveSelector->setCurrentIndex(2);
        check(perspectiveSelector->currentData().toInt() == 2,
              QStringLiteral("T6 切换被告方后语义值 2"));
        perspectiveSelector->setCurrentIndex(0);   // 还原中立：后续断言按默认态
    }

    // P3-5.2：疑似副本只告警不判失败（导入断言按 ≥21 下限执行）
    QStringList duplicates;
    for (const QString& f : corpus) {
        if (looksLikeDuplicate(QFileInfo(f).fileName())) {
            duplicates << QFileInfo(f).fileName();
        }
    }
    if (!duplicates.isEmpty()) {
        qInfo().noquote() << QStringLiteral("         ⚠ 语料目录含 %1 个疑似副本：%2（建议清理后重跑）")
                                 .arg(duplicates.size())
                                 .arg(duplicates.join(QStringLiteral("、")));
    }

    // ── 1. 导入 ──
    QElapsedTimer timer;
    timer.start();
    page->importPaths(corpus);
    waitUntil([&] { return !page->isBusy(); });   // P1：导入在引擎线程异步执行
    const qint64 importMs = timer.elapsed();
    check(lastDocs >= 21 && lastChunks > 0, QStringLiteral("导入索引"),
          QStringLiteral("%1 文档 / %2 文本块 / %3 ms")
              .arg(lastDocs).arg(lastChunks).arg(importMs));

    const bool keyReady = !qgetenv("DEEPSEEK_API_KEY").isEmpty();
    qInfo().noquote() << QStringLiteral("         API Key：%1")
                             .arg(keyReady ? QStringLiteral("已配置")
                                           : QStringLiteral("未配置（本次验证降级分支）"));

    // ── 2. 检索（P0-2 忙碌互斥一并在本段验证）──
    searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
    searchBtn->click();          // onSearch 内部用 singleShot(100) 排队
    QApplication::processEvents();

    // P0-2：检索+生成期间，检索页忙碌、引擎动作按钮全部禁用、
    // 他页动作（此处以「忙碌期导入」代言）必须被拒绝。
    check(page->isBusy(), QStringLiteral("P0-2：检索期间检索页处于忙碌态"));
    check(!searchBtn->isEnabled(), QStringLiteral("P0-2：检索期间检索按钮禁用"));
    if (QPushButton* importBtn = window.findChild<QPushButton*>("importBtn")) {
        check(!importBtn->isEnabled(), QStringLiteral("P0-2：检索期间导入按钮禁用"));
    }
    const int docsBeforeBusy = lastDocs;
    page->importPaths(corpus);   // 忙碌期导入必须整体 no-op
    check(lastDocs == docsBeforeBusy, QStringLiteral("P0-2：忙碌期导入被拒（引擎无变化）"),
          QStringLiteral("导入前后文档数 %1 / %2").arg(docsBeforeBusy).arg(lastDocs));

    // P0-2：任务进行中关窗被拦截——置于任何事件泵之前，确保必在忙碌窗口内
    // （引擎线程的检索仅数毫秒，期间不能泵事件，否则可能抢先完成）
    window.close();
    check(window.isVisible(), QStringLiteral("P0-2：任务进行中关闭被拦截"));
    check(page->isBusy(), QStringLiteral("P0-2：拦截后任务继续运行"));

    // P1：切页即时生效，UI 不被引擎调用阻塞（若检索已抢先完成，
    // 切页同样应即时生效——本断言验证的是 UI 事件循环始终畅通）
    if (QListWidget* nav = window.findChild<QListWidget*>("navList")) {
        clickNavItem(nav, 1);
        QStackedWidget* stack = window.findChild<QStackedWidget*>();
        check(stack && stack->currentIndex() == 1,
              QStringLiteral("P1：切页即时生效（UI 不被引擎阻塞）"),
              QStringLiteral("stack=%1").arg(stack ? stack->currentIndex() : -1));
        clickNavItem(nav, 0);
        QApplication::processEvents();
    }

    pump(3000);                  // 等检索与生成分支走完
    check(!page->isBusy(), QStringLiteral("P0-2：检索完成后忙碌解除"));
    check(searchBtn->isEnabled(), QStringLiteral("P0-2：完成后按钮恢复可用"));

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

    // ── 3.5 T5 角色标签 + 「只看本院认为」过滤 ──
    QCheckBox* courtOnly = window.findChild<QCheckBox*>("courtOnlyFilter");
    check(courtOnly != nullptr, QStringLiteral("T5：只看本院认为复选框存在"));
    bool anyTagged = false;
    for (int i = 0; i < resultList->count(); ++i) {
        const QString text = resultList->item(i)->text();
        if (text.contains(QStringLiteral("[诉称]")) || text.contains(QStringLiteral("[辩称]"))
            || text.contains(QStringLiteral("[本院认为"))
            || text.contains(QStringLiteral("[判决"))) {
            anyTagged = true;
        }
    }
    check(anyTagged, QStringLiteral("T5：检索结果行携带角色标签（抽检）"));

    courtOnly->setChecked(true);
    QApplication::processEvents();
    const int opinionCount = resultList->count();
    bool allOpinion = opinionCount > 0;
    for (int i = 0; i < opinionCount; ++i) {
        // 位过滤不变式：保留块必持法院认定位。跨段块标签可为
        // 「本院认为|判决」「诉称|本院认为」等组合，因此匹配子串"本院认为"
        // 而非前缀"[本院认为"（P3 冒烟发现：检索宽度变化会让跨段块进前列）
        allOpinion = allOpinion
            && resultList->item(i)->text().contains(QStringLiteral("本院认为"));
    }
    check(allOpinion, QStringLiteral("T5：只看本院认为过滤生效（结果全为法院认定块）"),
          QStringLiteral("%1 条").arg(opinionCount));
    page->grab().save(outDir + QStringLiteral("/15-search-court-only.png"));

    courtOnly->setChecked(false);
    QApplication::processEvents();
    check(resultList->count() == hits, QStringLiteral("T5：取消过滤后结果还原"),
          QStringLiteral("%1 条").arg(resultList->count()));

    // ── 4. 流式回答 ──
    if (keyReady) {
        check(true, QStringLiteral("流式回答请求已发出（逐字输出需人工确认）"));
    } else {
        qInfo().noquote() << QStringLiteral("  [SKIP] 流式回答：未配置 DEEPSEEK_API_KEY，未联网");
    }

    page->grab().save(QStringLiteral("docs/screenshots/07-search-filtered.png"));
    return g_failures;
}

/// T1 文档库页 E2E：导入后列表可见 → 查看详情 → 删除单篇 → 计数同步
int runLibraryE2E(MainWindow& window, const QString& corpusDir, const QString& outDir) {
    SearchPage* searchPage = window.findChild<SearchPage*>();
    LibraryPage* libPage = window.findChild<LibraryPage*>();
    QTableWidget* table = window.findChild<QTableWidget*>("libraryTable");
    QLineEdit* searchInput = window.findChild<QLineEdit*>("searchInput");

    if (!searchPage || !libPage || !table || !searchInput) {
        check(false, QStringLiteral("文档库页控件齐全"), QStringLiteral("存在控件未找到"));
        return g_failures;
    }

    const QStringList corpus = collectCorpus(corpusDir);
    if (corpus.isEmpty()) {
        check(false, QStringLiteral("语料目录可访问"));
        return g_failures;
    }

    // 先经检索页导入，验证"共享引擎"——导入的文档文档库页应立刻看到
    searchPage->importPaths(corpus);
    waitUntil([&] { return !searchPage->isBusy(); });
    QApplication::processEvents();
    libPage->refresh();
    QApplication::processEvents();

    check(libPage->rowCount() == corpus.size(),
          QStringLiteral("导入后文档库列表同步"),
          QStringLiteral("%1 行").arg(libPage->rowCount()));

    check(table->columnCount() == 6, QStringLiteral("文档库 6 列"),
          QStringLiteral("实际 %1 列").arg(table->columnCount()));

    // 结果倾向列（第 5 列，下标 4）应已填充，不应为空
    int filledTendency = 0;
    for (int r = 0; r < table->rowCount(); ++r) {
        QTableWidgetItem* item = table->item(r, 4);
        if (item && !item->text().isEmpty()) ++filledTendency;
    }
    check(filledTendency == table->rowCount(),
          QStringLiteral("结果倾向列全部填充"),
          QStringLiteral("%1 / %2").arg(filledTendency).arg(table->rowCount()));

    window.grab().save(outDir + QStringLiteral("/08-library.png"));

    // ── 删除单篇：检索页的陈旧缓存必须被清掉 ──
    // 先真的跑一次检索，让检索页有缓存可作废（只 setText 不会填充缓存）
    QPushButton* searchBtn = window.findChild<QPushButton*>("searchBtn");
    if (searchBtn) {
        searchInput->setText(QStringLiteral("合同 履行"));
        searchBtn->click();
        pump(2000);
    }
    const int cachedBefore = searchPage->lastResultCount();
    check(cachedBefore > 0, QStringLiteral("删除前检索页已有缓存"),
          QStringLiteral("%1 条").arg(cachedBefore));

    const QString victim = table->item(0, 0)->text();
    const bool removed = libPage->removeDocumentById(victim, /*confirm=*/false);
    QApplication::processEvents();

    check(removed, QStringLiteral("删除单篇成功"), victim);
    check(libPage->rowCount() == corpus.size() - 1,
          QStringLiteral("删除后列表行数 -1"),
          QStringLiteral("%1 行").arg(libPage->rowCount()));
    check(searchPage->lastResultCount() == 0,
          QStringLiteral("删除后检索页缓存被作废"),
          QStringLiteral("%1 条").arg(searchPage->lastResultCount()));

    window.grab().save(outDir + QStringLiteral("/09-library-after-delete.png"));

    return g_failures;
}

/// T1 持久化 E2E：落盘 → 销毁窗口 → 重建窗口 → 索引自动恢复
/// 直接验证"关闭程序再打开，文档库还在"这条 T1 核心验收标准。
///
/// 说明：MainWindow 走生产默认路径（config::INDEX_FILE，即工作目录下的
/// rag_index.dat），这里不做路径注入——真实验证的就是上线那条代码路径。
/// 跑完把文件删掉，避免污染工作目录。
int runPersistenceE2E(const QString& corpusDir, const QString& outDir) {
    // T10.1 期（案例 C-9）：改用套件隔离文件（环境变量已把窗口引到这里），
    // 持久化语义不变——会话 1 落盘到隔离文件，会话 2 从同一文件恢复。
    const QString indexPath = kSuiteIndexPath;

    // 先清掉可能存在的旧索引，保证"会话 1 从空库起"这个前提成立
    QFile::remove(indexPath);

    // ── 第一次会话：导入并落盘 ──
    int docsBefore = 0;
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        SearchPage* page = window.findChild<SearchPage*>();
        LibraryPage* libPage = window.findChild<LibraryPage*>();
        if (!page || !libPage) {
            check(false, QStringLiteral("会话 1 页面就绪"));
            return g_failures;
        }

        page->importPaths(collectCorpus(corpusDir));
        waitUntil([&] { return !page->isBusy(); });
        QApplication::processEvents();
        libPage->refresh();
        QApplication::processEvents();

        docsBefore = libPage->rowCount();
        check(docsBefore > 0, QStringLiteral("会话 1 导入成功"),
              QStringLiteral("%1 篇").arg(docsBefore));

        // 关闭窗口触发 closeEvent → saveIndex()
        window.close();
        QApplication::processEvents();
    }

    check(QFile::exists(indexPath), QStringLiteral("索引文件已落盘"), indexPath);

    // ── 第二次会话：全新窗口，启动时应自动恢复 ──
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        LibraryPage* libPage = window.findChild<LibraryPage*>();
        QTableWidget* table = window.findChild<QTableWidget*>("libraryTable");

        if (!libPage || !table) {
            check(false, QStringLiteral("会话 2 页面就绪"));
            return g_failures;
        }

        check(libPage->rowCount() == docsBefore,
              QStringLiteral("重启后文档库自动恢复"),
              QStringLiteral("%1 篇（会话 1 为 %2）")
                  .arg(libPage->rowCount()).arg(docsBefore));

        // 恢复出的列表内容也要可用：结果倾向列仍有值
        int filled = 0;
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem* item = table->item(r, 4);
            if (item && !item->text().isEmpty()) ++filled;
        }
        check(filled == table->rowCount(),
              QStringLiteral("恢复后元数据完整（结果倾向列）"),
              QStringLiteral("%1 / %2").arg(filled).arg(table->rowCount()));

        window.grab().save(outDir + QStringLiteral("/10-library-restored.png"));

        // 关闭时会把恢复出来的索引再写一遍——正常，符合生产行为
        window.close();
        QApplication::processEvents();
    }

    QFile::remove(indexPath);
    return g_failures;
}

/// T2 问答历史 E2E：真实检索 + 回答收尾 → 自动落库 → 列表 / 关键词 / 删除 / 导出 → 重启仍在
///
/// 说明：本组验证走的是 MainWindow 默认路径（config::HISTORY_DB，即工作目录下的
/// rag_history.db），跑完清空记录，不与用户手工留下的数据混淆。
/// P2 起：回答由 FakeTransport 驱动的真实生成链路产生（注入假 Key + SSE 应答），
/// simulateAnswer 测试后门已删除——假的只有传输层，其余全是生产代码路径。
///
/// ⚠️ 清状态一律走 SQL 层（removeAll），**不要试图 QFile::remove 库文件**：
/// 本进程里 main() 的主窗口全程存活，SQLite 连接一直开着，
/// Windows 不允许删掉被打开的文件 —— 删除必然失败，
/// 于是上一轮残留的记录会被下一轮读进来，整套断言级联假失败。
/// 用行级清理则不依赖文件系统状态，连跑多少次都从 0 条起步。
int runHistoryE2E(const QString& corpusDir, const QString& outDir) {
    const QString dbPath = QString::fromStdString(config::dataFilePath(config::HISTORY_DB));   // P3：exe 目录

    const QString exportPath = outDir + QStringLiteral("/11-history-export.md");
    QFile::remove(exportPath);

    const QStringList corpus = collectCorpus(corpusDir);
    if (corpus.isEmpty()) {
        check(false, QStringLiteral("语料目录可访问"));
        return g_failures;
    }

    int survived = 0;

    // ── 会话 1：落库 → 列表 → 关键词 → 详情 → 删除 ──
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        SearchPage* searchPage = window.findChild<SearchPage*>();
        HistoryPage* historyPage = window.findChild<HistoryPage*>();
        history::HistoryStore* store = window.historyStore();
        QTableWidget* table = window.findChild<QTableWidget*>("historyTable");
        QLineEdit* keyword = window.findChild<QLineEdit*>("historyKeyword");
        QLineEdit* searchInput = window.findChild<QLineEdit*>("searchInput");
        QPushButton* searchBtn = window.findChild<QPushButton*>("searchBtn");
        QListWidget* navList = window.findChild<QListWidget*>("navList");

        if (!searchPage || !historyPage || !store || !table || !keyword
            || !searchInput || !searchBtn || !navList) {
            check(false, QStringLiteral("问答历史页控件齐全"), QStringLiteral("存在控件未找到"));
            return g_failures;
        }

        check(store->isOpen(), QStringLiteral("问答历史库打开成功"),
              QString::fromStdString(store->lastError()));

        // P2：假传输装到本组窗口的 worker 上（离线驱动真实生成链路）。
        // chat 应答器回 2 段 SSE 增量；embeddings 按输入数量返回 index 化向量。
        g_fakeTransport = std::make_shared<FakeTransport>();
        g_fakeTransport->setResponder([](const QUrl& url, const QByteArray& body)
                                           -> FakeTransport::Reply {
            FakeTransport::Reply r;
            if (url.path().contains(QStringLiteral("chat/completions"))) {
                r.chunks = {
                    QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"交付凭证\"}}]}\n\n"),
                    QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"应当结合转账记录与收条综合认定。\"}}]}\n\n"),
                    QByteArray("data: [DONE]\n\n"),
                };
                r.finish = FakeTransport::Finish{200, QByteArray(), false, QString()};
            } else {
                const QJsonDocument doc = QJsonDocument::fromJson(body);
                const QJsonArray inputs = doc.object()["input"].toArray();
                QJsonArray data;
                for (int i = 0; i < inputs.size(); ++i) {
                    QJsonObject item;
                    item["index"] = i;
                    QJsonArray vec;
                    vec.append(1.0);
                    vec.append(2.0);
                    item["embedding"] = vec;
                    data.append(item);
                }
                QJsonObject root;
                root["data"] = data;
                r.finish = FakeTransport::Finish{
                    200, QJsonDocument(root).toJson(QJsonDocument::Compact), false, QString()};
            }
            return r;
        });
        window.engineWorker()->installTransport(g_fakeTransport);

        // 从空历史起步：清前现状记进详情里，将来若失败一眼能看出是残留还是写入异常
        const long long beforeCount = store->count();
        store->removeAll();
        QApplication::processEvents();
        check(store->count() == 0, QStringLiteral("起始历史为空（已清空历史数据）"),
              QStringLiteral("清理前 %1 条 → 清理后 %2 条").arg(beforeCount).arg(store->count()));

        // 真实的一半：导入 + 检索，命中结果作为落库来源
        searchPage->importPaths(corpus);
        waitUntil([&] { return !searchPage->isBusy(); });
        QApplication::processEvents();

        searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
        searchBtn->click();
        pump(2000);
        check(searchPage->lastResultCount() > 0,
              QStringLiteral("检索页已有真实命中结果（作为记录来源）"),
              QStringLiteral("%1 条").arg(searchPage->lastResultCount()));

        // ── P2：历史链路走真实生成（FakeTransport + 假 Key，不再用测试后门）──
        QLineEdit* apiKeyInput = window.findChild<QLineEdit*>("apiKeyInput");
        QPushButton* setApiKeyBtn = window.findChild<QPushButton*>("setApiKeyBtn");
        QPushButton* stopGenBtn = window.findChild<QPushButton*>("stopGenBtn");
        if (!apiKeyInput || !setApiKeyBtn || !stopGenBtn) {
            check(false, QStringLiteral("P2：生成链路控件齐全"),
                  QStringLiteral("存在控件未找到"));
            return g_failures;
        }
        check(true, QStringLiteral("P2：生成链路控件齐全"));
        apiKeyInput->setText(QStringLiteral("sk-fake-history-e2e-key"));
        setApiKeyBtn->click();
        QApplication::processEvents();

        // 回合 1：完整回答——真实检索 → 引擎线程 SSE 生成（2 段增量）→ 收场落库
        searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
        searchBtn->click();
        waitUntil([&] { return !searchPage->isBusy(); });
        QApplication::processEvents();

        // 落库后主窗口应主动刷新历史页（不经切页、不点刷新按钮）
        check(historyPage->rowCount() == 1,
              QStringLiteral("真实生成后历史列表自动出现记录（无需手动刷新）"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));

        // 回合 2：中断——fake 先吐一段再挂起（非空残卷）→ 点「■ 停止」→
        // interrupted 落库。换一个不同查询，保证关键词阶段「交付凭证」只命中回合 1。
        // 注：纯挂起不吐字会被 T2「空回合不入库」规则正确跳过，故必须先给一段。
        searchInput->setText(QStringLiteral("劳动争议 仲裁时效"));
        g_fakeTransport->setHangAfterChunks(1);
        searchBtn->click();
        pump(500);   // 等检索完成并进入生成阶段（生成已被 fake 挂起）
        check(searchPage->isBusy(), QStringLiteral("P2：挂起的生成保持忙碌态"));
        stopGenBtn->click();
        waitUntil([&] { return !searchPage->isBusy(); });
        QApplication::processEvents();

        check(historyPage->rowCount() == 2,
              QStringLiteral("中断回合后列表继续同步（真实停止链路）"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));
        check(store->count() == 2, QStringLiteral("两条记录均已落库"),
              QStringLiteral("%1 条").arg(store->count()));

        // 来源与中断标记确实进了库（recent 按时间倒序，最新在前）
        const auto records = store->recent(10);
        check(static_cast<int>(records.size()) == 2, QStringLiteral("可读回 2 条记录"));
        if (records.size() == 2) {
            // 同秒内两回合的 created_at 可能并列，断言不依赖返回顺序
            bool hasInterrupted = false, hasComplete = false;
            for (const auto& rec : records) {
                if (rec.interrupted) {
                    hasInterrupted = true;
                    check(!rec.sources.empty(),
                          QStringLiteral("命中来源随中断回合一起入库"),
                          QStringLiteral("%1 个文本块").arg(rec.hitCount()));
                } else if (QString::fromStdString(rec.answer)
                               .contains(QStringLiteral("综合认定"))) {
                    hasComplete = true;
                }
            }
            check(hasInterrupted,
                  QStringLiteral("「停止」中断回合被标记为未完成（真实取消链路）"));
            check(hasComplete,
                  QStringLiteral("完整回合回答来自真实 SSE 流"));
        }

        // ── 关键词搜索（命中回合 1 的查询；round 2 残卷恰好含「交付凭证」，
        //    故选只出现在回合 1 查询里的「民间借贷」）──
        keyword->setText(QStringLiteral("民间借贷"));
        QApplication::processEvents();
        check(historyPage->rowCount() == 1, QStringLiteral("关键词搜索命中"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));

        keyword->setText(QStringLiteral("凭空捏造的词"));
        QApplication::processEvents();
        check(historyPage->rowCount() == 0, QStringLiteral("无关关键词命中 0 条"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));

        keyword->clear();
        QApplication::processEvents();
        check(historyPage->rowCount() == 2, QStringLiteral("清空关键词后列表还原"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));

        // ── 选中一行 → 右侧详情 ──
        clickNavItem(navList, 2);
        QApplication::processEvents();
        if (table->rowCount() > 0) {
            table->selectRow(0);
            QApplication::processEvents();
            QTextEdit* detail = window.findChild<QTextEdit*>("historyDetail");
            check(detail && !detail->toPlainText().trimmed().isEmpty(),
                  QStringLiteral("选中记录后详情区有内容"));
        }
        window.grab().save(outDir + QStringLiteral("/11-history.png"));

        // ── 导出 Markdown ──
        long long targetId = 0;
        const auto all = store->recent(1);
        if (!all.empty()) {
            targetId = all.front().id;
        }
        check(targetId > 0, QStringLiteral("取到待导出记录的 id"));
        check(historyPage->exportRecordById(targetId, exportPath, /*confirm=*/false),
              QStringLiteral("导出 Markdown 成功"), exportPath);
        {
            QFile file(exportPath);
            check(file.exists() && file.size() > 0, QStringLiteral("导出文件已生成"),
                  QStringLiteral("%1 字节").arg(file.size()));
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray raw = file.readAll();
                file.close();
                check(raw.startsWith(QByteArray("\xEF\xBB\xBF")),
                      QStringLiteral("导出件带 UTF-8 BOM（防记事本乱码）"));
                const QString text = QString::fromUtf8(
                    QByteArray(raw.constData() + 3, qMax(0, raw.size() - 3)));
                check(text.contains(QStringLiteral("劳动争议")),
                      QStringLiteral("导出件回读中文正常（UTF-8 无损）"));
                check(text.contains(QStringLiteral("命中块数：")),
                      QStringLiteral("导出件含命中来源章节"));
            }
        }

        // ── T12：检索报告导出（ODT/PDF）──
        QPushButton* reportBtn = window.findChild<QPushButton*>("historyReportBtn");
        check(reportBtn != nullptr, QStringLiteral("T12：「导出报告」按钮存在"));
        check(reportBtn->isVisible(), QStringLiteral("T12：报告按钮在工具栏可见"));
        check(reportBtn->isEnabled(), QStringLiteral("T12：单选记录时报告按钮可用"));

        const QString reportOdt = outDir + QStringLiteral("/12-report-export.odt");
        const QString reportPdf = outDir + QStringLiteral("/12-report-export.pdf");
        QFile::remove(reportOdt);
        QFile::remove(reportPdf);

        check(historyPage->exportReportById(targetId, reportOdt, /*confirm=*/false),
              QStringLiteral("T12：导出 ODT 报告成功"), reportOdt);
        check(historyPage->exportReportById(targetId, reportPdf, /*confirm=*/false),
              QStringLiteral("T12：导出 PDF 报告成功"), reportPdf);

        {
            QFile odt(reportOdt);
            check(odt.exists() && odt.size() > 0, QStringLiteral("T12：ODT 文件已生成"),
                  QStringLiteral("%1 字节").arg(odt.size()));
            if (odt.open(QIODevice::ReadOnly)) {
                check(odt.read(2) == QByteArray("PK"),
                      QStringLiteral("T12：ODT 为合法 ZIP 容器（ODF 规范）"));
                odt.close();
            }
            QFile pdf(reportPdf);
            check(pdf.exists() && pdf.size() > 0, QStringLiteral("T12：PDF 文件已生成"),
                  QStringLiteral("%1 字节").arg(pdf.size()));
            if (pdf.open(QIODevice::ReadOnly)) {
                check(pdf.read(5) == QByteArray("%PDF-"),
                      QStringLiteral("T12：PDF 文件头正确"));
                pdf.close();
            }
        }

        // 非法扩展名 → 如实失败（不崩溃、不误写）
        check(!historyPage->exportReportById(
                  targetId, outDir + QStringLiteral("/12-report-export.xyz"),
                  /*confirm=*/false),
              QStringLiteral("T12：非法扩展名被拒绝且无文件写出"));

        // 未选中任何行 → 按钮禁用（「仅单选」语义的 UI 兜底）
        table->clearSelection();
        QApplication::processEvents();
        check(!reportBtn->isEnabled(), QStringLiteral("T12：未选中记录时报告按钮禁用"));

        // 多选两行 → 同样禁用
        table->selectRow(0);
        table->selectionModel()->select(table->model()->index(1, 0),
                                        QItemSelectionModel::Select
                                            | QItemSelectionModel::Rows);
        QApplication::processEvents();
        check(!reportBtn->isEnabled(), QStringLiteral("T12：多选记录时报告按钮禁用"));

        // 恢复单选并留证据截图（含「📄 导出报告」按钮的工具栏）
        table->selectRow(0);
        QApplication::processEvents();
        window.grab().save(outDir + QStringLiteral("/20-report-export.png"));

        // ── 删除一条 ──
        check(historyPage->removeRecordById(targetId, /*confirm=*/false),
              QStringLiteral("删除单条记录成功"));
        check(historyPage->rowCount() == 1, QStringLiteral("删除后列表行数 -1"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));

        survived = store->count();
        window.close();
        QApplication::processEvents();
    }

    check(QFile::exists(dbPath), QStringLiteral("历史库文件已落盘"), dbPath);

    // ── 会话 2：全新窗口，重启后记录仍在 ──
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        HistoryPage* historyPage = window.findChild<HistoryPage*>();
        history::HistoryStore* store = window.historyStore();
        if (!historyPage || !store) {
            check(false, QStringLiteral("会话 2 历史页就绪"));
            return g_failures;
        }

        check(historyPage->rowCount() == survived,
              QStringLiteral("重启程序后问答历史自动恢复"),
              QStringLiteral("%1 行（会话 1 剩余 %2）")
                  .arg(historyPage->rowCount()).arg(survived));

        window.grab().save(outDir + QStringLiteral("/12-history-restored.png"));

        // 切页也要刷新：会话 2 开窗时本页尚未被点开过，
        // 若只在构造时读一次库，这里就会看到 0 行（曾经的真实缺陷）。
        QListWidget* navList = window.findChild<QListWidget*>("navList");
        if (navList) {
            clickNavItem(navList, 2);
            QApplication::processEvents();
            check(historyPage->rowCount() == survived,
                  QStringLiteral("切到历史页时列表已刷新"),
                  QStringLiteral("%1 行").arg(historyPage->rowCount()));
        }

        // 详情：选中一行后右侧应有内容（验证来源 JSON 跨会话还原）
        QTableWidget* table = window.findChild<QTableWidget*>("historyTable");
        QTextEdit* detail = window.findChild<QTextEdit*>("historyDetail");
        if (table && table->rowCount() > 0 && detail) {
            table->selectRow(0);
            QApplication::processEvents();
            const QString text = detail->toPlainText();
            check(text.contains(QStringLiteral("命中来源")),
                  QStringLiteral("重启后详情含命中来源章节"));
            check(text.contains(QStringLiteral("case_")),
                  QStringLiteral("重启后来源文档名可读（JSON 往返无损）"));
        }

        window.close();
        QApplication::processEvents();
    }

    // 收尾：清空本轮产生的记录，别在用户自己的历史库里留测试数据。
    // 同样只用 SQL 清理（会话 2 的窗口析构后新建一条短连接即可），不删文件。
    {
        history::HistoryStore cleanup(dbPath.toStdString());
        if (cleanup.open()) {
            cleanup.removeAll();
        }
    }
    return g_failures;
}

/// P0-7 同名异路径覆盖确认 E2E：同路径更新静默放行；异路径同名经确认钩子
/// 完成 覆盖 / 跳过 / 取消 三种选择，引擎内容与之一致。
int runOverwriteE2E() {
    // T10.1 期（案例 C-9）：清理目标改为套件隔离文件（本组需要空库起步）
    QFile::remove(kSuiteIndexPath);

    // ── 准备三个目录下的同名文件（内容互不相同，各带独有标记词）──
    struct FileSpec { QString dir; QString marker; };
    const FileSpec specs[] = {
        { QStringLiteral("p0_dup_a"), QStringLiteral("甲案独有标记词玉衡") },
        { QStringLiteral("p0_dup_b"), QStringLiteral("乙案独有标记词璇玑") },
        { QStringLiteral("p0_dup_c"), QStringLiteral("丙案独有标记词玉衡三号") },
    };
    const QString name = QStringLiteral("case_p0_dup.txt");
    QStringList paths;
    for (const auto& spec : specs) {
        const QString dir = QDir::current().absoluteFilePath(spec.dir);
        QDir().mkpath(dir);
        const QString path = dir + QLatin1Char('/') + name;
        QFile file(path);
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(QStringLiteral("（2024）民初0000号 模拟文书。%1，本案事实清楚。")
                       .arg(spec.marker).toUtf8());
        file.close();
        paths << path;
    }

    MainWindow window;
    window.resize(1180, 720);
    window.show();
    QApplication::processEvents();

    SearchPage* page = window.findChild<SearchPage*>();
    LibraryPage* libPage = window.findChild<LibraryPage*>();
    QTableWidget* table = window.findChild<QTableWidget*>("libraryTable");
    QTextEdit* detail = window.findChild<QTextEdit*>("libraryDetail");
    if (!page || !libPage || !table || !detail) {
        check(false, QStringLiteral("P0-7：页面与控件就绪"), QStringLiteral("存在控件未找到"));
        return g_failures;
    }

    auto showFirstRowDetail = [&]() {
        QApplication::processEvents();
        if (table->rowCount() > 0) {
            table->selectRow(0);
            QApplication::processEvents();
        }
    };

    // 首次导入 A：docId 进入引擎
    page->importPaths({paths[0]});
    waitUntil([&] { return !page->isBusy(); });
    QApplication::processEvents();
    libPage->refresh();
    showFirstRowDetail();
    check(libPage->rowCount() == 1, QStringLiteral("P0-7：首次导入同名文件 A 成功"),
          QStringLiteral("%1 行").arg(libPage->rowCount()));

    // 注入脚本化确认钩子（默认弹窗在无人值守环境无法点击）
    struct Hook {
        int calls = 0;
        QString lastDocId, lastExisting, lastNew;
        SearchPage::OverwriteChoice answer = SearchPage::OverwriteChoice::Overwrite;
    } hook;
    page->setConfirmOverwriteHandler(
        [&hook](const QString& docId, const QString& existingPath, const QString& newPath) {
            ++hook.calls;
            hook.lastDocId = docId;
            hook.lastExisting = existingPath;
            hook.lastNew = newPath;
            return hook.answer;
        });

    // ① 同路径重复导入（更新场景）：静默放行，不触发钩子
    page->importPaths({paths[0]});
    waitUntil([&] { return !page->isBusy(); });
    QApplication::processEvents();
    check(hook.calls == 0 && libPage->rowCount() == 1,
          QStringLiteral("P0-7：同路径重复导入静默放行（不弹确认）"));

    // ② 异路径同名 + 选择「覆盖」：钩子参数正确，A 的内容被 B 替换
    page->importPaths({paths[1]});
    waitUntil([&] { return !page->isBusy(); });
    QApplication::processEvents();
    libPage->refresh();
    showFirstRowDetail();
    check(hook.calls == 1, QStringLiteral("P0-7：异路径同名导入触发确认钩子"));
    check(hook.lastDocId == name
              && hook.lastExisting == paths[0]
              && hook.lastNew == paths[1],
          QStringLiteral("P0-7：确认钩子收到正确的 docId 与新旧路径"),
          QStringLiteral("%1 | %2 → %3")
              .arg(hook.lastDocId, hook.lastExisting, hook.lastNew));
    check(detail->toPlainText().contains(specs[1].marker)
              && !detail->toPlainText().contains(specs[0].marker),
          QStringLiteral("P0-7：选择覆盖后引擎内容替换为新文件"));

    // ③ 异路径同名 + 选择「跳过」：C 不入库，内容仍是 B
    hook.answer = SearchPage::OverwriteChoice::Skip;
    page->importPaths({paths[2]});
    waitUntil([&] { return !page->isBusy(); });
    QApplication::processEvents();
    libPage->refresh();
    showFirstRowDetail();
    check(hook.calls == 2, QStringLiteral("P0-7：跳过路径同样经过确认钩子"));
    check(detail->toPlainText().contains(specs[1].marker)
              && !detail->toPlainText().contains(specs[2].marker),
          QStringLiteral("P0-7：选择跳过后原文档内容保持不变"));

    // ④ 异路径同名（C 未入库，docId 已在库）+ 选择「取消剩余导入」
    hook.answer = SearchPage::OverwriteChoice::CancelAll;
    page->importPaths({paths[2]});
    waitUntil([&] { return !page->isBusy(); });
    QApplication::processEvents();
    check(hook.calls == 3, QStringLiteral("P0-7：取消路径同样经过确认钩子"));
    check(libPage->rowCount() == 1, QStringLiteral("P0-7：取消后文档数不变"));

    window.close();
    QApplication::processEvents();

    // 收尾：清掉本组写出的索引与临时文件
    QFile::remove(kSuiteIndexPath);
    for (const auto& spec : specs) {
        QDir(QDir::current().absoluteFilePath(spec.dir)).removeRecursively();
    }
    return g_failures;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
// T3 设置 E2E：改参数 → 保存 → 检索得分可观测变化 → 重启保留 → 恢复默认
// ═══════════════════════════════════════════════════════════════
namespace {

/// 解析结果列表第一项里的「相关度: X.XX」（displayResults 以 2 位小数固定格式输出）
double firstRelevance(QListWidget* resultList, QString* debugText = nullptr) {
    if (debugText) {
        *debugText = QStringLiteral("count=%1")
                         .arg(resultList ? resultList->count() : -1);
        if (resultList && resultList->count() > 0) {
            *debugText += QStringLiteral(" 首项[%1]")
                              .arg(resultList->item(0)->text().left(60));
        }
    }
    if (!resultList || resultList->count() == 0) return -1.0;
    const QString text = resultList->item(0)->text();
    const int pos = text.indexOf(QStringLiteral("相关度: "));
    if (pos < 0) return -1.0;
    // QString::toDouble 要求整串均为数字，先截出 "10.96" 这一段
    const QString token = text.mid(pos + 5).split(QLatin1Char(' '),
                                                  Qt::SkipEmptyParts).value(0);
    bool ok = false;
    const double value = token.toDouble(&ok);
    return ok ? value : -1.0;
}

int runSettingsE2E(const QString& corpusDir) {
    const QString settingsPath = QString::fromStdString(config::dataFilePath(config::SETTINGS_FILE));   // P3：exe 目录
    const QString backupPath = settingsPath + QStringLiteral(".e2e_backup");
    // T4：rag_settings.json 现在含用户真实 Key，绝不能删——先备份，跑完还原。
    // 工作文件照旧删除（保证从默认值起步的断言语义不变），用户数据零风险。
    // 若上一轮异常中断留下备份，先还原再重新备份。
    if (QFile::exists(backupPath)) {
        QFile::remove(settingsPath);
        QFile::copy(backupPath, settingsPath);
        QFile::remove(backupPath);
    }
    const bool hadUserSettings = QFile::exists(settingsPath);
    if (hadUserSettings) {
        check(QFile::copy(settingsPath, backupPath),
              QStringLiteral("用户配置已备份（含 Key，不出仓库不出日志）"));
        QFile::remove(settingsPath);   // 工作文件照旧删除：保证从默认值起步（无 Key 纯 BM25）
    }

    const QStringList corpus = collectCorpus(corpusDir);
    if (corpus.isEmpty()) {
        check(false, QStringLiteral("语料目录可访问"));
        return g_failures;
    }

    // ── 会话 1：默认起步 → 改 k1 → 保存 → 得分可观测变化 ──
    double scoreBefore = -1.0, scoreAfter = -1.0;
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        SearchPage* searchPage = window.findChild<SearchPage*>();
        SettingsPage* settingsPage = window.findChild<SettingsPage*>();
        QDoubleSpinBox* k1Spin = window.findChild<QDoubleSpinBox*>("settingsK1");
        QListWidget* resultList = window.findChild<QListWidget*>("resultList");
        QLineEdit* searchInput = window.findChild<QLineEdit*>("searchInput");
        QPushButton* searchBtn = window.findChild<QPushButton*>("searchBtn");
        QPushButton* saveBtn = window.findChild<QPushButton*>("settingsSaveBtn");
        QListWidget* navList = window.findChild<QListWidget*>("navList");

        if (!searchPage || !settingsPage || !k1Spin || !resultList || !searchInput
            || !searchBtn || !saveBtn || !navList) {
            check(false, QStringLiteral("设置页控件齐全"), QStringLiteral("存在控件未找到"));
            return g_failures;
        }

        check(!QFile::exists(settingsPath), QStringLiteral("起点：无配置文件（默认值起步）"));
        check(std::fabs(k1Spin->value() - 1.5) < 1e-9,
              QStringLiteral("设置页表单回填默认 k1=1.5"),
              QStringLiteral("实际 %1").arg(k1Spin->value()));

        // 首次检索（默认 k1=1.5）
        searchPage->importPaths(corpus);
        waitUntil([&] { return !searchPage->isBusy(); });
        QApplication::processEvents();
        searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
        searchBtn->click();
        pump(2500);
        QString dbg;
        scoreBefore = firstRelevance(resultList, &dbg);
        check(scoreBefore > 0, QStringLiteral("默认参数下检索有得分"),
              QStringLiteral("相关度 %1 (%2)").arg(scoreBefore).arg(dbg));

        // 改 k1 1.5 → 4.0 并保存（走完整链路：写盘 → 信号 → 引擎热更新）
        k1Spin->setValue(4.0);
        clickNavItem(navList, 6);   // T11 后设置页在导航第 6 行（第 7 页）
        QApplication::processEvents();
        saveBtn->click();
        QApplication::processEvents();

        check(QFile::exists(settingsPath), QStringLiteral("保存后配置文件已落盘"));
        {
            QFile file(settingsPath);
            file.open(QIODevice::ReadOnly);
            const QByteArray raw = file.readAll();
            file.close();
            check(raw.contains("\"k1\": 4"), QStringLiteral("配置文件内容 k1=4"),
                  QString::fromUtf8(raw.left(40)));
        }

        // 同一查询再检索：k1 变了，相关度必须可观测地不同
        clickNavItem(navList, 0);
        QApplication::processEvents();
        searchBtn->click();
        pump(2500);
        scoreAfter = firstRelevance(resultList);
        check(scoreAfter > 0 && std::fabs(scoreBefore - scoreAfter) > 0.005,
              QStringLiteral("改 k1 后同查询得分可观测变化"),
              QStringLiteral("%1 → %2").arg(scoreBefore).arg(scoreAfter));

        // 截图留证（T3 新证据）
        clickNavItem(navList, 6);
        QApplication::processEvents();
        window.grab().save(QStringLiteral("docs/screenshots/13-settings-modified.png"));
    }

    // ── 会话 2：重启 → 配置保留 → 恢复默认 ──
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        QDoubleSpinBox* k1Spin = window.findChild<QDoubleSpinBox*>("settingsK1");
        SettingsPage* settingsPage = window.findChild<SettingsPage*>();
        QPushButton* saveBtn = window.findChild<QPushButton*>("settingsSaveBtn");
        QPushButton* defaultsBtn = window.findChild<QPushButton*>("settingsDefaultsBtn");
        QListWidget* navList = window.findChild<QListWidget*>("navList");

        if (!k1Spin || !settingsPage || !saveBtn || !defaultsBtn || !navList) {
            check(false, QStringLiteral("会话 2 设置页控件齐全"));
            return g_failures;
        }

        check(std::fabs(k1Spin->value() - 4.0) < 1e-9,
              QStringLiteral("重启程序后配置保留（表单 k1=4.0）"),
              QStringLiteral("实际 %1").arg(k1Spin->value()));

        // 恢复默认值 → 保存 → 文件回到 1.5（防误触设计：先回填再保存）
        clickNavItem(navList, 6);
        QApplication::processEvents();
        defaultsBtn->click();
        QApplication::processEvents();
        saveBtn->click();
        QApplication::processEvents();

        config::AppSettings reloaded;
        config::AppSettings::load(config::SETTINGS_FILE, reloaded);
        check(std::fabs(reloaded.k1 - 1.5) < 1e-9 && reloaded.topK == 20,
              QStringLiteral("恢复默认并保存后配置回到 k1=1.5 / TopK=20"),
              QStringLiteral("k1=%1 topK=%2").arg(reloaded.k1).arg(reloaded.topK));
    }

    // 收尾：删掉测试期间写出的工作配置，还原用户自己的配置（如有）
    QFile::remove(settingsPath);
    if (hadUserSettings && QFile::exists(backupPath)) {
        QFile::copy(backupPath, settingsPath);
        QFile::remove(backupPath);
    }
    return g_failures;
}

/// T4 质量分析 E2E：四路并列对比 → 降级语义（无 Key）→ 批量评测出表。
/// 无 Key 环境下全量断言（确定性）；有 Key 时本组只做真实调用的冒烟
/// （向量列非空即过），批量评测不做数值断言——避免消耗大量代金券。
int runQualityE2E(const QString& corpusDir) {
    // 起步清掉工作目录残留索引，保证从语料全量导入（此刻无窗口持有该文件，安全）
    QFile::remove(kSuiteIndexPath);

    MainWindow window;
    window.resize(1180, 760);
    window.show();
    QApplication::processEvents();

    SearchPage* searchPage = window.findChild<SearchPage*>();
    QualityPage* qualityPage = window.findChild<QualityPage*>();
    QListWidget* navList = window.findChild<QListWidget*>("navList");
    QLineEdit* queryInput = window.findChild<QLineEdit*>("qualityQueryInput");
    QPushButton* runBtn = window.findChild<QPushButton*>("qualityRunBtn");
    QPushButton* evalBtn = window.findChild<QPushButton*>("qualityEvalBtn");
    QListWidget* listBm25 = window.findChild<QListWidget*>("qualityListBm25");
    QListWidget* listVector = window.findChild<QListWidget*>("qualityListVector");
    QListWidget* listWeighted = window.findChild<QListWidget*>("qualityListWeighted");
    QListWidget* listRrf = window.findChild<QListWidget*>("qualityListRrf");
    QTableWidget* metricsTable = window.findChild<QTableWidget*>("qualityMetricsTable");
    QLabel* serviceHint = window.findChild<QLabel*>("qualityServiceHint");

    if (!searchPage || !navList || !queryInput || !runBtn || !evalBtn
        || !listBm25 || !listVector || !listWeighted || !listRrf
        || !metricsTable || !serviceHint) {
        check(false, QStringLiteral("质量页控件齐全"), QStringLiteral("存在控件未找到"));
        return g_failures;
    }

    clickNavItem(navList, 3);
    QApplication::processEvents();

    // 导入语料（引擎唯一，检索页导入 → 质量页立即可用）
    const QStringList corpus = collectCorpus(corpusDir);
    if (corpus.isEmpty()) {
        check(false, QStringLiteral("语料目录可访问"));
        return g_failures;
    }
    searchPage->importPaths(corpus);
    waitUntil([&] { return !searchPage->isBusy(); });
    QApplication::processEvents();

    const bool vectorReady = !serviceHint->text().contains(QStringLiteral("未配置"));
    if (!vectorReady) {
        check(serviceHint->text().contains(QStringLiteral("降级")),
              QStringLiteral("提示行：Embedding 未配置时明示降级语义"),
              serviceHint->text());
    }

    // ── 单查询四路对比 ──
    queryInput->setText(QStringLiteral("民间借贷 交付凭证"));
    runBtn->click();
    waitUntil([&] { return qualityPage && !qualityPage->isBusy(); });

    check(listBm25->count() >= 1 && listWeighted->count() >= 1 && listRrf->count() >= 1,
          QStringLiteral("四路对比：BM25 / 加权 / RRF 列有结果"),
          QStringLiteral("%1/%2/%3").arg(listBm25->count())
              .arg(listWeighted->count()).arg(listRrf->count()));

    if (!vectorReady) {
        // ② 向量列显示降级说明而非冒充结果
        bool vectorDegraded = false;
        for (int i = 0; i < listVector->count(); ++i) {
            if (listVector->item(i)->text().contains(QStringLiteral("未配置"))) {
                vectorDegraded = true;
            }
        }
        check(vectorDegraded, QStringLiteral("四路对比：向量列明示「未配置」而非伪装结果"));

        // ③④ 降级 = ① 的排序（文本逐行一致）
        const int n = listBm25->count();
        bool sameW = listWeighted->count() == n;
        bool sameR = listRrf->count() == n;
        for (int i = 0; i < n && (sameW || sameR); ++i) {
            const QString t = listBm25->item(i)->text();
            sameW = sameW && listWeighted->item(i)->text() == t;
            sameR = sameR && listRrf->item(i)->text() == t;
        }
        check(sameW, QStringLiteral("降级下加权融合列与 BM25 列逐行一致"));
        check(sameR, QStringLiteral("降级下 RRF 列与 BM25 列逐行一致"));
    }

    // ── 批量评测（P1：引擎线程异步执行，等待收尾）──
    evalBtn->click();
    waitUntil([&] { return qualityPage && !qualityPage->isBusy(); });

    check(metricsTable->rowCount() == 4,
          QStringLiteral("指标表 4 行（四路）"),
          QStringLiteral("实际 %1 行").arg(metricsTable->rowCount()));

    // 数值列可解析且在 [0,1]；无 Key 时 ③④ 的 Hit@5/R@10/MRR 应与 ① 相等
    auto cell = [metricsTable](int row, int col) {
        auto* item = metricsTable->item(row, col);
        return item ? item->text().toDouble() : -1.0;
    };
    bool inRange = true;
    for (int row = 0; row < 4; ++row) {
        for (int col = 1; col <= 4; ++col) {
            const double v = cell(row, col);
            inRange = inRange && v >= 0.0 && v <= 1.0;
        }
    }
    check(inRange, QStringLiteral("指标表 12 个数值均可解析且在 [0,1]"));

    if (!vectorReady) {
        const bool degradedEqual =
            std::fabs(cell(0, 2) - cell(2, 2)) < 1e-9 &&
            std::fabs(cell(0, 3) - cell(2, 3)) < 1e-9 &&
            std::fabs(cell(0, 4) - cell(2, 4)) < 1e-9 &&
            std::fabs(cell(0, 2) - cell(3, 2)) < 1e-9 &&
            std::fabs(cell(0, 3) - cell(3, 3)) < 1e-9 &&
            std::fabs(cell(0, 4) - cell(3, 4)) < 1e-9;
        check(degradedEqual,
              QStringLiteral("降级下加权/RRF 的 Hit@5/R@10/MRR 与 BM25 路相等"),
              QStringLiteral("① Hit@5=%1 ③=%2 ④=%3")
                  .arg(cell(0, 2)).arg(cell(2, 2)).arg(cell(3, 2)));
        check(cell(1, 2) == 0.0,
              QStringLiteral("降级下向量路 Hit@5=0（无结果）"));
    }

    // 截图留证（T4 新证据：质量分析页 + 降级指标表）
    window.grab().save(QStringLiteral("docs/screenshots/14-quality-page.png"));

    // 收尾：不留测试期间写出的索引
    QFile::remove(kSuiteIndexPath);
    return g_failures;
}

// ── T9 法条速查 E2E：导航 → 目录树 → 条号精确定位 → 关键词检索 → 主检索零污染 ──
int runLawE2E(MainWindow& window, const QString& outDir) {
    pump(30);

    QListWidget* navList = window.findChild<QListWidget*>(QStringLiteral("navList"));
    QStackedWidget* stack = window.findChild<QStackedWidget*>();
    if (!navList || !stack) {
        check(false, QStringLiteral("T9：导航容器存在"));
        return g_failures;
    }

    // ── 导航到第 5 页（06-law，T11 后法条页在导航第 5 行）──
    clickNavItem(navList, 5);
    check(stack->currentIndex() == 5,
          QStringLiteral("T9：导航 06-law → 第 5 页"),
          QStringLiteral("stack=%1").arg(stack->currentIndex()));

    auto* queryInput = window.findChild<QLineEdit*>(QStringLiteral("lawQueryInput"));
    auto* searchBtn = window.findChild<QPushButton*>(QStringLiteral("lawSearchBtn"));
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("lawTree"));
    auto* resultList = window.findChild<QListWidget*>(QStringLiteral("lawResultList"));
    auto* detail = window.findChild<QTextBrowser*>(QStringLiteral("lawDetail"));
    auto* stats = window.findChild<QLabel*>(QStringLiteral("lawStats"));
    check(queryInput && searchBtn && tree && resultList && detail && stats,
          QStringLiteral("T9：法条页控件齐全"));

    law::LawStore* store = window.lawStore();
    check(store != nullptr && store->isOpen() &&
              store->lawCount() == 3 && store->articleCount() == 284,
          QStringLiteral("T9：法条库加载 3 部 / 284 条"),
          store ? QStringLiteral("%1 部 / %2 条")
                      .arg(store->lawCount()).arg(store->articleCount())
                : QStringLiteral("store 为空"));
    check(stats != nullptr && stats->text().contains(QStringLiteral("3 部法律 / 284 条")),
          QStringLiteral("T9：统计标签如实显示收录规模"));
    check(tree != nullptr && tree->topLevelItemCount() == 3,
          QStringLiteral("T9：目录树 3 部法律"),
          tree ? QStringLiteral("%1").arg(tree->topLevelItemCount()) : QString());

    // ── 主检索零污染基线 ──
    rag::Retriever* mainRetriever = window.engineWorker()->retriever();
    const int docsBefore = mainRetriever->documentCount();
    const int chunksBefore = mainRetriever->chunkCount();
    const auto docIdsBefore = mainRetriever->allDocIds();

    // ── 1) 条号精确定位（法律名 + 条号）──
    if (queryInput && searchBtn && detail) {
        queryInput->setText(QStringLiteral("民法典 第一千零七十九条"));
        searchBtn->click();
        pump(50);
        const QString text = detail->toPlainText();
        check(text.contains(QStringLiteral("第一千零七十九条")) &&
                  text.contains(QStringLiteral("离婚诉讼")),
              QStringLiteral("T9：条号精确定位（民法典 第一千零七十九条）"));
        check(text.contains(QStringLiteral("文本来源")),
              QStringLiteral("T9：详情页脚带来源说明"));
    }

    // ── 2) 仅条号 → 跨法律定位（劳动法 第一百零七条；劳动合同法仅 98 条不命中）──
    if (queryInput && searchBtn && detail) {
        queryInput->setText(QStringLiteral("第一百零七条"));
        searchBtn->click();
        pump(50);
        check(detail->toPlainText().contains(QStringLiteral("第一百零七条")),
              QStringLiteral("T9：仅条号跨法律定位（第一百零七条）"));
    }

    // ── 3) 未收录条号如实提示（不静默转关键词）──
    if (queryInput && searchBtn && detail) {
        queryInput->setText(QStringLiteral("民法典 第九百九十九条"));
        searchBtn->click();
        pump(50);
        check(detail->toPlainText().contains(QStringLiteral("未收录")),
              QStringLiteral("T9：未收录条号如实提示"));
    }

    // ── 4) 关键词检索 → 结果列表 → 选中看详情 ──
    if (queryInput && searchBtn && resultList && detail) {
        queryInput->setText(QStringLiteral("试用期 不得超过"));
        searchBtn->click();
        pump(80);
        check(resultList->isVisible() && resultList->count() > 0,
              QStringLiteral("T9：关键词检索出结果列表"),
              QStringLiteral("%1 条").arg(resultList->count()));
        if (resultList->count() > 0) {
            resultList->setCurrentRow(0);
            QApplication::processEvents();
            check(detail->toPlainText().contains(QStringLiteral("试用期")),
                  QStringLiteral("T9：结果选中后详情展示条文"));
        }
    }

    // ── 5) 主检索零污染断言（T9 选型：独立实例，主引擎必须纹丝不动）──
    check(mainRetriever->documentCount() == docsBefore &&
              mainRetriever->chunkCount() == chunksBefore,
          QStringLiteral("T9：主引擎文档/块计数零漂移"),
          QStringLiteral("%1/%2 → %3/%4")
              .arg(docsBefore).arg(chunksBefore)
              .arg(mainRetriever->documentCount())
              .arg(mainRetriever->chunkCount()));
    check(mainRetriever->allDocIds() == docIdsBefore,
          QStringLiteral("T9：主引擎 docId 集合不变"));
    bool lawLeak = false;
    for (const auto& r : mainRetriever->search(QStringLiteral("试用期 不得超过").toStdString(), 20)) {
        if (QString::fromStdString(r.docId).contains(QLatin1Char('#'))) {
            lawLeak = true;
        }
    }
    check(!lawLeak, QStringLiteral("T9：主检索结果无法条 docId（零污染）"));

    // 截图留证
    check(window.grab().save(outDir + QStringLiteral("/16-law-page.png")),
          QStringLiteral("T9：截图 16-law-page"));

    // 回到检索页，不影响后续分组
    clickNavItem(navList, 0);
    return g_failures;
}

// ── T11 案例收藏 E2E：导航 → 收藏 → 分组 → 删除文档失效标记 → 重启仍在 ──
//
// 说明：本组验证走的是 MainWindow 默认路径（config::FAVORITE_DB，即 exe 所在
// 目录下的 favorites.db），跑完清掉全部分组（removeCase 级联删收藏），不与
// 用户手工攒的类案集混淆。删除文档联动只走脚本路径（confirm=false），
// 不弹模态框，离屏环境可无人值守跑完。
int runFavoritesE2E(const QString& corpusDir, const QString& outDir) {
    long long caseId = 0;
    std::string favoritedDocId;

    // ── 会话 1：导入 → 检索 → 收藏 → 删除文档 → 失效标记 ──
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        SearchPage* searchPage = window.findChild<SearchPage*>();
        LibraryPage* libPage = window.findChild<LibraryPage*>();
        FavoritesPage* favPage = window.findChild<FavoritesPage*>();
        favorite::FavoriteStore* store = window.favoriteStore();
        QListWidget* navList = window.findChild<QListWidget*>(QStringLiteral("navList"));
        QStackedWidget* stack = window.findChild<QStackedWidget*>();
        QListWidget* resultList = window.findChild<QListWidget*>(QStringLiteral("resultList"));
        QLineEdit* searchInput = window.findChild<QLineEdit*>(QStringLiteral("searchInput"));
        QPushButton* searchBtn = window.findChild<QPushButton*>("searchBtn");

        if (!searchPage || !libPage || !favPage || !store || !navList || !stack
            || !resultList || !searchInput || !searchBtn) {
            check(false, QStringLiteral("T11：收藏 E2E 控件齐全"),
                  QStringLiteral("存在控件未找到"));
            return g_failures;
        }

        // 从空收藏起步：走 SQL 层清理（本进程连接开着，删库文件在 Windows 上必失败）
        for (const auto& c : store->cases()) {
            store->removeCase(c.id);
        }
        check(store->caseCount() == 0, QStringLiteral("T11：起始收藏为空"));

        // 导入语料 + 真实检索（命中结果作为收藏来源）
        searchPage->importPaths(collectCorpus(corpusDir));
        waitUntil([&] { return !searchPage->isBusy(); });
        QApplication::processEvents();
        searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
        searchBtn->click();
        pump(2000);
        check(searchPage->lastResultCount() > 0,
              QStringLiteral("T11：检索有命中（作为收藏来源）"),
              QStringLiteral("%1 条").arg(searchPage->lastResultCount()));

        // 主引擎计数基线：收藏操作不得触碰索引
        rag::Retriever* mainRetriever = window.engineWorker()->retriever();
        const int docsBefore = mainRetriever->documentCount();
        const int chunksBefore = mainRetriever->chunkCount();

        // 新建分组 + 收藏第 0 行（与右键菜单同一存储链路，含 item data 快照）
        caseId = store->createCase("E2E·民间借贷类案");
        check(caseId > 0, QStringLiteral("T11：新建案件分组"));
        const long long favId = searchPage->favoriteResultRow(0, caseId);
        check(favId > 0, QStringLiteral("T11：收藏检索结果第 0 行"));

        // favoriteChanged 经 MainWindow 中转 → 收藏页自动刷新
        check(favPage->caseCount() == 1,
              QStringLiteral("T11：收藏页分组数 = 1（信号中转刷新）"),
              QStringLiteral("%1 个").arg(favPage->caseCount()));
        check(favPage->currentFavoriteCount() == 1,
              QStringLiteral("T11：当前分组收藏数 = 1"),
              QStringLiteral("%1 条").arg(favPage->currentFavoriteCount()));
        check(favPage->currentCaseName() == QStringLiteral("E2E·民间借贷类案"),
              QStringLiteral("T11：当前分组名正确"));

        // 收藏后主引擎纹丝不动（收藏与索引物理隔离）
        check(mainRetriever->documentCount() == docsBefore
                  && mainRetriever->chunkCount() == chunksBefore,
              QStringLiteral("T11：主引擎文档/块计数零漂移"));

        // 状态列应为「正常」
        check(favPage->cellText(0, 2) == QStringLiteral("正常"),
              QStringLiteral("T11：收藏初始状态为「正常」"),
              favPage->cellText(0, 2));

        // 详情区显示片段快照（收藏当时的块内容）
        QTextBrowser* detail =
            window.findChild<QTextBrowser*>(QStringLiteral("favoriteDetail"));
        check(detail != nullptr
                  && detail->toPlainText().contains(QStringLiteral("本院认为")),
              QStringLiteral("T11：详情区显示片段快照"));

        // 记住被收藏文书的 docId，随后删除它
        const auto items = store->favoritesOf(caseId);
        if (!items.empty()) {
            favoritedDocId = items.front().docId;
        }
        check(!favoritedDocId.empty(), QStringLiteral("T11：收藏项定位到 docId"));

        // 导航到收藏页截图留证
        clickNavItem(navList, 4);
        check(stack->currentIndex() == 4,
              QStringLiteral("T11：导航 05-favorites → 第 4 页"),
              QStringLiteral("stack=%1").arg(stack->currentIndex()));
        check(window.grab().save(outDir + QStringLiteral("/17-favorites-page.png")),
              QStringLiteral("T11：截图 17-favorites-page"));

        // 回文档库删除该文书 → 收藏应标记失效（不删项、快照保留）
        clickNavItem(navList, 1);
        QApplication::processEvents();
        const bool removed = libPage->removeDocumentById(
            QString::fromStdString(favoritedDocId), /*confirm=*/false);
        check(removed, QStringLiteral("T11：删除已收藏文书成功"));

        // 回收藏页：条目仍在，状态翻转「原文已删除」
        clickNavItem(navList, 4);
        QApplication::processEvents();
        check(favPage->currentFavoriteCount() == 1,
              QStringLiteral("T11：删除后收藏项仍在（标记失效而非删除）"),
              QStringLiteral("%1 条").arg(favPage->currentFavoriteCount()));
        check(favPage->cellText(0, 2) == QStringLiteral("原文已删除"),
              QStringLiteral("T11：状态列显示「原文已删除」"),
              favPage->cellText(0, 2));
        check(detail->toPlainText().contains(QStringLiteral("原文已删除"))
                  && detail->toPlainText().contains(QStringLiteral("快照")),
              QStringLiteral("T11：详情标注原文已删除且快照可读"));

        // 关闭窗口（closeEvent 照常落盘索引，与其它分组一致）
        window.close();
        QApplication::processEvents();
    }

    // ── 会话 2：全新窗口 = 重启后，收藏与失效标记必须还在 ──
    {
        MainWindow window;
        window.resize(1180, 720);
        window.show();
        QApplication::processEvents();

        FavoritesPage* favPage = window.findChild<FavoritesPage*>();
        favorite::FavoriteStore* store = window.favoriteStore();
        if (!favPage || !store) {
            check(false, QStringLiteral("T11：会话 2 页面就绪"));
            return g_failures;
        }

        check(store->caseCount() == 1,
              QStringLiteral("T11：重启后分组仍在（持久化）"),
              QStringLiteral("%1 个").arg(store->caseCount()));
        const auto items = store->favoritesOf(caseId);
        check(items.size() == 1 && !items.front().valid
                  && !items.front().snippet.empty(),
              QStringLiteral("T11：重启后失效标记与快照仍在"));
        favPage->refresh();
        check(favPage->currentFavoriteCount() == 1,
              QStringLiteral("T11：重启后收藏页显示 1 条"),
              QStringLiteral("%1 条").arg(favPage->currentFavoriteCount()));

        window.close();
        QApplication::processEvents();
    }

    // 收尾：清掉测试分组（级联删收藏）+ 本组写出的索引，不留测试数据
    {
        favorite::FavoriteStore cleanup(
            QString::fromStdString(config::dataFilePath(config::FAVORITE_DB))
                .toStdString());
        if (cleanup.open()) {
            for (const auto& c : cleanup.cases()) {
                cleanup.removeCase(c.id);
            }
        }
    }
    QFile::remove(kSuiteIndexPath);
    return g_failures;
}

}  // namespace

// ── T10 全文阅读 E2E：检索 → 打开弹窗 → 定位高亮 → 只读零污染 → 越界安全 ──
//
// 说明：与双击/右键菜单共用 openFullTextForItem 同一链路（E2E 走行号入口，
// 离屏环境不模拟真实双击手势——手感归人工手测）。弹窗非模态，断言完即关。
int runDocViewerE2E(const QString& corpusDir, const QString& outDir) {
    MainWindow window;
    window.resize(1180, 720);
    window.show();
    QApplication::processEvents();

    SearchPage* searchPage = window.findChild<SearchPage*>();
    QListWidget* resultList = window.findChild<QListWidget*>(QStringLiteral("resultList"));
    QLineEdit* searchInput = window.findChild<QLineEdit*>(QStringLiteral("searchInput"));
    QPushButton* searchBtn = window.findChild<QPushButton*>(QStringLiteral("searchBtn"));
    if (!searchPage || !resultList || !searchInput || !searchBtn) {
        check(false, QStringLiteral("T10：全文阅读 E2E 控件齐全"),
              QStringLiteral("控件缺失"));
        return g_failures;
    }

    // 导入语料 + 真实检索（命中结果作为打开全文的来源）
    searchPage->importPaths(collectCorpus(corpusDir));
    waitUntil([&] { return !searchPage->isBusy(); });
    QApplication::processEvents();
    searchInput->setText(QStringLiteral("民间借贷 交付凭证"));
    searchBtn->click();
    pump(2000);
    check(searchPage->lastResultCount() > 0,
          QStringLiteral("T10：检索有命中（作为打开全文的来源）"),
          QStringLiteral("%1 条").arg(searchPage->lastResultCount()));

    // 主引擎计数基线：全文阅读只读，不得触碰索引
    rag::Retriever* mainRetriever = window.engineWorker()->retriever();
    const int docsBefore = mainRetriever->documentCount();
    const int chunksBefore = mainRetriever->chunkCount();

    const QString expectDocId =
        resultList->item(0)->data(Qt::UserRole + 1).toString();
    check(!expectDocId.isEmpty(),
          QStringLiteral("T10：结果项携带 docId"));

    check(searchPage->openFullTextForRow(0),
          QStringLiteral("T10：打开全文（第 0 行，与双击/右键同链路）"));

    DocViewerDialog* viewer = window.findChild<DocViewerDialog*>();
    check(viewer != nullptr, QStringLiteral("T10：全文弹窗已创建"));
    if (!viewer) {
        return g_failures;
    }

    check(viewer->documentTitle() == expectDocId,
          QStringLiteral("T10：弹窗标题 = 命中文书 docId"),
          viewer->documentTitle());
    check(viewer->locatedCount() >= 1,
          QStringLiteral("T10：命中块定位 ≥ 1 处"),
          QStringLiteral("%1 处").arg(viewer->locatedCount()));

    QPlainTextEdit* textView =
        viewer->findChild<QPlainTextEdit*>(QStringLiteral("docViewerText"));
    QListWidget* hitList =
        viewer->findChild<QListWidget*>(QStringLiteral("docViewerHitList"));
    check(textView != nullptr
              && textView->toPlainText().contains(QStringLiteral("本院认为")),
          QStringLiteral("T10：正文为整篇文书原文（含「本院认为」）"));
    check(hitList != nullptr && hitList->count() == viewer->locatedCount(),
          QStringLiteral("T10：命中列表条目数 = 定位数"),
          QStringLiteral("list=%1 located=%2")
              .arg(hitList ? hitList->count() : -1)
              .arg(viewer->locatedCount()));
    check(hitList != nullptr && hitList->currentRow() == 0,
          QStringLiteral("T10：默认定位第一处命中"),
          QStringLiteral("row=%1").arg(hitList ? hitList->currentRow() : -2));

    check(viewer->grab().save(outDir + QStringLiteral("/18-doc-viewer.png")),
          QStringLiteral("T10：截图 18-doc-viewer"));

    check(mainRetriever->documentCount() == docsBefore
              && mainRetriever->chunkCount() == chunksBefore,
          QStringLiteral("T10：只读零污染（文档/块计数不变）"));

    viewer->close();
    QApplication::processEvents();  // WA_DeleteOnClose → 弹窗销毁
    viewer = nullptr;

    check(!searchPage->openFullTextForRow(resultList->count() + 5),
          QStringLiteral("T10：越界行号安全返回 false（不崩溃）"));

    // ── S3：收藏页「打开全文」入口 ──
    favorite::FavoriteStore* store = window.favoriteStore();
    FavoritesPage* favPage = window.findChild<FavoritesPage*>();
    QPushButton* openFullBtn =
        window.findChild<QPushButton*>(QStringLiteral("favoriteOpenFullTextBtn"));
    if (store && favPage && openFullBtn) {
        // 起步清空（走 SQL 层，与 T11 组同策略），再攒一条收藏
        for (const auto& c : store->cases()) {
            store->removeCase(c.id);
        }
        const long long caseId = store->createCase("E2E·全文阅读类案");
        check(caseId > 0, QStringLiteral("T10-S3：新建分组"));
        check(searchPage->favoriteResultRow(0, caseId) > 0,
              QStringLiteral("T10-S3：收藏检索第 0 行"));
        QApplication::processEvents();  // favoriteChanged 信号中转刷新收藏页

        // 收藏页默认选中首项（T11 修复的语义）→ 按钮应已启用
        check(openFullBtn->isEnabled(),
              QStringLiteral("T10-S3：选中收藏后「打开全文」按钮启用"));

        openFullBtn->click();
        QApplication::processEvents();
        DocViewerDialog* favViewer = window.findChild<DocViewerDialog*>();
        check(favViewer != nullptr,
              QStringLiteral("T10-S3：收藏页打开全文弹窗"));
        if (favViewer) {
            check(favViewer->documentTitle() == expectDocId,
                  QStringLiteral("T10-S3：弹窗标题 = 收藏的 docId"),
                  favViewer->documentTitle());
            check(favViewer->locatedCount() >= 1,
                  QStringLiteral("T10-S3：收藏块号已定位"),
                  QStringLiteral("%1 处").arg(favViewer->locatedCount()));
            favViewer->close();
            QApplication::processEvents();
        }

        // 收尾：清掉本组测试分组（级联删收藏），不留测试数据
        store->removeCase(caseId);
    } else {
        check(false, QStringLiteral("T10-S3：收藏页控件齐全"),
              QStringLiteral("控件缺失"));
    }

    // ── S4：大文书性能实测（约 17 万字符，量级 = 数十页文书 / 扫描件 OCR 全文）──
    {
        const QString bigPath = outDir + QStringLiteral("/t10_big_doc.txt");
        QFile bigFile(bigPath);
        QString body;
        for (int i = 0; i < 600; ++i) {
            body += QStringLiteral(
                "第%1段：本院经审理查明，原告与被告之间的民间借贷关系依法成立，"
                "被告应依约偿还借款本金及利息，逾期未还应承担违约责任。\n").arg(i);
        }
        check(bigFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
                  && bigFile.write(body.toUtf8()) > 0,
              QStringLiteral("T10-S4：构造大文书临时文件"));
        bigFile.close();

        searchPage->importPaths({bigPath});
        waitUntil([&] { return !searchPage->isBusy(); });
        QApplication::processEvents();

        const QString bigDocId = QFileInfo(bigPath).fileName();
        QElapsedTimer sw;
        sw.start();
        // 直接构造弹窗测打开性能（大文书不在检索结果里，不走行号入口）；
        // 命中块传空 → 只加载全文不做高亮，量的是最坏路径里的主开销
        DocViewerDialog bigViewer(mainRetriever, bigDocId, {}, &window);
        const qint64 openMs = sw.elapsed();

        QPlainTextEdit* bigText =
            bigViewer.findChild<QPlainTextEdit*>(QStringLiteral("docViewerText"));
        check(bigText != nullptr
                  && bigText->toPlainText().size() == body.size(),
              QStringLiteral("T10-S4：大文书全文加载完整（字符数与原文逐一相等）"),
              QStringLiteral("viewer=%1 expected=%2")
                  .arg(bigText ? bigText->toPlainText().size() : 0)
                  .arg(body.size()));
        check(openMs < 3000,
              QStringLiteral("T10-S4：大文书打开耗时 < 3 s（任务书级门槛）"),
              QStringLiteral("%1 ms").arg(openMs));
        check(bigViewer.grab().save(outDir + QStringLiteral("/19-doc-viewer-big.png")),
              QStringLiteral("T10-S4：截图 19-doc-viewer-big"));

        QFile::remove(bigPath);  // 探针文件用完即删
    }

    // ── T10.1：单击片段弹窗（AI 回答区不被覆盖）+ 收藏入口可见化 ──
    {
        QTextEdit* aiAnswer =
            window.findChild<QTextEdit*>(QStringLiteral("aiAnswer"));
        // 先在回答区注入标记文本——原缺陷正是单击会用片段覆盖这里
        const QString marker = QStringLiteral("AI 回答原文·对照保护标记");
        if (aiAnswer) {
            aiAnswer->setPlainText(marker);
        }

        check(searchPage->openSnippetForRow(0),
              QStringLiteral("T10.1：单击打开片段弹窗（第 0 行）"));
        SnippetDialog* snippet = window.findChild<SnippetDialog*>();
        check(snippet != nullptr, QStringLiteral("T10.1：片段弹窗已创建"));
        if (!snippet) {
            return g_failures;
        }

        check(snippet->snippetText()
                  == resultList->item(0)->data(Qt::UserRole).toString(),
              QStringLiteral("T10.1：片段内容 = 结果条目的块原文"));
        const QString aiAfter =
            aiAnswer ? aiAnswer->toPlainText() : QString();
        check(aiAfter == marker,
              QStringLiteral("T10.1：AI 回答区逐字不变（未被片段覆盖）"),
              aiAfter.left(20));

        // 片段弹窗「查看全文」→ DocViewerDialog 拉起
        QPushButton* fullBtn = snippet->findChild<QPushButton*>(
            QStringLiteral("snippetFullTextBtn"));
        check(fullBtn != nullptr,
              QStringLiteral("T10.1：片段弹窗「查看全文」按钮存在"));
        if (fullBtn) {
            fullBtn->click();
            QApplication::processEvents();
            DocViewerDialog* fullViewer = window.findChild<DocViewerDialog*>();
            check(fullViewer != nullptr,
                  QStringLiteral("T10.1：片段弹窗「查看全文」拉起全文弹窗"));
            if (fullViewer) {
                check(fullViewer->documentTitle() == expectDocId,
                      QStringLiteral("T10.1：全文弹窗标题与片段同源 docId"),
                      fullViewer->documentTitle());
                fullViewer->close();
                QApplication::processEvents();
            }
        }
        snippet->close();
        QApplication::processEvents();

        // 收藏入口可见化：结果列表标题行常驻按钮
        QPushButton* favCurrentBtn = window.findChild<QPushButton*>(
            QStringLiteral("favoriteCurrentBtn"));
        check(favCurrentBtn != nullptr && favCurrentBtn->isEnabled(),
              QStringLiteral("T10.1：「⭐ 收藏当前」按钮可见且可用"));
    }

    window.close();
    QApplication::processEvents();
    return g_failures;
}

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

    // P3：用户设置全局隔离——各分组必须在默认参数下运行（迁移进 build/ 的
    // 开发者配置若带 Embedding Key 会意外激活向量路，吞掉 fake 的挂起标记）。
    // 设置组自带备份/还原逻辑，但其余分组没有；统一在套件层处理。
    const QString userSettingsPath =
        QString::fromStdString(config::dataFilePath(config::SETTINGS_FILE));
    const QString userSettingsBackup = userSettingsPath + QStringLiteral(".suite_backup");
    const bool hadUserSettings = QFile::exists(userSettingsPath);
    if (hadUserSettings) {
        QFile::remove(userSettingsBackup);
        check(QFile::copy(userSettingsPath, userSettingsBackup),
              QStringLiteral("套件级：用户配置已备份"));
        QFile::remove(userSettingsPath);
    }

    if (!QDir().mkpath(outDirPath)) {
        qCritical() << "cannot create output dir:" << outDirPath;
        return 2;
    }

    // T10.1 期修复（案例 C-9）：E2E 索引隔离——与上面的设置隔离同一策略。
    // ① 启动时清掉隔离文件：套件不吃任何残留/用户索引（导入组曾因启动时
    //    已有 1 篇文档而 21+1=22 假失败，2026-10-20 实测）；
    // ② 环境变量把全部窗口（共享 + 各分组自建）的索引恢复/落盘引到
    //    kSuiteIndexPath，套件 closeEvent 不再把测试语料写回用户真实
    //    rag_index.dat（此前每次跑 E2E 都会覆盖用户索引，同日实测暴露）。
    QFile::remove(kSuiteIndexPath);
    qputenv("RAG_SMOKE_INDEX_FILE", kSuiteIndexPath.toLocal8Bit());

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

        qInfo().noquote() << QStringLiteral("── 文档库页 E2E（共享引擎 → 详情 → 删除）──");
        runLibraryE2E(window, parser.value(e2eOption), outDirPath);

        qInfo().noquote() << QStringLiteral("── 索引持久化 E2E（落盘 → 重启 → 自动恢复）──");
        runPersistenceE2E(parser.value(e2eOption), outDirPath);

        // 历史库不能用删文件的方式清理（本进程主窗口的连接一直开着），
        // runHistoryE2E 内部改为行级清理，跑完不留测试数据。
        qInfo().noquote() << QStringLiteral("── 问答历史 E2E（落库 → 列表 → 关键词 → 导出 → 重启仍在）──");
        runHistoryE2E(parser.value(e2eOption), outDirPath);

        qInfo().noquote() << QStringLiteral("── 设置 E2E（改参数 → 保存 → 得分变化 → 重启保留 → 恢复默认）──");
        runSettingsE2E(parser.value(e2eOption));

        qInfo().noquote() << QStringLiteral("── 质量分析 E2E（四路对比 → 降级语义 → 批量评测）──");
        runQualityE2E(parser.value(e2eOption));

        qInfo().noquote() << QStringLiteral("── 法条速查 E2E（目录树 → 条号定位 → 关键词 → 零污染）──");
        runLawE2E(window, outDirPath);

        qInfo().noquote() << QStringLiteral("── 案例收藏 E2E（收藏 → 分组 → 删除失效标记 → 重启仍在）──");
        runFavoritesE2E(parser.value(e2eOption), outDirPath);

        qInfo().noquote() << QStringLiteral("── 全文阅读 E2E（检索 → 弹窗 → 定位高亮 → 只读零污染）──");
        runDocViewerE2E(parser.value(e2eOption), outDirPath);

        qInfo().noquote() << QStringLiteral("── P0 同名覆盖确认 E2E（覆盖 / 跳过 / 取消 + 内容一致）──");
        runOverwriteE2E();
    }

    // 套件级收尾：还原用户配置 + 清掉隔离索引文件
    if (hadUserSettings) {
        QFile::remove(userSettingsPath);
        QFile::copy(userSettingsBackup, userSettingsPath);
        QFile::remove(userSettingsBackup);
    }
    QFile::remove(kSuiteIndexPath);

    qInfo().noquote() << (g_failures == 0
                              ? QStringLiteral("UI smoke: 全部通过")
                              : QStringLiteral("UI smoke: %1 项失败").arg(g_failures));

    window.close();
    QApplication::processEvents();
    return g_failures == 0 ? 0 : 1;
}
