/**
 * 检索质量量化评测
 *
 * 基于 21 篇模拟裁判文书语料构建 23 条标注查询（qrels），
 * 对混合检索器（BM25 路径）计算 P@5 / Recall@10 / MRR。
 *
 * 查询类型：
 *   A. 案情描述型 —— 用自然语言描述案情，考察语义匹配
 *   B. 法律术语型 —— 案由/罪名等术语查询
 *   C. 易混区分型 —— 相似案由的判别力（如 职务侵占 vs 损害公司利益）
 *   D. 多相关文档型 —— 一个查询对应多份相关文书
 *
 * 运行方式：从项目根目录执行 build/eval_retrieval.exe
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include <QDirIterator>
#include <QFile>

#include "rag/retriever.h"

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

struct GoldenQuery {
    std::string query;
    std::vector<std::string> relevant;   // 相关文档 docId（文件名，含扩展名）
    const char* type;                    // 查询类型标注
};

// ── 标注查询集（人工标注，依据各文书案由与案情）──
static const std::vector<GoldenQuery> GOLDEN = {
    // ── 民事 ──
    {"民间借贷纠纷",                     {"case_civil_001_loan_dispute.txt"},      "B 法律术语"},
    {"借钱不还被起诉会有什么后果",       {"case_civil_001_loan_dispute.txt"},      "A 案情描述"},
    {"公司拖欠工资违法解除劳动合同怎么维权", {"case_civil_006_labor_dispute.txt"}, "A 案情描述"},
    {"劳动争议仲裁",                     {"case_civil_006_labor_dispute.txt"},      "B 法律术语"},
    {"夫妻感情破裂离婚孩子抚养权归属",   {"case_civil_004_divorce_case.txt"},      "A 案情描述"},
    {"法定继承遗产分配顺序",             {"case_civil_005_inheritance_dispute.txt"},"A 案情描述"},
    {"开发商逾期交房违约金 商品房预售合同", {"case_civil_007_property_dispute.txt"},"A 案情描述"},
    {"交通事故责任认定保险理赔",         {"case_civil_008_traffic_accident.txt"},  "A 案情描述"},
    {"业主在小区受伤物业管理公司责任",   {"case_civil_003_tort_dispute.txt"},      "C 易混区分"},
    {"技术服务合同纠纷违约责任",         {"case_civil_002_contract_dispute.txt"},  "B 法律术语"},
    // ── 刑事 ──
    {"醉驾血液酒精含量 危险驾驶罪",      {"case_criminal_005_drunk_driving.txt"},  "A 案情描述"},
    {"盗窃罪立案量刑标准",               {"case_criminal_002_theft.txt"},          "B 法律术语"},
    {"电信诈骗数额较大怎么判刑",         {"case_criminal_001_fraud.txt"},          "A 案情描述"},
    {"故意伤害罪附带民事诉讼赔偿",       {"case_criminal_003_assault.txt"},        "B 法律术语"},
    {"财务人员侵占公司资金",             {"case_criminal_004_embezzlement.txt"},   "C 易混区分"},
    // ── 知识产权 ──
    {"发明专利侵权损害赔偿",             {"case_ip_002_patent.txt"},               "B 法律术语"},
    {"电视剧信息网络传播权侵权",         {"case_ip_003_copyright.txt"},            "A 案情描述"},
    {"商标权纠纷",                       {"case_ip_001_trademark.txt",
                                          "case_admin_001_license_dispute.txt"},   "D 多相关"},
    // ── 商事/行政 ──
    {"股东起诉法定代表人损害公司利益",   {"case_commercial_001_company_dispute.txt"}, "A 案情描述"},
    {"资不抵债申请破产清算",             {"case_commercial_002_bankruptcy.txt"},   "A 案情描述"},
    {"对市场监督管理局行政处罚不服提起诉讼", {"case_admin_002_penalty_dispute.txt"},"A 案情描述"},
    {"行政赔偿请求",                     {"case_admin_003_compensation.txt"},      "B 法律术语"},
    {"商标无效宣告行政诉讼",             {"case_admin_001_license_dispute.txt"},   "C 易混区分"},
};

static std::string baseName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

int main() {
    if (!QFile::exists(QStringLiteral("test/data/rag_intro.txt"))) {
        std::cerr << "请从项目根目录运行: ./build/eval_retrieval.exe" << std::endl;
        return 1;
    }

    // ── 导入语料 ──
    rag::Retriever retriever;
    const auto files = listTxtFiles("test/data/legal_cases");
    int chunks = 0;
    for (const auto& f : files) {
        auto r = retriever.addDocument(f);
        chunks += r.chunksAdded;
    }
    std::cout << "语料导入: " << files.size() << " 篇文书, " << chunks << " 个文本块\n\n";

    // ── 逐查询评测（文档级排序：同文档多块取其最高名次）──
    double sumP5 = 0, sumHit5 = 0, sumR10 = 0, sumMRR = 0;
    struct Row { std::string query, type; int hits5; int relN; double p5, r10, mrr; int top1Rank; };
    std::vector<Row> rows;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "──────────────────────────────────────────────────────────────────────────\n";
    std::cout << "查询                                类型        P@5  Hit@5   R@10   MRR  首命中\n";
    std::cout << "──────────────────────────────────────────────────────────────────────────\n";

    for (const auto& g : GOLDEN) {
        auto results = retriever.search(g.query, 50);

        // 文档级排序（去重保序）
        std::vector<std::string> docRank;
        for (const auto& r : results) {
            if (std::find(docRank.begin(), docRank.end(), r.docId) == docRank.end()) {
                docRank.push_back(r.docId);
            }
        }

        const int relN = static_cast<int>(g.relevant.size());
        int hits5 = 0, hits10 = 0, firstRank = 0;
        for (int i = 0; i < static_cast<int>(docRank.size()); ++i) {
            bool rel = std::find(g.relevant.begin(), g.relevant.end(), docRank[i]) != g.relevant.end();
            if (!rel) continue;
            if (i < 5)  ++hits5;
            if (i < 10) ++hits10;
            if (firstRank == 0) firstRank = i + 1;
        }

        double p5   = static_cast<double>(hits5) / 5.0;
        double hit5 = hits5 > 0 ? 1.0 : 0.0;   // Success@5：Top-5 内至少命中一个相关文档
        double r10  = relN > 0 ? static_cast<double>(hits10) / relN : 0.0;
        double mrr  = firstRank > 0 ? 1.0 / firstRank : 0.0;
        sumP5 += p5; sumHit5 += hit5; sumR10 += r10; sumMRR += mrr;

        rows.push_back({g.query, g.type, hits5, relN, p5, r10, mrr, firstRank});

        std::cout << std::left << std::setw(36) << g.query
                  << std::setw(12) << g.type
                  << std::right << std::setw(5) << p5
                  << std::setw(7) << hit5
                  << std::setw(7) << r10
                  << std::setw(6) << mrr
                  << std::setw(7) << (firstRank > 0 ? std::to_string(firstRank) : "-")
                  << "\n";
    }

    const int n = static_cast<int>(GOLDEN.size());
    std::cout << "──────────────────────────────────────────────────────────────────────────\n";
    std::cout << "合计 " << n << " 条查询"
              << "  P@5=" << sumP5 / n
              << "  Hit@5=" << sumHit5 / n
              << "  R@10=" << sumR10 / n
              << "  MRR=" << sumMRR / n << "\n";
    std::cout << "──────────────────────────────────────────────────────────────────────────\n";

    // ── 未命中/劣置查询明细（用于改进分析）──
    std::cout << "\n未在 Top-5 首位命中或未命中的查询:\n";
    int miss = 0;
    for (const auto& r : rows) {
        if (r.top1Rank == 0 || r.top1Rank > 5) {
            std::cout << "  - " << r.query << " (首命中: "
                      << (r.top1Rank > 0 ? std::to_string(r.top1Rank) : std::string("未命中"))
                      << ", R@10=" << r.r10 << ")\n";
            ++miss;
        }
    }
    if (miss == 0) std::cout << "  （无）\n";

    return 0;
}
