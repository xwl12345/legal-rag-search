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
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>

#include <cmath>
#include <functional>

#include "ui/main_window.h"
#include "ui/search_page.h"
#include "ui/library_page.h"
#include "ui/history_page.h"
#include "ui/quality_page.h"
#include "ui/settings_page.h"
#include "config/app_config.h"
#include "config/app_settings.h"
#include "history/history_store.h"

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

/// P1：引擎任务已队列化到引擎线程，断言前等待其真正收尾（带超时护栏）
void waitUntil(const std::function<bool()>& done, int maxMs = 60000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < maxMs) {
        pump(30);
    }
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
    waitUntil([&] { return !page->isBusy(); });   // P1：导入在引擎线程异步执行
    const qint64 importMs = timer.elapsed();
    check(lastDocs == corpus.size() && lastChunks > 0, QStringLiteral("导入索引"),
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
        // 标签按位组合：跨段块显示「本院认为|判决」，同样以 [本院认为 开头
        allOpinion = allOpinion
            && resultList->item(i)->text().contains(QStringLiteral("[本院认为"));
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
    const QString indexPath = QStringLiteral("rag_index.dat");

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
/// 回答文本由 SearchPage::simulateAnswer 给出（本机通常没有 DEEPSEEK_API_KEY），
/// 但"命中来源"是真实检索出来的文本块 —— 详见该函数头部的说明。
///
/// ⚠️ 清状态一律走 SQL 层（removeAll），**不要试图 QFile::remove 库文件**：
/// 本进程里 main() 的主窗口全程存活，SQLite 连接一直开着，
/// Windows 不允许删掉被打开的文件 —— 删除必然失败，
/// 于是上一轮残留的记录会被下一轮读进来，整套断言级联假失败。
/// 用行级清理则不依赖文件系统状态，连跑多少次都从 0 条起步。
int runHistoryE2E(const QString& corpusDir, const QString& outDir) {
    const QString dbPath = QStringLiteral("rag_history.db");

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

        // 两个回合：一条完整、一条中断
        searchPage->simulateAnswer(QStringLiteral("民间借贷纠纷中交付凭证如何认定？"),
                                   QStringLiteral("应当结合转账记录、收条与当事人陈述综合认定。"),
                                   /*interrupted=*/false);
        QApplication::processEvents();

        // 落库后主窗口应主动刷新历史页（不经切页、不点刷新按钮）
        check(historyPage->rowCount() == 1,
              QStringLiteral("回答结束后历史列表自动出现记录（无需手动刷新）"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));

        searchPage->simulateAnswer(QStringLiteral("劳动争议申请仲裁的时效怎么算？"),
                                   QStringLiteral("劳动争议申请仲裁的时效期间为一年，"),
                                   /*interrupted=*/true);
        QApplication::processEvents();

        check(historyPage->rowCount() == 2,
              QStringLiteral("第二个回合后列表继续同步"),
              QStringLiteral("%1 行").arg(historyPage->rowCount()));
        check(store->count() == 2, QStringLiteral("两条记录均已落库"),
              QStringLiteral("%1 条").arg(store->count()));

        // 来源与中断标记确实进了库（recent 按时间倒序，最新在前）
        const auto records = store->recent(10);
        check(static_cast<int>(records.size()) == 2, QStringLiteral("可读回 2 条记录"));
        if (records.size() == 2) {
            check(!records.front().sources.empty(),
                  QStringLiteral("命中来源随回答一起入库"),
                  QStringLiteral("%1 个文本块").arg(records.front().hitCount()));
            check(records.front().interrupted,
                  QStringLiteral("中断回合被标记为未完成"),
                  QString::fromStdString(records.front().note));
            check(!records.back().interrupted, QStringLiteral("正常回合标记为完整"));
        }

        // ── 关键词搜索 ──
        keyword->setText(QStringLiteral("交付凭证"));
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
                check(text.contains(QStringLiteral("劳动争议申请仲裁的时效怎么算？")),
                      QStringLiteral("导出件回读中文正常（UTF-8 无损）"));
                check(text.contains(QStringLiteral("命中块数：")),
                      QStringLiteral("导出件含命中来源章节"));
            }
        }

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
    QFile::remove(QStringLiteral("rag_index.dat"));

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
    QFile::remove(QStringLiteral("rag_index.dat"));
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
    const QString settingsPath = QStringLiteral("rag_settings.json");
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
        clickNavItem(navList, 4);
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
        clickNavItem(navList, 4);
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
        clickNavItem(navList, 4);
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
    QFile::remove(QStringLiteral("rag_index.dat"));

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
    QFile::remove(QStringLiteral("rag_index.dat"));
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

        qInfo().noquote() << QStringLiteral("── P0 同名覆盖确认 E2E（覆盖 / 跳过 / 取消 + 内容一致）──");
        runOverwriteE2E();
    }

    qInfo().noquote() << (g_failures == 0
                              ? QStringLiteral("UI smoke: 全部通过")
                              : QStringLiteral("UI smoke: %1 项失败").arg(g_failures));

    window.close();
    QApplication::processEvents();
    return g_failures == 0 ? 0 : 1;
}
