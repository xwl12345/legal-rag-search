/**
 * 检索质量量化评测（T4：四路对比版）
 *
 * 基于 21 篇模拟裁判文书语料的 23 条标注查询（qrels），
 * 对四条检索通路分别计算 P@5 / Hit@5 / R@10 / MRR（文档级排名，宏平均）：
 *   ① BM25 单路
 *   ② 向量单路（Embedding 未配置时无结果，指标为 0，仅供参考）
 *   ③ 加权融合（默认路；向量未配置时降级 = ①的排序）
 *   ④ RRF 融合（倒数排名融合，k=60；向量未配置时降级 = ①的排序）
 *
 * 标注集与指标实现是单一事实源（rag/golden_queries.h + rag/eval_metrics.h），
 * 与质量分析页共用；本工具负责命令行批量出数，供论文实验数据引用。
 *
 * 运行方式：从项目根目录执行 build/eval_retrieval.exe
 * （配置 Embedding 后运行会真实调用 API：全库向量懒重建约 6 批 + 每查询 1 次）
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>

#include <chrono>

#include "rag/retriever.h"
#include "rag/golden_queries.h"
#include "rag/eval_metrics.h"
#include "config/app_settings.h"
#include "config/app_config.h"

#include <cstdlib>

// ── 列出语料（与 test_main.cpp 相同的 Qt 实现）──
static std::vector<std::string> listTxtFiles(const std::string& dir) {
    std::vector<std::string> files;
    QDirIterator it(QString::fromStdString(dir), {QStringLiteral("*.txt")}, QDir::Files);
    while (it.hasNext()) {
        files.push_back(it.next().toStdString());
    }
    std::sort(files.begin(), files.end());
    return files;
}

int main(int argc, char* argv[]) {
    // QCoreApplication 必须先建：embedBatch 用 QEventLoop 等待网络应答，
    // 没有应用实例时事件派发不工作（实测：进程卡死空转 30+ 分钟）。
    QCoreApplication app(argc, argv);

    if (!QFile::exists(QStringLiteral("test/data/rag_intro.txt"))) {
        std::cerr << "请从项目根目录运行: ./build/eval_retrieval.exe" << std::endl;
        return 1;
    }

    // ── 导入语料 ──
    rag::Retriever retriever;
    // 裸 Retriever 不读配置文件（那是 MainWindow 的职责）——评测工具自己加载：
    // Embedding 三项来自 rag_settings.json（工作目录），Key 缺省回落 env。
    config::AppSettings settings;
    if (config::AppSettings::load(config::SETTINGS_FILE, settings)) {
        retriever.setEmbeddingEndpoint(settings.embeddingBaseUrl, settings.embeddingModel);
        if (!settings.embeddingApiKey.empty()) {
            retriever.setApiKey(settings.embeddingApiKey);
        }
        std::cout << "配置加载: " << config::SETTINGS_FILE
                  << "（模型 " << settings.embeddingModel << "）\n";
    } else {
        std::cout << "配置加载: 未找到 " << config::SETTINGS_FILE
                  << "，回落 DEEPSEEK_API_KEY 环境变量\n";
        const char* env = std::getenv("DEEPSEEK_API_KEY");
        if (env && *env) retriever.setApiKey(env);
    }
    const auto files = listTxtFiles("test/data/legal_cases");
    int chunks = 0;
    for (const auto& f : files) {
        auto r = retriever.addDocument(f);
        chunks += r.chunksAdded;
    }
    std::cout << "语料导入: " << files.size() << " 篇文书, " << chunks << " 个文本块\n";
    std::cout << "Embedding: "
              << (retriever.embeddingReady()
                      ? "已配置（" + retriever.embeddingModel() + "@"
                            + retriever.embeddingHost() + "）"
                      : "未配置（② 无结果，③④ 降级纯 BM25）")
              << "\n\n";

    struct ModeRow {
        const char* name;
        rag::SearchMode mode;
        std::vector<rag::QueryMetrics> rows;
    };
    std::vector<ModeRow> modes = {
        {"① BM25 单路",     rag::SearchMode::Bm25Only,       {}},
        {"② 向量单路",      rag::SearchMode::VectorOnly,     {}},
        {"③ 加权融合",      rag::SearchMode::WeightedFusion, {}},
        {"④ RRF 融合",      rag::SearchMode::RrfFusion,      {}},
    };

    const auto& golden = rag::goldenQueries();
    constexpr int kWidth = 50;

    // ── 向量路预检（快速失败）：Key 已配置但 API 实际不通时，每条查询会
    // 白等 HTTP 30s 超时——先探一次，不通就只评 BM25 路并明确说明。──
    bool vectorUsable = retriever.embeddingReady();
    if (vectorUsable) {
        const auto t0 = std::chrono::steady_clock::now();
        auto probe = retriever.searchWithMode("预检探针 借贷纠纷", 5, rag::SearchMode::VectorOnly);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        vectorUsable = !probe.empty();
        std::cout << "向量路预检: " << (vectorUsable ? "通过" : "失败（本次评测只评 BM25 路）")
                  << "，探针耗时 " << ms << " ms\n\n";
    }

    // ── 逐查询 × 逐路评测 ──
    std::cout << std::fixed << std::setprecision(3);
    const auto evalStart = std::chrono::steady_clock::now();
    for (size_t gi = 0; gi < golden.size(); ++gi) {
        std::cout << "[" << (gi + 1) << "/" << golden.size() << "] "
                  << golden[gi].query << std::flush;
        const auto q0 = std::chrono::steady_clock::now();

        for (auto& m : modes) {
            // 向量路不可用时：② 无结果（全 0）；③④ 在引擎内确定性降级为
            // BM25 排序——直接复用 ① 本条查询的指标值（省去每条查询 3 次
            // 注定失败的超时等待）。① 是 modes[0]，先于 ③④ 处理。
            if (!vectorUsable && m.mode != rag::SearchMode::Bm25Only) {
                if (m.mode == rag::SearchMode::VectorOnly) {
                    m.rows.emplace_back();   // 全 0
                } else {
                    m.rows.push_back(modes[0].rows.back());
                }
                continue;
            }
            auto results = retriever.searchWithMode(golden[gi].query, kWidth, m.mode);
            std::vector<std::string> chunkDocs;
            chunkDocs.reserve(results.size());
            for (const auto& r : results) {
                chunkDocs.push_back(r.docId);
            }
            m.rows.push_back(rag::computeQueryMetrics(
                rag::docLevelRanking(chunkDocs), golden[gi].relevant));
        }

        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - q0).count();
        std::cout << "  (" << ms << " ms)\n" << std::flush;
    }

    // ── 汇总对比表 ──
    const int n = static_cast<int>(golden.size());
    std::cout << "\n════════ 四路指标对比（" << n << " 条标注查询，文档级排名，宏平均）════════\n";
    std::cout << "通路          P@5    Hit@5  R@10   MRR\n";
    std::cout << "─────────────────────────────────────────\n";
    for (const auto& m : modes) {
        const auto avg = rag::averageMetrics(m.rows);
        std::cout << std::left << std::setw(14) << m.name
                  << std::right << std::setw(5) << avg.p5
                  << std::setw(7) << avg.hit5
                  << std::setw(7) << avg.r10
                  << std::setw(7) << avg.mrr << "\n";
    }
    std::cout << "═════════════════════════════════════════\n";

    // ── 默认路（加权融合）的劣置/未命中明细（用于改进分析）──
    const auto& w = modes[2].rows;
    std::cout << "\n默认路（加权融合）未在 Top-5 首位命中或未命中的查询:\n";
    int miss = 0;
    for (int i = 0; i < n; ++i) {
        if (w[i].firstRank == 0 || w[i].firstRank > 5) {
            std::cout << "  - " << golden[i].query
                      << " (首命中: "
                      << (w[i].firstRank > 0 ? std::to_string(w[i].firstRank)
                                             : std::string("未命中"))
                      << ", R@10=" << w[i].r10 << ")\n";
            ++miss;
        }
    }
    if (miss == 0) std::cout << "  （无）\n";

    return 0;
}
