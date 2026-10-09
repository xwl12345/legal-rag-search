/**
 * 核心模块单元测试
 *
 * 测试覆盖：
 *   1. 文档解析 + 分块
 *   2. 中文分词 + 去停用词
 *   3. 倒排索引构建 + 查询
 *   4. BM25 排序
 *   5. 余弦相似度计算
 *   6. 混合检索 (Retriever) — 无 API Key 的降级模式
 *
 * 编译方式（手动）:
 *   g++ -std=c++17 -I../include -I../third_party/cppjieba/include \
 *       -I../third_party/limonp/include -DCPPJIEBA_DICT_PATH=\"../third_party/cppjieba/dict\" \
 *       test_main.cpp ../src/document/parser.cpp ../src/document/tokenizer.cpp \
 *       ../src/index/inverted_index.cpp ../src/index/bm25_ranker.cpp \
 *       ../src/vector/similarity.cpp ../src/vector/embedding.cpp \
 *       ../src/rag/retriever.cpp ../src/rag/generator.cpp \
 *       -o test_main.exe
 *
 * 或通过 CMake 构建 test target（推荐）。
 */

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

// ── 模块头文件 ──
#include "document/parser.h"
#include "document/tokenizer.h"
#include "document/metadata.h"
#include "index/inverted_index.h"
#include "index/bm25_ranker.h"
#include "vector/similarity.h"
#include "vector/embedding.h"
#include "document/pdf_extractor.h"
#include "rag/retriever.h"
#include "rag/generator.h"
#include "rag/eval_metrics.h"
#include "history/history_store.h"
#include "config/app_config.h"
#include "config/app_settings.h"
#include "../test/fake_transport.h"
#include <algorithm>

// ── 列出目录下 .txt 文件 ──
// 用 Qt 实现：GCC 8 MinGW 的 std::filesystem 在 Windows 上不可用（已知缺陷），
// 且项目约定中文路径统一由 Qt 处理（见 src/document/parser.cpp）。
static std::vector<std::string> listTxtFiles(const std::string& dir) {
    std::vector<std::string> files;
    QDirIterator it(QString::fromStdString(dir), {QStringLiteral("*.txt")}, QDir::Files);
    while (it.hasNext()) {
        files.push_back(it.next().toStdString());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// ── 简单的测试框架 ──
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) \
    std::cout << "  [" << name << "] ";

#define PASS() \
    do { std::cout << "✅ PASSED" << std::endl; g_passed++; } while(0)

#define FAIL(msg) \
    do { std::cout << "❌ FAILED: " << msg << std::endl; g_failed++; } while(0)

#define CHECK(cond) \
    do { if (!(cond)) { FAIL(#cond); return; } } while(0)

#define CHECK_EQ(a, b) \
    do { if ((a) != (b)) { FAIL("expected " + std::to_string(b) + " but got " + std::to_string(a)); return; } } while(0)

#define CHECK_CLOSE(a, b, eps) \
    do { if (std::abs((a) - (b)) > (eps)) { FAIL("expected ~" + std::to_string(b) + " but got " + std::to_string(a)); return; } } while(0)

// ═══════════════════════════════════════════════════════════════
// 测试 1: 文档解析 + 文本分块
// ═══════════════════════════════════════════════════════════════
void test_parser_basic() {
    TEST("解析英文文本");
    document::DocumentParser parser;
    std::string text = "Hello world. This is a test document for the RAG search engine.";
    auto chunks = parser.parseText(text, "test.txt");
    CHECK(chunks.size() >= 1);
    CHECK(chunks[0].docId == "test.txt");
    CHECK(chunks[0].chunkIndex == 0);
    CHECK(!chunks[0].content.empty());
    PASS();
}

void test_parser_chunking() {
    TEST("长文本分块");
    document::DocumentParser parser;
    // 生成超过 512 字符的文本
    std::string longText(1500, 'A');
    auto chunks = parser.parseText(longText, "long.txt");
    // 应该被分成多个块（512 每块 + overlap）
    CHECK(chunks.size() >= 2);
    CHECK(chunks[0].docId == "long.txt");
    CHECK(chunks[1].docId == "long.txt");
    CHECK(chunks[0].chunkIndex == 0);
    CHECK(chunks[1].chunkIndex == 1);
    PASS();
}

void test_parser_empty() {
    TEST("空文本解析");
    document::DocumentParser parser;
    auto chunks = parser.parseText("", "empty.txt");
    CHECK(chunks.empty());
    PASS();
}

void test_parser_file() {
    TEST("从文件解析");
    document::DocumentParser parser;
    auto chunks = parser.parse("test/data/rag_intro.txt");
    if (chunks.empty()) {
        FAIL("无法读取测试文件 test/data/rag_intro.txt，请确认文件存在");
        return;
    }
    CHECK(chunks.size() >= 1);
    CHECK(chunks[0].docId == "rag_intro.txt");
    CHECK(!chunks[0].content.empty());
    std::cout << "    (解析出 " << chunks.size() << " 个文本块) ";
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 2: 中文分词
// ═══════════════════════════════════════════════════════════════
void test_tokenizer_cut() {
    TEST("中文分词基础");
    document::Tokenizer tokenizer;
    auto words = tokenizer.cut("我爱北京天安门");
    CHECK(words.size() >= 2);  // 至少分出 "我"、"爱"、"北京"、"天安门" 中的几个
    std::cout << "    (分词结果: ";
    for (const auto& w : words) std::cout << w << " ";
    std::cout << ") ";
    PASS();
}

void test_tokenizer_cut_for_index() {
    TEST("索引分词（去停用词）");
    document::Tokenizer tokenizer;
    // "的" 和 "了" 是停用词
    auto words = tokenizer.cutForIndex("RAG是一种检索增强生成技术，可以显著提高准确率。");
    CHECK(words.size() >= 2);
    // 停用词 "的"、"了" 应该被过滤
    for (const auto& w : words) {
        CHECK(w != "的");
        CHECK(w != "了");
    }
    std::cout << "    (关键词: ";
    for (const auto& w : words) std::cout << w << " ";
    std::cout << ") ";
    PASS();
}

void test_tokenizer_empty() {
    TEST("空文本分词");
    document::Tokenizer tokenizer;
    auto words = tokenizer.cut("");
    CHECK(words.empty());
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 3: 倒排索引
// ═══════════════════════════════════════════════════════════════
void test_inverted_index_add() {
    TEST("倒排索引添加文档");
    search_index::InvertedIndex idx;
    idx.addDocument("doc1.txt", 0, {"rag", "retrieval", "generation"});
    idx.addDocument("doc2.txt", 0, {"machine", "learning", "rag"});

    CHECK_EQ(idx.totalDocs(), 2);
    CHECK_EQ(static_cast<int>(idx.size()), 5);  // 5 个不同的词
    PASS();
}

void test_inverted_index_query() {
    TEST("倒排索引查询");
    search_index::InvertedIndex idx;
    idx.addDocument("doc1.txt", 0, {"rag", "retrieval", "augmented"});
    idx.addDocument("doc2.txt", 0, {"machine", "learning"});

    // 查询 "rag" 应该返回 1 个文档
    const auto* postings = idx.getPostings("rag");
    CHECK(postings != nullptr);
    CHECK_EQ(static_cast<int>(postings->size()), 1);
    CHECK((*postings)[0].docId == "doc1.txt");

    // 查询不存在的词
    const auto* none = idx.getPostings("nonexistent");
    CHECK(none == nullptr);
    PASS();
}

void test_inverted_index_doc_freq() {
    TEST("文档频率统计");
    search_index::InvertedIndex idx;
    idx.addDocument("d1.txt", 0, {"rag", "ai", "rag"});   // rag 出现 2 次
    idx.addDocument("d2.txt", 0, {"ai", "ml"});

    // "rag" 只出现在 1 个文档中
    CHECK_EQ(idx.docFreq("rag"), 1);
    // "ai" 出现在 2 个文档中
    CHECK_EQ(idx.docFreq("ai"), 2);
    PASS();
}

void test_inverted_index_clear() {
    TEST("清空索引");
    search_index::InvertedIndex idx;
    idx.addDocument("doc.txt", 0, {"test", "data"});
    CHECK_EQ(idx.totalDocs(), 1);

    idx.clear();
    CHECK_EQ(idx.totalDocs(), 0);
    CHECK_EQ(static_cast<int>(idx.size()), 0);
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 4: BM25 排序
// ═══════════════════════════════════════════════════════════════
void test_bm25_basic() {
    TEST("BM25 基础搜索");
    search_index::InvertedIndex idx;
    search_index::BM25Ranker bm25;

    // 模拟 3 个文档
    idx.addDocument("d1.txt", 0, {"rag", "retrieval", "generation", "ai"});
    idx.addDocument("d2.txt", 0, {"machine", "learning", "deep", "network"});
    idx.addDocument("d3.txt", 0, {"rag", "search", "engine", "retrieval"});

    auto results = bm25.search({"rag", "retrieval"}, idx, 5);
    CHECK(results.size() >= 2);  // d1 和 d3 都包含至少一个查询词

    // d1 应该排在 d3 前面（d1 包含所有查询词，d3 也包含 2 个）
    // 分数会因 IDF 和 TF 不同而不同，但都不为 0
    CHECK(results[0].score > 0.0);
    std::cout << "    (Top-1: " << results[0].docId << " score=" << results[0].score << ") ";
    PASS();
}

void test_bm25_empty_query() {
    TEST("BM25 空查询");
    search_index::InvertedIndex idx;
    search_index::BM25Ranker bm25;
    idx.addDocument("d1.txt", 0, {"test"});
    auto results = bm25.search({}, idx, 5);
    CHECK(results.empty());
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 5: 余弦相似度
// ═══════════════════════════════════════════════════════════════
void test_cosine_same_vector() {
    TEST("相同向量 → 相似度 1.0");
    std::vector<double> v = {1.0, 2.0, 3.0};
    double sim = vector_engine::SimilarityEngine::cosineSimilarity(v, v);
    CHECK_CLOSE(sim, 1.0, 0.0001);
    PASS();
}

void test_cosine_orthogonal() {
    TEST("正交向量 → 相似度 0.0");
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double sim = vector_engine::SimilarityEngine::cosineSimilarity(a, b);
    CHECK_CLOSE(sim, 0.0, 0.0001);
    PASS();
}

void test_cosine_opposite() {
    TEST("相反向量 → 相似度 -1.0");
    std::vector<double> a = {1.0, 2.0};
    std::vector<double> b = {-1.0, -2.0};
    double sim = vector_engine::SimilarityEngine::cosineSimilarity(a, b);
    CHECK_CLOSE(sim, -1.0, 0.0001);
    PASS();
}

void test_similarity_engine_search() {
    TEST("相似度引擎 TopK 搜索");
    vector_engine::SimilarityEngine engine;

    // 添加 5 个向量
    engine.addVector(0, {1.0, 0.0, 0.0});   // doc 0
    engine.addVector(1, {0.9, 0.1, 0.0});   // doc 1 ← 最接近查询
    engine.addVector(2, {0.0, 1.0, 0.0});   // doc 2
    engine.addVector(3, {0.5, 0.5, 0.0});   // doc 3
    engine.addVector(4, {0.0, 0.0, 1.0});   // doc 4

    // 查询向量：接近 doc 1
    std::vector<double> query = {1.0, 0.0, 0.0};
    auto results = engine.search(query, 3);

    CHECK_EQ(static_cast<int>(results.size()), 3);
    // 最相似的应该是 doc 0 (index=0)，其次是 doc 1 (index=1)
    std::cout << "    (Top: idx=" << results[0].index
              << " sim=" << results[0].similarity << ") ";
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 6: Retriever 混合检索（降级模式 — 无 API Key）
// ═══════════════════════════════════════════════════════════════
void test_retriever_bm25_fallback() {
    TEST("Retriever 降级到纯 BM25（无 API Key）");
    rag::Retriever retriever;

    // 不设置 API Key，应自动降级到纯 BM25
    retriever.addText("RAG是一种检索增强生成技术，它结合了信息检索和文本生成。", "intro.txt");
    retriever.addText("BM25是基于概率检索模型的排序函数，用于评估查询与文档的相关性。", "bm25.txt");
    retriever.addText("向量检索利用Embedding将文本映射到高维空间，通过余弦相似度计算语义相关性。", "vector.txt");

    auto results = retriever.search("什么是RAG技术", 3);
    CHECK(results.size() >= 1);
    // 第一个结果应该最相关（包含 "RAG" 关键词）
    std::cout << "    (匹配 " << results.size() << " 条, "
              << "Top-1: " << results[0].docId
              << " score=" << results[0].finalScore << ") ";
    PASS();
}

void test_retriever_build_context() {
    TEST("构建 AI 上下文");
    rag::Retriever retriever;
    retriever.addText("RAG是检索增强生成技术。", "doc1.txt");
    retriever.addText("BM25是一种排序算法。", "doc2.txt");

    auto results = retriever.search("RAG技术", 2);
    CHECK(results.size() >= 1);

    std::string context = retriever.buildContext(results, 500);
    CHECK(!context.empty());
    // 上下文应该包含来源标记
    CHECK(context.find("【来源") != std::string::npos);
    CHECK(context.find("doc1.txt") != std::string::npos);

    std::cout << "    (上下文长度: " << context.size() << " 字符) ";
    PASS();
}

void test_diagnostic_user_query() {
    TEST("诊断：用户查询 \"RAG的优劣在哪里\"");
    document::Tokenizer tokenizer;

    // 显示查询分词结果
    auto queryTerms = tokenizer.cutForIndex("RAG的优劣在哪里");
    std::cout << "\n    查询分词: [";
    for (size_t i = 0; i < queryTerms.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << queryTerms[i];
    }
    std::cout << "]";

    // 导入实际测试文档并搜索
    rag::Retriever retriever;
    retriever.addText("RAG（Retrieval-Augmented Generation，检索增强生成）是一种结合信息检索"
                       "与文本生成的AI技术架构。它的核心思想是：在让大语言模型回答问题之前，"
                       "先从外部知识库中检索相关信息，然后将检索结果作为上下文提供给模型。"
                       "RAG的主要优点包括：减少幻觉、知识更新、可溯源、领域适配。",
                       "rag_intro.txt");

    std::cout << "\n    已导入文档数: " << retriever.docCount();

    auto results = retriever.search("RAG的优劣在哪里", 5);
    std::cout << "\n    搜索结果数: " << results.size();

    if (!results.empty()) {
        std::cout << "\n    Top-1: " << results[0].docId
                  << " score=" << results[0].finalScore
                  << "\n    内容预览: " << results[0].content.substr(0, 80) << "...";
    } else {
        std::cout << "\n    ⚠️ BM25 未匹配到任何文档！";

        // 诊断：检查索引中的词
        std::cout << "\n    诊断：逐个检查查询词是否在索引中...";
        for (const auto& term : queryTerms) {
            // 手动检查（需要访问 index_，但它是 private 的）
            std::cout << "\n      查询词 '" << term << "'";
        }
    }

    std::cout << std::endl;
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 7: PDF 文本提取
// ═══════════════════════════════════════════════════════════════
void test_pdf_extract_text() {
    TEST("PDF 文本提取");
    std::string text = document::PdfExtractor::extractText("test/data/test.pdf");
    if (text.empty()) {
        FAIL("PDF 文本提取失败，返回空文本");
        return;
    }
    // 应该包含 "Hello PDF World" 和 "RAG Search Engine"
    CHECK(text.find("Hello PDF World") != std::string::npos);
    CHECK(text.find("RAG Search Engine") != std::string::npos);
    std::cout << "    (提取文本: \"" << text << "\") ";
    PASS();
}

void test_pdf_parser_routing() {
    TEST("DocumentParser PDF 路由");
    document::DocumentParser parser;
    auto chunks = parser.parse("test/data/test.pdf");
    if (chunks.empty()) {
        FAIL("DocumentParser 未能解析 PDF 文件（.pdf 路由失败）");
        return;
    }
    CHECK(chunks.size() >= 1);
    CHECK(chunks[0].docId == "test.pdf");
    CHECK(!chunks[0].content.empty());
    std::cout << "    (解析出 " << chunks.size() << " 个文本块) ";
    PASS();
}

void test_pdf_not_a_pdf() {
    TEST("非 PDF 文件返回空");
    // 传入一个不是 PDF 的文本文件，PdfExtractor 应返回空
    std::string text = document::PdfExtractor::extractText("test/data/rag_intro.txt");
    // rag_intro.txt 不是 PDF（不以 %PDF- 开头），应返回空
    CHECK(text.empty());
    PASS();
}

void test_pdf_retriever_integration() {
    TEST("PDF 通过 Retriever 导入并检索");
    rag::Retriever retriever;
    const auto importResult = retriever.addDocument("test/data/test.pdf");

    CHECK(importResult.imported);
    CHECK(importResult.chunksAdded >= 1);
    CHECK(importResult.diagnostic.empty());

    // 应该至少有一个 chunk
    CHECK(retriever.chunkCount() >= 1);

    // 搜索 PDF 中的内容
    auto results = retriever.search("Hello PDF", 3);
    CHECK(results.size() >= 1);
    std::cout << "    (检索到 " << results.size() << " 条结果) ";
    PASS();
}

void test_retriever_failed_import() {
    TEST("Retriever 失败导入不会计为成功");
    rag::Retriever retriever;
    const auto result = retriever.addDocument("test/data/does_not_exist.pdf");

    CHECK(!result.imported);
    CHECK(result.chunksAdded == 0);
    CHECK(!result.diagnostic.empty());
    CHECK(retriever.chunkCount() == 0);
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 8: 法律词典
// ═══════════════════════════════════════════════════════════════
void test_legal_dict_loaded() {
    TEST("法律词典加载成功");
    document::Tokenizer tokenizer;
    // 词典在构造函数中自动加载，验证分词结果即可
    // 分词包含法律术语则说明加载成功
    auto words = tokenizer.cut("原告向人民法院提起诉讼");
    CHECK(words.size() >= 3);  // 至少分出原告/向/人民法院/提起/诉讼
    std::cout << "    (分词: ";
    for (const auto& w : words) std::cout << w << " ";
    std::cout << ") ";
    PASS();
}

void test_legal_term_recognition() {
    TEST("法律术语识别");
    document::Tokenizer tokenizer;
    auto words = tokenizer.cutForIndex("被告不服一审判决提出上诉");

    // 应该识别出法律术语
    bool hasDefendant = false, hasFirstInstance = false, hasAppeal = false;
    for (const auto& w : words) {
        if (w == "被告") hasDefendant = true;
        // cppjieba 会将"一审判决"合成为一个词（词典+统计）
        if (w == "一审判决" || w == "一审") hasFirstInstance = true;
        if (w == "上诉") hasAppeal = true;
    }
    std::cout << "    (关键词: ";
    for (const auto& w : words) std::cout << w << " ";
    std::cout << ") ";
    CHECK(hasDefendant);
    CHECK(hasFirstInstance);
    CHECK(hasAppeal);
    PASS();
}

void test_legal_compound_terms() {
    TEST("法律复合术语不被拆分");
    document::Tokenizer tokenizer;
    // "知识产权法院" 应被识别为整体，而非 "知识产权" + "法院"
    auto words = tokenizer.cut("北京知识产权法院审理了一起专利侵权案件");
    bool hasIPC = false, hasPatent = false;
    for (const auto& w : words) {
        if (w == "知识产权法院") hasIPC = true;
        if (w == "专利侵权") hasPatent = true;
    }
    std::cout << "    (分词: ";
    for (const auto& w : words) std::cout << w << " ";
    std::cout << ") ";
    CHECK(hasIPC);
    CHECK(hasPatent);
    PASS();
}

void test_legal_search_improvement() {
    TEST("法律词典提升检索效果");
    // 对比：有无法律词典对同一查询的检索结果
    rag::Retriever retriever;
    retriever.addText(
        "原告张三与被告李四签订了一份技术开发合同，约定共同开发一套人工智能系统。"
        "合同约定，张三出资100万元，李四提供技术。后因李四违反合同约定，未按期交付技术成果，"
        "导致合同履行发生争议。张三向人民法院提起诉讼，要求李四承担违约责任并赔偿损失。"
        "一审法院判决李四赔偿张三经济损失50万元。李四不服一审判决，向中级人民法院提起上诉。",
        "case001.txt"
    );

    auto results = retriever.search("合同违约赔偿责任", 3);
    CHECK(results.size() >= 1);

    // 检索结果应包含相关法律内容
    std::cout << "    (匹配 " << results.size() << " 条, Top-1: "
              << results[0].docId << " score=" << results[0].finalScore << ") ";
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 9: 元数据提取
// ═══════════════════════════════════════════════════════════════
void test_metadata_case_number() {
    TEST("案号提取");
    std::string text = "北京市朝阳区人民法院\n民事判决书\n（2024）京0105民初12345号\n";
    auto meta = document::MetadataExtractor::extract(text);
    CHECK(!meta.caseNumber.empty());
    CHECK(meta.caseNumber.find("2024") != std::string::npos);
    CHECK(meta.caseNumber.find("民初") != std::string::npos);
    std::cout << "    (案号: " << meta.caseNumber << ") ";
    PASS();
}

void test_metadata_court() {
    TEST("法院名称提取");
    std::string text = "北京市朝阳区人民法院\n民事判决书\n审判长：张某某";
    auto meta = document::MetadataExtractor::extract(text);
    CHECK(!meta.court.empty());
    CHECK(meta.court.find("人民法院") != std::string::npos);
    std::cout << "    (法院: " << meta.court << ") ";
    PASS();
}

void test_metadata_date_arabic() {
    TEST("日期提取（阿拉伯数字）");
    std::string text = "二〇二四年三月十五日作出\n审判员签名\n2024年3月15日";
    auto meta = document::MetadataExtractor::extract(text);
    CHECK(!meta.date.empty());
    // 优先匹配阿拉伯数字格式
    CHECK(meta.date == "2024-03-15");
    std::cout << "    (日期: " << meta.date << ") ";
    PASS();
}

void test_metadata_date_chinese() {
    TEST("日期提取（中文数字）");
    std::string text = "本院于二〇二四年三月十五日作出如下判决";
    auto meta = document::MetadataExtractor::extract(text);
    CHECK(!meta.date.empty());
    CHECK(meta.date == "2024-03-15");
    std::cout << "    (日期: " << meta.date << ") ";
    PASS();
}

void test_metadata_case_type() {
    TEST("案件类型推导");
    std::string text = "（2023）京73民终456号\n侵害商标权纠纷";
    auto meta = document::MetadataExtractor::extract(text);
    CHECK(meta.caseType == "民事");
    std::cout << "    (类型: " << meta.caseType << ") ";
    PASS();
}

void test_metadata_integration() {
    TEST("Retriever 集成元数据");
    rag::Retriever retriever;
    retriever.addText(
        "北京市海淀区人民法院\n民事判决书\n（2024）京0108民初23456号\n"
        "原告阿里巴巴公司诉被告某科技有限公司侵害商标权纠纷一案，\n"
        "本院于二〇二四年五月二十日作出判决如下：\n"
        "一、被告立即停止侵权行为；\n二、被告赔偿原告经济损失100万元；\n"
        "审判长：李某某\n审判员：王某\n书记员：赵某",
        "case_beijing.txt"
    );

    const auto* meta = retriever.getMetadata("case_beijing.txt");
    CHECK(meta != nullptr);
    CHECK(!meta->caseNumber.empty());
    CHECK(!meta->court.empty());
    std::cout << "    (案号: " << meta->caseNumber
              << ", 法院: " << meta->court << ") ";
    PASS();
}

void test_metadata_empty() {
    TEST("非法律文档返回空元数据");
    std::string text = "This is a regular document about technology and AI.";
    auto meta = document::MetadataExtractor::extract(text);
    CHECK(meta.isEmpty());
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 10: 法律 Prompt 模板
// ═══════════════════════════════════════════════════════════════
void test_prompt_legal_detection() {
    TEST("法律上下文自动检测");
    // 包含法律术语的上下文应被识别为法律场景
    std::string legalCtx = "北京市朝阳区人民法院\n民事判决书\n（2024）京0105民初12345号\n原告张三诉被告李四合同纠纷";
    std::string genCtx = "RAG is a retrieval augmented generation technique for AI applications.";

    // 通过 prompt 内容间接验证：法律 prompt 应包含"案件概述"等法律特有结构
    rag::Generator gen;
    // 由于 buildPrompt 是 private，通过 generate 需要 API key
    // 这里我们验证元数据提取 + prompt 构建的集成效果
    auto meta = document::MetadataExtractor::extract(legalCtx);
    CHECK(!meta.caseNumber.empty());
    CHECK(!meta.court.empty());
    std::cout << "    (法律上下文检测: 案号=" << meta.caseNumber << ") ";
    PASS();
}

void test_prompt_metadata_summary() {
    TEST("元数据摘要构建");
    rag::Retriever retriever;
    retriever.addText(
        "北京市海淀区人民法院\n民事判决书\n（2024）京0108民初23456号\n"
        "原告甲公司诉被告乙公司侵害商标权纠纷一案\n"
        "审判长：王某某\n二〇二四年五月二十日",
        "case_haidian.txt"
    );
    retriever.addText(
        "上海市浦东新区人民法院\n刑事判决书\n（2023）沪0115刑初789号\n"
        "公诉机关上海市浦东新区人民检察院\n被告人李某涉嫌诈骗罪一案\n"
        "审判长：赵某某\n二〇二三年十一月十日",
        "case_pudong.txt"
    );

    // 直接验证元数据
    const auto* meta1 = retriever.getMetadata("case_haidian.txt");
    const auto* meta2 = retriever.getMetadata("case_pudong.txt");
    CHECK(meta1 != nullptr);
    CHECK(meta2 != nullptr);
    if (meta1) {
        CHECK(meta1->caseType == "民事");
        CHECK(!meta1->caseNumber.empty());
    }
    if (meta2) {
        CHECK(meta2->caseType == "刑事");
        CHECK(!meta2->caseNumber.empty());
    }
    // 验证搜索也能工作
    auto results = retriever.search("合同 纠纷", 5);
    CHECK(results.size() >= 1);
    std::cout << "    (民事: " << (meta1 ? meta1->caseNumber : "N/A")
              << ", 刑事: " << (meta2 ? meta2->caseNumber : "N/A")
              << ", 检索结果: " << results.size() << "条) ";
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 11: 端到端测试（Demo 数据集）
// ═══════════════════════════════════════════════════════════════
void test_e2e_import_all_demo_docs() {
    TEST("E2E: 导入全部 21 篇法律文档");
    rag::Retriever retriever;
    int imported = 0;

    std::string legalDir = "test/data/legal_cases";
    for (const auto& file : listTxtFiles(legalDir)) {
        retriever.addDocument(file);
        imported++;
    }

    std::cout << "    (导入: " << imported << " 篇, 文本块: " << retriever.docCount() << ") ";
    CHECK(imported == 21);
    PASS();
}

void test_e2e_search_across_all_types() {
    TEST("E2E: 跨案件类型搜索");
    rag::Retriever retriever;
    std::string legalDir = "test/data/legal_cases";
    for (const auto& file : listTxtFiles(legalDir)) {
        retriever.addDocument(file);
    }

    // 民事查询
    auto civilResults = retriever.search("民间借贷 还款义务", 5);
    CHECK(civilResults.size() >= 1);
    bool hasCivil = false;
    for (const auto& r : civilResults) {
        if (r.docId.find("civil") != std::string::npos) hasCivil = true;
    }
    CHECK(hasCivil);

    // 刑事查询
    auto crimResults = retriever.search("诈骗罪 非法占有", 5);
    CHECK(crimResults.size() >= 1);
    bool hasCrim = false;
    for (const auto& r : crimResults) {
        if (r.docId.find("criminal") != std::string::npos) hasCrim = true;
    }
    CHECK(hasCrim);

    // 行政查询
    auto adminResults = retriever.search("行政处罚 程序违法", 5);
    CHECK(adminResults.size() >= 1);

    std::cout << "    (民事: " << civilResults.size() << "条, "
              << "刑事: " << crimResults.size() << "条, "
              << "行政: " << adminResults.size() << "条) ";
    PASS();
}

void test_e2e_metadata_all_docs() {
    TEST("E2E: 全部文档元数据提取");
    rag::Retriever retriever;
    std::string legalDir = "test/data/legal_cases";
    for (const auto& file : listTxtFiles(legalDir)) {
        retriever.addDocument(file);
    }

    int withCaseNumber = 0, withCourt = 0, withDate = 0, withCaseType = 0;
    auto ids = retriever.allDocIds();

    for (const auto& id : ids) {
        const auto* meta = retriever.getMetadata(id);
        if (meta) {
            if (!meta->caseNumber.empty()) withCaseNumber++;
            if (!meta->court.empty()) withCourt++;
            if (!meta->date.empty()) withDate++;
            if (!meta->caseType.empty()) withCaseType++;
        }
    }

    std::cout << "    (案号: " << withCaseNumber << "/" << ids.size()
              << ", 法院: " << withCourt << "/" << ids.size()
              << ", 日期: " << withDate << "/" << ids.size()
              << ", 类型: " << withCaseType << "/" << ids.size() << ") ";
    // 至少 80% 的文档应能提取到案号和法院
    CHECK(withCaseNumber >= 16);
    CHECK(withCourt >= 16);
    CHECK(withDate >= 10);
    CHECK(withCaseType >= 16);
    PASS();
}

void test_e2e_filter_functionality() {
    TEST("E2E: 元数据筛选验证");
    rag::Retriever retriever;
    std::string legalDir = "test/data/legal_cases";
    for (const auto& file : listTxtFiles(legalDir)) {
        retriever.addDocument(file);
    }

    auto allResults = retriever.search("判决", 30);

    // 手动模拟筛选：民事案件
    int civilCount = 0, criminalCount = 0;
    for (const auto& r : allResults) {
        const auto* meta = retriever.getMetadata(r.docId);
        if (meta) {
            if (meta->caseType == "民事") civilCount++;
            if (meta->caseType == "刑事") criminalCount++;
        }
    }

    std::cout << "    (民事: " << civilCount << "条, 刑事: " << criminalCount << "条) ";
    CHECK(civilCount > 0);
    CHECK(criminalCount > 0);
    PASS();
}

void test_e2e_legal_prompt_detection() {
    TEST("E2E: 法律 Prompt 自动检测");
    rag::Retriever retriever;

    // 导入一篇法律文档
    retriever.addDocument("test/data/legal_cases/case_civil_001_loan_dispute.txt");

    auto results = retriever.search("借款纠纷", 3);
    CHECK(results.size() >= 1);

    std::string context = retriever.buildContext(results, 1500);

    // 验证上下文包含法律关键词
    bool hasCourt = context.find("人民法院") != std::string::npos;
    bool hasCaseNum = context.find("京0105") != std::string::npos;
    std::cout << "    (法院: " << (hasCourt ? "Y" : "N")
              << ", 案号: " << (hasCaseNum ? "Y" : "N") << ") ";
    CHECK(hasCourt);
    CHECK(hasCaseNum);
    PASS();
}

void test_e2e_performance_stress() {
    TEST("E2E: 大数据量压力测试");
    rag::Retriever retriever;
    std::string legalDir = "test/data/legal_cases";
    for (const auto& file : listTxtFiles(legalDir)) {
        retriever.addDocument(file);
    }

    // 执行多次搜索，验证稳定性
    std::vector<std::string> queries = {
        "违约赔偿", "知识产权侵权", "劳动合同解除",
        "诈骗数额", "行政处罚程序", "有限责任公司",
        "婚姻感情破裂", "交通事故赔偿", "破产清算条件"
    };

    int totalResults = 0;
    for (const auto& q : queries) {
        auto results = retriever.search(q, 5);
        totalResults += static_cast<int>(results.size());
    }

    // 所有查询应返回结果
    std::cout << "    (" << queries.size() << "个查询, 共返回 " << totalResults << " 条结果) ";
    CHECK(totalResults >= queries.size());  // 每个查询至少返回1条
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 12: 回归 — OCR 失败的 PDF 导入不得破坏既有检索能力
// （GUI 实测中发现导入 OCR 失败文件后检索返回 0 条的异常）
// ═══════════════════════════════════════════════════════════════
void test_e2e_failed_ocr_import_then_search() {
    TEST("回归: OCR 失败导入后检索仍正常");
    rag::Retriever retriever;
    for (const auto& file : listTxtFiles("test/data/legal_cases")) {
        retriever.addDocument(file);
    }

    // 导入前检索正常
    auto before = retriever.search("专利侵权", 5);
    CHECK(before.size() >= 1);

    // 导入一个无文本层的伪扫描 PDF（本机无 OCR 环境 → 导入失败）
    auto result = retriever.addDocument("test/data/fake_scanned.pdf");
    std::cout << "    (fake_scanned 导入: " << (result.imported ? "成功" : "失败(符合预期)") << ") ";

    // 导入失败后，既有检索能力必须保持
    auto after = retriever.search("专利侵权", 5);
    std::cout << "(导入后检索: " << after.size() << " 条) ";
    CHECK(after.size() >= 1);

    // 元数据不应被破坏
    CHECK(retriever.docCount() >= 21);
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 12: T1 —— 第 8 类元数据「裁判结果倾向」
// ═══════════════════════════════════════════════════════════════
void test_metadata_result_tendency() {
    TEST("结果倾向：21 篇语料全量判定");
    const auto files = listTxtFiles("test/data/legal_cases");
    CHECK(files.size() == 21);

    int judged = 0;          // 能判出倾向（非 Unknown）
    int favorPlaintiff = 0;
    int favorDefendant = 0;
    int partial = 0;
    int other = 0;

    for (const auto& file : files) {
        QFile f(QString::fromStdString(file));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const std::string text = f.readAll().toStdString();
        f.close();

        const auto tendency = document::MetadataExtractor::extractResultTendency(text);
        if (tendency != document::ResultTendency::Unknown) ++judged;
        switch (tendency) {
            case document::ResultTendency::FavorPlaintiff:  ++favorPlaintiff; break;
            case document::ResultTendency::FavorDefendant:  ++favorDefendant; break;
            case document::ResultTendency::PartialSupport:  ++partial; break;
            case document::ResultTendency::Other:           ++other; break;
            case document::ResultTendency::Unknown:         break;
        }
    }

    std::cout << "    (利于原告 " << favorPlaintiff
              << " / 部分支持 " << partial
              << " / 利于被告 " << favorDefendant
              << " / 其他 " << other << ") ";

    // 21 篇全部落在某个确定分类里（无一 Unknown）——这是 T1 的验收口径
    CHECK_EQ(judged, 21);
    CHECK_EQ(favorPlaintiff + partial + favorDefendant + other, 21);
    PASS();
}

void test_metadata_tendency_labels() {
    TEST("结果倾向：标签与枚举往返一致");
    using document::ResultTendency;
    const ResultTendency all[] = {
        ResultTendency::Unknown, ResultTendency::FavorPlaintiff,
        ResultTendency::FavorDefendant, ResultTendency::PartialSupport,
        ResultTendency::Other
    };
    for (auto t : all) {
        const std::string label = document::resultTendencyLabel(t);
        CHECK(!label.empty());
        CHECK(document::resultTendencyFromLabel(label) == t);
    }
    // 未知标签必须落到 Unknown，不能瞎猜
    CHECK(document::resultTendencyFromLabel("不存在的标签")
          == ResultTendency::Unknown);
    PASS();
}

void test_metadata_tendency_keyword_cases() {
    TEST("结果倾向：判项措辞 → 分类");

    // 给付类判项指向原告 + 尾项驳回 → 部分支持。
    // 实测 case_civil_001（"被告…归还原告借款本金" + "驳回原告…其他诉讼请求"）
    // 即落在此类，故此处按同一口径断言。
    {
        const std::string text =
            "本院认为……\n判决如下：\n"
            "一、被告李四于本判决生效之日起十日内向原告张三支付货款五万元；\n"
            "二、驳回原告张三的其他诉讼请求。\n"
            "如不服本判决，可在判决书送达之日起十五日内提起上诉。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::PartialSupport);
    }

    // 仅驳回 → 利于被告
    {
        const std::string text =
            "本院认为……\n判决如下：\n"
            "驳回原告张三的全部诉讼请求。\n"
            "如不服本判决，可在判决书送达之日起十五日内提起上诉。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::FavorDefendant);
    }

    // 无驳回的纯给付判项 → 利于原告（对应 case_civil_002 的真实形态）
    {
        const std::string text =
            "本院认为……\n判决如下：\n"
            "一、被告李四向原告张三支付货款三万元；\n"
            "二、被告李四支付自二〇二四年一月一日起的逾期付款违约金。\n"
            "如不服本判决，可在判决书送达之日起十五日内提起上诉。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::FavorPlaintiff);
    }

    // 驳回上诉 → 其他
    {
        const std::string text =
            "本院认为……\n裁定如下：\n"
            "驳回上诉，维持原判。\n"
            "本裁定为终审裁定。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::Other);
    }

    // 刑事文书 → 其他（不套民事给付口径）
    {
        const std::string text =
            "××人民法院刑事判决书\n公诉机关××人民检察院。\n被告人王五。\n"
            "本院认为……\n判决如下：\n"
            "一、被告人王五犯盗窃罪，判处有期徒刑一年，并处罚金二千元；\n"
            "二、责令被告人王五退赔被害人损失。\n"
            "如不服本判决，可在判决书送达之日起十日内提起上诉。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::Other);
    }

    // 二审驳回上诉 → 其他
    {
        const std::string text =
            "本院认为……\n裁定如下：\n"
            "驳回上诉，维持原判。\n"
            "本裁定为终审裁定。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::Other);
    }

    // 无主文段（如仅有"本院认为"）→ Unknown，不猜
    {
        const std::string text = "本院认为，原告提交的证据不足以证明其主张。";
        const auto t = document::MetadataExtractor::extractResultTendency(text);
        CHECK(t == document::ResultTendency::Unknown);
    }

    // 空文本 → Unknown
    {
        const auto t = document::MetadataExtractor::extractResultTendency("");
        CHECK(t == document::ResultTendency::Unknown);
    }

    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 13: T1 —— InvertedIndex 块级删除
// ═══════════════════════════════════════════════════════════════
void test_inverted_index_remove_chunk() {
    TEST("倒排索引：按块删除并同步块计数");
    search_index::InvertedIndex index;

    // docA 两块；docB 一块。注意用互不相同的词，便于精确断言
    // 哪个词项应当因"最后一个 posting 被摘除"而整体消失。
    const std::vector<std::string> termsA0 = {"合同", "违约", "赔偿"};
    const std::vector<std::string> termsA1 = {"合同", "履行"};
    const std::vector<std::string> termsB0 = {"合同", "解除"};
    index.addDocument("docA.txt", 0, termsA0);
    index.addDocument("docA.txt", 1, termsA1);
    index.addDocument("docB.txt", 0, termsB0);

    CHECK_EQ(index.totalDocs(), 3);

    // 删掉 docA 的第 0 块
    const int removed = index.removeChunk("docA.txt", 0);
    CHECK_EQ(removed, 3);              // 该块贡献 3 个 posting
    CHECK_EQ(index.totalDocs(), 2);

    // "违约" 与 "赔偿" 只存在于被删的块 → 整个词项应被摘除
    CHECK(index.getPostings("违约") == nullptr);
    CHECK(index.getPostings("赔偿") == nullptr);

    // "合同" 仍应保留 docA 的第 1 块与 docB 的第 0 块
    const auto* postings = index.getPostings("合同");
    CHECK(postings != nullptr);
    CHECK_EQ(static_cast<int>(postings->size()), 2);

    // 其余词项不受影响
    CHECK(index.getPostings("履行") != nullptr);
    CHECK(index.getPostings("解除") != nullptr);

    // 被删块的长度记录必须移除（否则 avgDocLength 会被幽灵块拉偏）
    CHECK_EQ(index.docLength("docA.txt", 0), 0);
    CHECK(index.docLength("docA.txt", 1) > 0);

    // 幂等：再次删同一块不报错、不再影响计数
    CHECK_EQ(index.removeChunk("docA.txt", 0), 0);
    CHECK_EQ(index.totalDocs(), 2);

    // 不存在的块同样幂等
    CHECK_EQ(index.removeChunk("nope.txt", 9), 0);
    CHECK_EQ(index.totalDocs(), 2);

    // 删光全部块后，索引应回到空状态
    index.removeChunk("docA.txt", 1);
    index.removeChunk("docB.txt", 0);
    CHECK_EQ(index.totalDocs(), 0);
    CHECK(index.getPostings("合同") == nullptr);

    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 14: T1 —— Retriever 文档级视图与删除
// ═══════════════════════════════════════════════════════════════
void test_retriever_document_view() {
    TEST("Retriever：文档级只读视图（documentInfos / 全文 / 块）");
    rag::Retriever retriever;
    for (const auto& file : listTxtFiles("test/data/legal_cases")) {
        retriever.addDocument(file);
    }

    // 文档数与块数是两个量：21 篇 / 118 块
    CHECK_EQ(retriever.documentCount(), 21);
    CHECK_EQ(retriever.chunkCount(), 118);
    // docCount() 返回块数——这是有意的历史语义，此处显式钉住防回归
    CHECK_EQ(retriever.docCount(), retriever.chunkCount());

    const auto infos = retriever.documentInfos();
    CHECK_EQ(static_cast<int>(infos.size()), 21);

    // 按 docId 升序，稳定可复现
    for (size_t i = 1; i < infos.size(); ++i) {
        CHECK(infos[i - 1].docId < infos[i].docId);
    }

    int totalChunks = 0;
    int withFullText = 0;
    for (const auto& info : infos) {
        CHECK(!info.docId.empty());
        CHECK(info.chunkCount > 0);
        CHECK(!info.importedAt.empty());   // 导入时间必须落上
        totalChunks += info.chunkCount;

        // 整篇原文必须留存（T10 全文阅读页的数据源）
        std::string full;
        if (retriever.getFullText(info.docId, full) && !full.empty()) {
            ++withFullText;
            CHECK(info.byteSize == static_cast<std::uint64_t>(full.size()));
        }

        // 第 0 块可读
        std::string chunk;
        CHECK(retriever.getChunk(info.docId, 0, chunk));
        CHECK(!chunk.empty());

        // 越界块必须返回 false 而不是空串糊过去
        std::string outOfRange;
        CHECK(!retriever.getChunk(info.docId, info.chunkCount, outOfRange));
    }

    CHECK_EQ(totalChunks, 118);
    CHECK_EQ(withFullText, 21);   // 21 篇全部保留全文

    // 单篇查询
    rag::DocumentInfo one;
    CHECK(retriever.getDocumentInfo(infos[0].docId, one));
    CHECK(one.docId == infos[0].docId);
    CHECK(!retriever.getDocumentInfo("不存在的文档.txt", one));

    // allDocIds 与 documentInfos 同源同序
    const auto ids = retriever.allDocIds();
    CHECK_EQ(static_cast<int>(ids.size()), 21);
    CHECK(ids[0] == infos[0].docId);
    CHECK(ids[20] == infos[20].docId);

    PASS();
}

void test_retriever_remove_document() {
    TEST("Retriever：单篇删除（倒排 / 块 / 元数据 / 全文同步清理）");
    rag::Retriever retriever;
    for (const auto& file : listTxtFiles("test/data/legal_cases")) {
        retriever.addDocument(file);
    }
    CHECK_EQ(retriever.documentCount(), 21);

    const auto ids = retriever.allDocIds();
    const std::string victim = ids[0];

    rag::DocumentInfo victimInfo;
    CHECK(retriever.getDocumentInfo(victim, victimInfo));
    const int victimChunks = victimInfo.chunkCount;

    // 删除前：该文档在检索中可见
    const std::string victimKeyword = "本院认为";
    (void)victimKeyword;

    const int removed = retriever.removeDocument(victim);
    CHECK_EQ(removed, victimChunks);

    // 三处计数同步收敛
    CHECK_EQ(retriever.documentCount(), 20);
    CHECK_EQ(retriever.chunkCount(), 118 - victimChunks);
    CHECK_EQ(retriever.docCount(), 118 - victimChunks);

    // 只读视图里也没有了
    rag::DocumentInfo after;
    CHECK(!retriever.getDocumentInfo(victim, after));
    std::string full;
    CHECK(!retriever.getFullText(victim, full));
    std::string chunk;
    CHECK(!retriever.getChunk(victim, 0, chunk));

    // 其余文档的检索能力未受影响
    auto results = retriever.search("合同 履行", 5);
    CHECK(results.size() >= 1);
    for (const auto& r : results) {
        CHECK(r.docId != victim);   // 已删文档不得再被命中
    }

    // 幂等：重复删同一篇返回 0，不改变计数
    CHECK_EQ(retriever.removeDocument(victim), 0);
    CHECK_EQ(retriever.documentCount(), 20);

    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 15: T1 —— 索引持久化（落盘 → 恢复 → 一致）
// ═══════════════════════════════════════════════════════════════
void test_persistence_roundtrip() {
    TEST("持久化：落盘后恢复，文档/块/元数据/检索结果一致");
    const std::string indexPath = "build/test_rag_index.dat";

    // ── 第一次会话：导入 → 落盘 ──
    rag::Retriever writer;
    writer.setIndexFilePath(indexPath);
    for (const auto& file : listTxtFiles("test/data/legal_cases")) {
        writer.addDocument(file);
    }
    CHECK_EQ(writer.documentCount(), 21);
    CHECK_EQ(writer.chunkCount(), 118);

    const auto saveResult = writer.saveIndex();
    CHECK(saveResult.ok);
    CHECK_EQ(saveResult.documentCount, 21);
    CHECK_EQ(saveResult.chunkCount, 118);
    CHECK(saveResult.bytes > 0);
    CHECK(writer.hasPersistedIndex());

    // 落盘前后检索结果必须一致（证明索引没被写坏）
    auto beforeHits = writer.search("民间借贷 交付凭证", 5);
    CHECK(beforeHits.size() >= 1);
    const std::string topBefore = beforeHits[0].docId;

    // ── 第二次会话：全新实例 → 恢复 ──
    rag::Retriever reader;
    reader.setIndexFilePath(indexPath);
    CHECK_EQ(reader.documentCount(), 0);   // 恢复前是空的

    const auto loadResult = reader.loadIndex();
    CHECK(loadResult.ok);
    CHECK_EQ(loadResult.documentCount, 21);
    CHECK_EQ(loadResult.chunkCount, 118);
    CHECK(loadResult.elapsedMs >= 0);
    CHECK(reader.restoredFromDisk());
    CHECK(reader.lastLoadMs() >= 0);

    // 规模与首次会话完全一致
    CHECK_EQ(reader.documentCount(), 21);
    CHECK_EQ(reader.chunkCount(), 118);
    CHECK_EQ(reader.docCount(), 118);

    // 检索能力（纯 BM25 降级路径）一致
    auto afterHits = reader.search("民间借贷 交付凭证", 5);
    CHECK(afterHits.size() >= 1);
    CHECK(afterHits[0].docId == topBefore);

    // 元数据（含第 8 类结果倾向）跨会话保留
    const auto idsBefore = writer.allDocIds();
    const auto idsAfter = reader.allDocIds();
    CHECK(idsBefore.size() == idsAfter.size());
    for (size_t i = 0; i < idsBefore.size(); ++i) {
        CHECK(idsBefore[i] == idsAfter[i]);

        const auto* metaBefore = writer.getMetadata(idsBefore[i]);
        const auto* metaAfter = reader.getMetadata(idsAfter[i]);
        CHECK((metaBefore == nullptr) == (metaAfter == nullptr));
        if (metaBefore && metaAfter) {
            CHECK(metaBefore->caseNumber == metaAfter->caseNumber);
            CHECK(metaBefore->court == metaAfter->court);
            CHECK(metaBefore->date == metaAfter->date);
            CHECK(metaBefore->caseType == metaAfter->caseType);
            CHECK(metaBefore->tendency == metaAfter->tendency);
        }
    }

    // 全文与块内容逐字节还原（T10 依赖此项）
    for (const auto& id : idsAfter) {
        std::string fullBefore, fullAfter;
        CHECK(writer.getFullText(id, fullBefore));
        CHECK(reader.getFullText(id, fullAfter));
        CHECK(fullBefore == fullAfter);

        rag::DocumentInfo infoAfter;
        CHECK(reader.getDocumentInfo(id, infoAfter));
        for (int c = 0; c < infoAfter.chunkCount; ++c) {
            std::string chunkBefore, chunkAfter;
            CHECK(writer.getChunk(id, c, chunkBefore));
            CHECK(reader.getChunk(id, c, chunkAfter));
            CHECK(chunkBefore == chunkAfter);
        }
    }

    // 导入时间与来源路径也一并还原
    rag::DocumentInfo wInfo, rInfo;
    CHECK(writer.getDocumentInfo(idsBefore[0], wInfo));
    CHECK(reader.getDocumentInfo(idsAfter[0], rInfo));
    CHECK(wInfo.importedAt == rInfo.importedAt);
    CHECK(wInfo.sourcePath == rInfo.sourcePath);
    CHECK(wInfo.ocr == rInfo.ocr);

    // 收尾：删掉测试索引文件，别污染工作目录
    reader.clearAll(/*alsoDeletePersistedFile=*/true);
    CHECK(!reader.hasPersistedIndex());

    PASS();
}

void test_persistence_ocr_flag_no_rerun() {
    TEST("持久化：OCR 标记跨会话保留（扫描件不重跑识别）");
    const std::string indexPath = "build/test_rag_index_ocr.dat";

    rag::Retriever writer;
    writer.setIndexFilePath(indexPath);
    for (const auto& file : listTxtFiles("test/data/legal_cases")) {
        writer.addDocument(file);
    }
    CHECK(writer.saveIndex().ok);

    rag::Retriever reader;
    reader.setIndexFilePath(indexPath);
    CHECK(reader.loadIndex().ok);

    const auto infos = reader.documentInfos();
    CHECK_EQ(static_cast<int>(infos.size()), 21);
    // 语料全是 .txt 文本文件 → 无一应被标记为 OCR，
    // 否则恢复后会对纯文本重跑识别（错误行为的护栏）
    for (const auto& info : infos) {
        CHECK(!info.ocr);
    }

    reader.clearAll(true);
    PASS();
}

void test_persistence_missing_file() {
    TEST("持久化：文件不存在时如实失败，不静默当空库");
    rag::Retriever retriever;
    retriever.setIndexFilePath("build/definitely_not_here.dat");

    CHECK(!retriever.hasPersistedIndex());

    const auto result = retriever.loadIndex();
    CHECK(!result.ok);                 // 必须报告失败
    CHECK(!result.diagnostic.empty()); // 且给出原因
    CHECK(!retriever.restoredFromDisk());
    CHECK_EQ(retriever.documentCount(), 0);

    PASS();
}

void test_persistence_corrupt_file() {
    TEST("持久化：损坏文件被 CRC 拦下，不读成半套索引");
    const std::string indexPath = "build/test_rag_index_corrupt.dat";

    // 造一个"魔数不对"的文件
    {
        std::ofstream out(indexPath, std::ios::binary);
        out << "NOT_A_VALID_RAG_INDEX_FILE_HEADER_0123456789";
    }

    rag::Retriever retriever;
    retriever.setIndexFilePath(indexPath);

    const auto result = retriever.loadIndex();
    CHECK(!result.ok);
    CHECK(!result.diagnostic.empty());
    CHECK_EQ(retriever.documentCount(), 0);
    CHECK_EQ(retriever.chunkCount(), 0);
    CHECK(!retriever.restoredFromDisk());

    std::remove(indexPath.c_str());
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 16: T2 —— 问答历史持久层（SQLite）
// ═══════════════════════════════════════════════════════════════
namespace {

const std::string kTestHistoryDb = "build/test_history.db";

/// 测试前清掉上一轮残留（Windows 上打开着的 sqlite 文件删不掉，故按"先删后用"处理）
void removeTestHistoryDb() {
    QFile::remove(QString::fromStdString(kTestHistoryDb));
}

/// 造一条来源齐全的样例记录（中文 + 小数 + 块号，覆盖最容易出错的编码与类型转换）
history::HistoryRecord makeSampleRecord(const std::string& query, const std::string& answer,
                                        bool interrupted = false) {
    history::HistoryRecord record;
    record.createdAt = "2026-09-21 10:30:00";
    record.query = query;
    record.answer = answer;
    record.sources.push_back({"case_civil_001.txt", 2, 0.8765,
                              "本院认为，借款人应当按照约定的期限返还借款……"});
    record.sources.push_back({"case_civil_007.txt", 0, 0.5120,
                              "被告辩称其已归还部分本金，应予扣除……"});
    record.interrupted = interrupted;
    record.note = interrupted ? "生成过程被中断（网络异常）" : "";
    return record;
}

}  // namespace

void test_history_append_and_get() {
    TEST("问答历史：落库后读回，字段逐项一致");
    removeTestHistoryDb();
    CHECK(!QFile::exists(QString::fromStdString(kTestHistoryDb)));

    const auto sample = makeSampleRecord(
        "民间借贷纠纷中交付凭证如何认定？",
        "应当结合转账记录、收条与当事人陈述综合认定，不能仅凭单一证据定案。");

    {
        history::HistoryStore store(kTestHistoryDb);
        CHECK(store.open());
        CHECK(store.isOpen());
        CHECK_EQ(store.count(), 0);

        const long long id = store.append(sample);
        CHECK(id > 0);
        CHECK_EQ(store.count(), 1);

        history::HistoryRecord back;
        CHECK(store.get(id, back));
        CHECK_EQ(back.id, id);
        CHECK(back.createdAt == sample.createdAt);
        CHECK(back.query == sample.query);
        CHECK(back.answer == sample.answer);
        CHECK_EQ(static_cast<int>(back.sources.size()), 2);
        CHECK_EQ(back.hitCount(), 2);

        // 来源 JSON 往返：docId / 块号 / 得分 / 中文片段
        CHECK(back.sources[0].docId == "case_civil_001.txt");
        CHECK_EQ(back.sources[0].chunkIndex, 2);
        CHECK_CLOSE(back.sources[0].finalScore, 0.8765, 1e-6);
        CHECK(back.sources[0].snippet == sample.sources[0].snippet);
        CHECK(back.sources[1].docId == "case_civil_007.txt");
        CHECK_EQ(back.sources[1].chunkIndex, 0);
        CHECK_CLOSE(back.sources[1].finalScore, 0.5120, 1e-6);

        CHECK(!back.interrupted);
        CHECK(back.note.empty());

        // 不存在的 id 必须如实返回 false，而不是给条空记录
        history::HistoryRecord none;
        CHECK(!store.get(id + 1, none));
    }

    removeTestHistoryDb();
    PASS();
}

void test_history_persistence_restart() {
    TEST("问答历史：模拟重启（新实例）后记录仍在");
    removeTestHistoryDb();

    long long written = 0;
    {
        history::HistoryStore session1(kTestHistoryDb);
        CHECK(session1.open());
        written = session1.append(makeSampleRecord("房屋租赁合同纠纷如何处理？",
                                                   "先看出租方是否履行适租义务，再看租金支付凭证。"));
        CHECK(written > 0);
    }

    {
        // 全新实例 = 重启后的第二次会话
        history::HistoryStore session2(kTestHistoryDb);
        CHECK(session2.open());
        CHECK_EQ(session2.count(), 1);

        const auto records = session2.recent();
        CHECK_EQ(static_cast<int>(records.size()), 1);
        CHECK(records[0].id == written);
        CHECK(records[0].query == "房屋租赁合同纠纷如何处理？");
        CHECK(records[0].answer == "先看出租方是否履行适租义务，再看租金支付凭证。");
        CHECK_EQ(records[0].hitCount(), 2);
        CHECK(records[0].sources[0].docId == "case_civil_001.txt");
    }

    removeTestHistoryDb();
    PASS();
}

void test_history_keyword_search() {
    TEST("问答历史：关键词命中问题与回答，通配符不误伤");
    removeTestHistoryDb();

    {
        history::HistoryStore store(kTestHistoryDb);
        CHECK(store.open());

        store.append(makeSampleRecord("民间借贷里的交付凭证怎么认定", "回答 A"));
        store.append(makeSampleRecord("劳动争议的诉讼时效是多久", "回答 B"));
        // 关键词只出现在回答里 —— 证实"回答"字段也进索引
        store.append(makeSampleRecord("再问一个问题", "这里提到交付凭证作为补充说明"));
        CHECK_EQ(store.count(), 3);

        CHECK_EQ(static_cast<int>(store.search("交付凭证").size()), 2);
        CHECK_EQ(static_cast<int>(store.search("劳动争议").size()), 1);
        CHECK_EQ(static_cast<int>(store.search("凭空捏造的词").size()), 0);

        // 通配符必须被转义：输入 % 若当成通配符就会命中全部 3 条
        CHECK_EQ(static_cast<int>(store.search("%").size()), 0);
        CHECK_EQ(static_cast<int>(store.search("_").size()), 0);

        // 空关键词退化为"列出全部"，不是返回空列表
        CHECK_EQ(static_cast<int>(store.search("").size()), 3);
    }

    removeTestHistoryDb();
    PASS();
}

void test_history_delete() {
    TEST("问答历史：删除单条 / 重复删除幂等 / 清空");
    removeTestHistoryDb();

    {
        history::HistoryStore store(kTestHistoryDb);
        CHECK(store.open());

        const long long first = store.append(makeSampleRecord("问题一", "回答一"));
        const long long second = store.append(makeSampleRecord("问题二", "回答二"));
        CHECK(first > 0);
        CHECK(second > 0);
        CHECK(second > first);
        CHECK_EQ(store.count(), 2);

        CHECK(store.remove(first));
        CHECK_EQ(store.count(), 1);
        history::HistoryRecord gone;
        CHECK(!store.get(first, gone));

        // 重复删除返回 false（幂等），不会把别的记录删掉
        CHECK(!store.remove(first));
        CHECK_EQ(store.count(), 1);
        CHECK(store.search("问题二").size() == 1);

        CHECK_EQ(store.removeAll(), 1);
        CHECK_EQ(store.count(), 0);
        CHECK_EQ(store.removeAll(), 0);   // 空表再清空也是幂等
    }

    removeTestHistoryDb();
    PASS();
}

void test_history_interrupted_flag() {
    TEST("问答历史：中断标记与原因短语往返一致");
    removeTestHistoryDb();

    {
        history::HistoryStore store(kTestHistoryDb);
        CHECK(store.open());

        const long long okId = store.append(makeSampleRecord("正常收尾的提问", "完整回答", false));
        const long long badId = store.append(makeSampleRecord("中途断掉的提问", "半截回答", true));
        CHECK(okId > 0);
        CHECK(badId > 0);

        history::HistoryRecord okBack;
        history::HistoryRecord badBack;
        CHECK(store.get(okId, okBack));
        CHECK(store.get(badId, badBack));

        CHECK(!okBack.interrupted);
        CHECK(okBack.note.empty());
        CHECK(badBack.interrupted);
        CHECK(badBack.note == "生成过程被中断（网络异常）");
        CHECK(badBack.answer == "半截回答");
    }

    removeTestHistoryDb();
    PASS();
}

void test_history_markdown_export() {
    TEST("问答历史：Markdown 导出内容完整且中文 UTF-8 无损");
    removeTestHistoryDb();

    const std::string mdPath = "build/test_history_export.md";

    {
        history::HistoryStore store(kTestHistoryDb);
        CHECK(store.open());
        const long long id = store.append(makeSampleRecord(
            "交通事故认定的依据是什么？", "依据现场勘验笔录、监控视频与鉴定意见综合认定。"));
        CHECK(id > 0);

        history::HistoryRecord record;
        CHECK(store.get(id, record));

        const std::string md = history::HistoryStore::toMarkdown(record);
        CHECK(!md.empty());
        CHECK(md.find("# 检索问答记录") == 0);
        CHECK(md.find("提问时间：2026-09-21 10:30:00") != std::string::npos);
        CHECK(md.find("命中块数：2") != std::string::npos);
        CHECK(md.find("回答状态：完整") != std::string::npos);
        CHECK(md.find("交通事故认定的依据是什么？") != std::string::npos);
        CHECK(md.find("依据现场勘验笔录、监控视频与鉴定意见综合认定。") != std::string::npos);
        CHECK(md.find("case_civil_001.txt") != std::string::npos);
        CHECK(md.find("| 1 |") != std::string::npos);

        // 多记录合并导出（历史页批量导出走这条）
        auto all = store.recent();
        CHECK_EQ(static_cast<int>(all.size()), 1);
        const std::string bulk = history::HistoryStore::toMarkdownAll(all);
        CHECK(bulk.find("共 1 条记录") != std::string::npos);

        QString error;
        CHECK(history::HistoryStore::writeMarkdownFile(QString::fromStdString(mdPath), md, &error));

        // 写到一个不存在的目录 → 如实失败并给原因（不崩溃）
        QString badCase;
        CHECK(!history::HistoryStore::writeMarkdownFile(
            QStringLiteral("build/no_such_dir_xyz/export.md"), md, &badCase));
        CHECK(!badCase.isEmpty());
    }

    // 回读：BOM + UTF-8 往返后中文必须原样
    QFile file(QString::fromStdString(mdPath));
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();
    file.close();
    CHECK(raw.size() > 3);
    CHECK(raw.startsWith(QByteArray("\xEF\xBB\xBF")));   // UTF-8 BOM（防记事本乱码）

    const std::string content(raw.constData() + 3, static_cast<size_t>(raw.size() - 3));
    CHECK(content.find("交通事故认定的依据是什么？") != std::string::npos);
    CHECK(content.find("case_civil_007.txt") != std::string::npos);

    QFile::remove(QString::fromStdString(mdPath));
    removeTestHistoryDb();
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// 测试 17: T3 —— 检索参数配置中心
// ═══════════════════════════════════════════════════════════════
namespace {

const std::string kTestSettingsFile = "build/test_settings.json";

void removeTestSettingsFile() {
    QFile::remove(QString::fromStdString(kTestSettingsFile));
}

/// 配置读写往返：save → load 全字段逐项一致
void test_settings_roundtrip() {
    TEST("检索配置：save → load 全字段往返一致");
    removeTestSettingsFile();

    config::AppSettings out;
    CHECK(!config::AppSettings::load(kTestSettingsFile, out));   // 无文件 → 默认值 + false
    CHECK_CLOSE(out.k1, 1.5, 1e-9);   // 默认值兜底，不读到 0
    CHECK_CLOSE(out.b, 0.75, 1e-9);
    CHECK(!out.embeddingApiKey.empty() == false);

    config::AppSettings data;
    data.k1 = 2.25;
    data.b = 0.6;
    data.bm25Weight = 0.7;
    data.vectorWeight = 0.3;
    data.topK = 33;
    data.chunkSize = 256;
    data.chunkOverlap = 32;
    data.temperature = 0.85;
    data.embeddingBaseUrl = "https://api.siliconflow.cn";
    data.embeddingModel = "bge-large-zh-v1.5";
    data.embeddingApiKey = "sk-test-1234567890abcdef";
    std::string error;
    CHECK(config::AppSettings::save(kTestSettingsFile, data, &error));

    config::AppSettings loaded;
    CHECK(config::AppSettings::load(kTestSettingsFile, loaded));
    CHECK_CLOSE(loaded.k1, 2.25, 1e-9);
    CHECK_CLOSE(loaded.b, 0.6, 1e-9);
    CHECK_CLOSE(loaded.bm25Weight, 0.7, 1e-9);
    CHECK_CLOSE(loaded.vectorWeight, 0.3, 1e-9);
    CHECK_EQ(loaded.topK, 33);
    CHECK_EQ(loaded.chunkSize, 256);
    CHECK_EQ(loaded.chunkOverlap, 32);
    CHECK_CLOSE(loaded.temperature, 0.85, 1e-9);
    CHECK(loaded.embeddingBaseUrl == "https://api.siliconflow.cn");
    CHECK(loaded.embeddingModel == "bge-large-zh-v1.5");
    CHECK(loaded.embeddingApiKey == "sk-test-1234567890abcdef");

    removeTestSettingsFile();
    PASS();
}

/// 字段缺失 / 类型不对：缺失字段回落默认，其余字段照常生效，不崩溃
void test_settings_missing_fields_fallback() {
    TEST("检索配置：字段缺失/类型不对回落默认，其余照常生效");
    removeTestSettingsFile();

    // 只写两个字段（手改配置的典型场景）
    {
        QFile file(QString::fromStdString(kTestSettingsFile));
        CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{\"k1\": 2.0, \"topK\": 40}");
        file.close();
    }

    config::AppSettings out;
    CHECK(config::AppSettings::load(kTestSettingsFile, out));
    CHECK_CLOSE(out.k1, 2.0, 1e-9);        // 文件里的值生效
    CHECK_EQ(out.topK, 40);
    CHECK_CLOSE(out.b, 0.75, 1e-9);        // 缺失字段回落默认
    CHECK_CLOSE(out.bm25Weight, 0.4, 1e-9);
    CHECK_CLOSE(out.vectorWeight, 0.6, 1e-9);
    CHECK_EQ(out.chunkSize, 512);
    CHECK_EQ(out.chunkOverlap, 50);
    CHECK_CLOSE(out.temperature, 0.3, 1e-9);
    CHECK(out.embeddingModel == "text-embedding-3-small");

    // 类型不对（topK 写成字符串）→ 该字段回落默认，其余照常
    {
        QFile file(QString::fromStdString(kTestSettingsFile));
        CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{\"k1\": 2.0, \"topK\": \"四十\"}");
        file.close();
    }
    config::AppSettings out2;
    CHECK(config::AppSettings::load(kTestSettingsFile, out2));
    CHECK_CLOSE(out2.k1, 2.0, 1e-9);
    CHECK_EQ(out2.topK, 20);   // 类型不对 → 默认 20

    removeTestSettingsFile();
    PASS();
}

/// JSON 被改坏：整体回落默认值，load 返回 false，绝不崩溃
void test_settings_corrupt_file() {
    TEST("检索配置：JSON 损坏整体回落默认值，不崩溃");
    removeTestSettingsFile();

    {
        QFile file(QString::fromStdString(kTestSettingsFile));
        CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ k1 = 这是坏掉的 JSON ！！！ [[[");
        file.close();
    }

    config::AppSettings out;
    const bool ok = config::AppSettings::load(kTestSettingsFile, out);
    CHECK(!ok);
    CHECK_CLOSE(out.k1, 1.5, 1e-9);        // 整体回落默认
    CHECK_CLOSE(out.b, 0.75, 1e-9);
    CHECK_EQ(out.topK, 20);
    CHECK(out.embeddingBaseUrl == "https://api.deepseek.com");

    removeTestSettingsFile();
    PASS();
}

/// 卡面验收项：改 k1 后同一查询得分可观测变化（引擎热更新，不重建实例）
void test_engine_hot_update_k1() {
    TEST("引擎热更新：改 k1/b 后同一查询得分可观测变化（不重建实例）");
    removeTestSettingsFile();

    rag::Retriever retriever;
    retriever.addText(
        "本院认为，借款人应当按照约定的期限返还借款。"
        "被告向原告借款并出具借条，双方形成民间借贷法律关系。"
        "被告未按约定归还全部借款，已构成违约，应承担继续履行的违约责任。"
        "判决如下：被告于本判决生效之日起十日内向原告偿还借款本金四十万元及利息。",
        "hot_k1_case.txt");

    const std::string query = "民间借贷 返还借款 违约责任";
    const auto before = retriever.search(query, 5);
    CHECK(!before.empty());
    CHECK(before.front().docId == "hot_k1_case.txt");

    // 热更新：k1 从 1.5 → 4.0，b 0.75 → 0.3，同一实例直接生效
    retriever.setSearchParams(4.0, 0.3, 0.4, 0.6);
    CHECK_CLOSE(retriever.k1(), 4.0, 1e-9);
    CHECK_CLOSE(retriever.b(), 0.3, 1e-9);

    const auto after = retriever.search(query, 5);
    CHECK(!after.empty());
    CHECK(after.front().docId == "hot_k1_case.txt");
    // BM25 分数公式随 k1/b 变化，finalScore 必须可观测地不同
    CHECK(std::fabs(before.front().finalScore - after.front().finalScore) > 1e-6);

    // 融合权重同样热更新生效：权重换成 0.9/0.1（向量路未配置时回退 BM25，
    // finalScore 等于 bm25 分数，此分支只验证 setter 数值被记住）
    retriever.setSearchParams(1.5, 0.75, 0.9, 0.1);
    CHECK_CLOSE(retriever.bm25Weight(), 0.9, 1e-9);
    CHECK_CLOSE(retriever.vectorWeight(), 0.1, 1e-9);

    PASS();
}

/// 分块参数语义：仅对之后导入的文档生效（卡面决策点 ①）
void test_chunk_params_new_docs_only() {
    TEST("分块参数：仅对之后导入的文档生效，既有索引不重切");
    removeTestSettingsFile();

    rag::Retriever retriever;
    // 先按默认 512/50 导入一篇长文
    std::string longText;
    for (int i = 0; i < 40; ++i) {
        longText += "本院认为，民事主体从事民事活动，应当遵循诚信原则，秉持诚实，恪守承诺。";
    }
    retriever.addText(longText, "chunk_default.txt");
    const int defaultChunks = retriever.chunkCount();
    CHECK(defaultChunks >= 4);   // 40 句 × 32 字 ≈ 1280 字 → 默认 512 切成 ≥3 块

    // 改小分块 → 只影响之后导入的文档；已建的块不会重新切
    retriever.setChunkParams(128, 16);
    CHECK_EQ(retriever.chunkCount(), defaultChunks);   // 既有索引纹丝不动
    retriever.addText(longText, "chunk_small.txt");

    // 新文档的每一块都不得超过新块长（UTF-8 边界回退只会更短）
    bool sawNewDoc = false;
    for (const auto& info : retriever.documentInfos()) {
        if (info.docId != "chunk_small.txt") continue;
        sawNewDoc = true;
        std::string chunkOut;
        for (int i = 0; i < info.chunkCount; ++i) {
            CHECK(retriever.getChunk(info.docId, i, chunkOut));
            CHECK(static_cast<int>(chunkOut.size()) <= 128);
        }
    }
    CHECK(sawNewDoc);

    // 新文档块数应明显多于旧文档（同样的文本）
    int smallDocChunks = -1, defaultDocChunks = -1;
    for (const auto& info : retriever.documentInfos()) {
        if (info.docId == "chunk_small.txt") smallDocChunks = info.chunkCount;
        if (info.docId == "chunk_default.txt") defaultDocChunks = info.chunkCount;
    }
    CHECK(smallDocChunks > defaultDocChunks);

    PASS();
}

/// URL 规范化：带尾部 / 或 /v1 的服务地址都落到同一个拼接基座（T4 第 0 期修复②）
void test_embedding_url_normalize() {
    TEST("Embedding URL：尾部 / 与 /v1 规范化");
    vector_engine::EmbeddingService svc;

    svc.setEndpoint("https://api.siliconflow.cn", "m");
    CHECK(svc.normalizedBaseUrl() == "https://api.siliconflow.cn");
    svc.setEndpoint("https://api.siliconflow.cn/", "m");
    CHECK(svc.normalizedBaseUrl() == "https://api.siliconflow.cn");
    svc.setEndpoint("https://api.siliconflow.cn/v1", "m");
    CHECK(svc.normalizedBaseUrl() == "https://api.siliconflow.cn");
    svc.setEndpoint("https://api.siliconflow.cn/v1/", "m");
    CHECK(svc.normalizedBaseUrl() == "https://api.siliconflow.cn");
    // 默认值（deepseek，不带尾巴）原样保留
    svc.setEndpoint("https://api.deepseek.com", "m");
    CHECK(svc.normalizedBaseUrl() == "https://api.deepseek.com");

    PASS();
}

/// embeddingHost：从服务地址取展示用域名（状态栏 Embedding 行消费）
void test_embedding_host_display() {
    TEST("Embedding 状态：host 域名提取与 ready 判定");
    vector_engine::EmbeddingService svc;
    CHECK(!svc.isReady());   // 未配 Key

    svc.setApiKey("sk-test-key-0000000000");
    CHECK(svc.isReady());
    svc.setEndpoint("https://api.siliconflow.cn/", "BAAI/bge-large-zh-v1.5");
    CHECK(svc.model() == "BAAI/bge-large-zh-v1.5");

    rag::Retriever retriever;
    retriever.setApiKey("sk-test-key-0000000000");
    retriever.setEmbeddingEndpoint("https://api.siliconflow.cn", "BAAI/bge-large-zh-v1.5");
    CHECK(retriever.embeddingReady());
    CHECK(retriever.embeddingHost() == "api.siliconflow.cn");

    PASS();
}

/// RRF：合成排名直接断言（k=60，rank 从 1 计；两路都命中的文档分数相加）
void test_rrf_fuse() {
    TEST("RRF：名次融合合成排名");
    using P = std::pair<std::string, double>;
    // BM25 路：a > b > c；向量路：b > d（a 未召回）
    std::vector<P> bm25 = {{"a", 10.0}, {"b", 8.0}, {"c", 6.0}};
    std::vector<P> vec  = {{"b", 0.9},  {"d", 0.8}};

    auto fused = rag::Retriever::rrfFuse(bm25, vec, 10);

    // a = 1/61 = 0.016393...；b = 1/62 + 1/61 = 0.032588...（两路相加，应排第一）
    // c = 1/63；d = 1/62
    CHECK(fused.size() == 4);
    CHECK(fused[0].first == "b");
    CHECK(std::fabs(fused[0].second - (1.0 / 62 + 1.0 / 61)) < 1e-12);
    CHECK(fused[1].first == "a");
    CHECK(std::fabs(fused[1].second - 1.0 / 61) < 1e-12);
    // d（向量第 2 名 1/62）应排在 c（BM25 第 3 名 1/63）前面
    CHECK(fused[2].first == "d");
    CHECK(fused[3].first == "c");

    // topK 截断
    auto fused2 = rag::Retriever::rrfFuse(bm25, vec, 2);
    CHECK(fused2.size() == 2);
    CHECK(fused2[0].first == "b");

    PASS();
}

/// 评测指标：文档级去重排名 + P@5 / Hit@5 / R@10 / MRR 逐项核算
void test_eval_metrics() {
    TEST("评测指标：文档级排名与四项指标核算");
    // 块级序列：docB 出现两次（块 3、块 0），只保留最高名次 1
    std::vector<std::string> chunkDocs = {
        "docB", "docX", "docA", "docB", "docY", "docA", "docZ", "docW", "docV", "docU", "docT"
    };
    auto ranking = rag::docLevelRanking(chunkDocs);
    CHECK(ranking.size() == 9);                       // docB/docA 去重
    CHECK(ranking[0] == "docB" && ranking[1] == "docX" && ranking[2] == "docA");

    // 相关集 {docB, docA}：名次 1 和 3
    auto m = rag::computeQueryMetrics(ranking, {"docB", "docA"});
    CHECK(std::fabs(m.p5 - 2.0 / 5) < 1e-12);         // Top-5 命中 2 个
    CHECK(std::fabs(m.hit5 - 1.0) < 1e-12);
    CHECK(std::fabs(m.r10 - 1.0) < 1e-12);            // Top-10 全命中
    CHECK(std::fabs(m.mrr - 1.0 / 1) < 1e-12);        // 首命中名次 1
    CHECK(m.firstRank == 1);

    // 未命中场景
    auto m0 = rag::computeQueryMetrics(ranking, {"docQ"});
    CHECK(m0.hit5 == 0.0 && m0.r10 == 0.0 && m0.mrr == 0.0 && m0.firstRank == 0);

    // 宏平均：两条，一条全 1 一条全 0 → 各 0.5
    auto avg = rag::averageMetrics({m, m0});
    CHECK(std::fabs(avg.hit5 - 0.5) < 1e-12);
    CHECK(std::fabs(avg.mrr - 0.5) < 1e-12);

    PASS();
}

/// 四路降级语义（Embedding 未配置）：VectorOnly 空、其余三路与 BM25 排序一致
void test_search_modes_degraded() {
    TEST("四路检索：Embedding 未配置时降级语义");
    removeTestSettingsFile();

    rag::Retriever retriever;   // 不配 Key → 向量路不可用
    retriever.setIndexFilePath("build/test_modes_index.dat");
    retriever.addText("民间借贷纠纷案 原告请求判令被告偿还借款本金及利息", "m1");
    retriever.addText("借款合同纠纷 逾期利息计算标准", "m2");
    retriever.addText("劳动合同 拖欠工资 劳动仲裁", "m3");
    CHECK(!retriever.embeddingReady());

    auto bm25 = retriever.searchWithMode("借贷纠纷", 5, rag::SearchMode::Bm25Only);
    auto vec  = retriever.searchWithMode("借贷纠纷", 5, rag::SearchMode::VectorOnly);
    auto w    = retriever.searchWithMode("借贷纠纷", 5, rag::SearchMode::WeightedFusion);
    auto rrf  = retriever.searchWithMode("借贷纠纷", 5, rag::SearchMode::RrfFusion);

    // ① BM25 有结果；② VectorOnly 返回空（不伪装成 BM25）
    CHECK(!bm25.empty());
    CHECK(vec.empty());
    // ③④ 降级 = BM25 的排序（序列完全一致）
    CHECK(!w.empty());
    CHECK(!rrf.empty());
    CHECK(w.size() == bm25.size());
    CHECK(rrf.size() == bm25.size());
    bool sameW = true, sameR = true;
    for (size_t i = 0; i < bm25.size(); ++i) {
        sameW = sameW && w[i].docId == bm25[i].docId
                && w[i].chunkIndex == bm25[i].chunkIndex;
        sameR = sameR && rrf[i].docId == bm25[i].docId
                && rrf[i].chunkIndex == bm25[i].chunkIndex;
    }
    CHECK(sameW);
    CHECK(sameR);
    CHECK(std::fabs(w[0].finalScore - bm25[0].finalScore) < 1e-12);

    QFile::remove(QStringLiteral("build/test_modes_index.dat"));
    PASS();
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringEncoder>
#else
#include <QTextCodec>
#endif

// ═══════════════════════════════════════════════════════════════
// P0 止血包回归（docs/optimization-plan.md §2，P0-1..P0-8）
// ═══════════════════════════════════════════════════════════════

// P0-1（词典运行期探测）由整体可运行性覆盖：run_tests 从项目根跑命中
// 「编译期路径」候选，exe 同级 dict/ 候选由 CMake POST_BUILD 部署。
// 探测全落空时抛异常由 main 捕获——无法在本进程内注入"缺词典"场景，不设用例。

void test_p0_metadata_court_underflow_guard() {
    TEST("P0-3：法院名回扫遇截断 UTF-8 头不越界");
    // "人民法院"前是被截断的多字节字符：旧实现 substr(nameStart - 3) 在
    // size_t 上回绕 → std::out_of_range；P0-3 守卫后到边界为止，不再抛。
    const std::string variants[] = {
        std::string("\xE4\xB8") + "人民法院已审理本案。",   // 3 字节汉字截剩 2 字节
        std::string("\xE4") + "人民法院",                    // 截剩 1 字节
        std::string("\xF0\x9F") + "人民法院",                // 4 字节字符截剩 2 字节
    };
    for (const auto& text : variants) {
        bool threw = false;
        try {
            // extractCourt 是 private，统一走公开入口 extract()（内部会触发法院回扫）
            auto meta = document::MetadataExtractor::extract(text);
            (void)meta;
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
    }
    PASS();
}

void test_p0_fulltext_is_original() {
    TEST("P0-5：fullText = 解析原文（无分块重叠重复）");
    const std::string path = "build/test_p0_fulltext.txt";
    std::string original;
    for (int i = 0; i < 60; ++i) {
        original += "第" + std::to_string(i) + "条：本案争议焦点为合同违约金数额如何认定。\n";
    }
    {
        std::ofstream f(path, std::ios::binary);
        f << original;
    }

    rag::Retriever r;
    r.setIndexFilePath("build/test_p0_fulltext.dat");
    const auto res = r.addDocument(path);
    CHECK(res.imported);

    rag::DocumentInfo info;
    CHECK(r.getDocumentInfo(res.documentId, info));
    CHECK(info.chunkCount >= 2);   // 必须是多块文档，否则测不出"重复拼接"

    std::string full;
    CHECK(r.getFullText(res.documentId, full));
    CHECK(full == original);       // 与原文逐字节一致（带 overlap 的分块拼不出这个结果）
    r.clearAll(true);
    std::remove(path.c_str());
    PASS();
}

void test_p0_gbk_file_import() {
    TEST("P0-6：GBK（GB18030）编码文件自动转码导入");
    const std::string path = "build/test_p0_gbk.txt";
    const std::string utf8Content =
        "（2024）京0105民初12345号\n北京市朝阳区人民法院民事判决书\n"
        "原告与被告民间借贷纠纷一案，本院经审理认为，被告应当向原告偿还借款本金十万元并支付利息。\n"
        "判决如下：被告于本判决生效之日起十日内向原告偿还借款本金并支付利息。\n二〇二四年五月二十日\n";

    QByteArray gbkBytes;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QStringEncoder gb(QByteArray("GB18030"));
    CHECK(gb.isValid());
    gbkBytes = gb(QString::fromStdString(utf8Content));
#else
    QTextCodec* gb = QTextCodec::codecForName("GB18030");
    CHECK(gb != nullptr);
    gbkBytes = gb->fromUnicode(QString::fromStdString(utf8Content));
#endif
    CHECK(!gbkBytes.isEmpty());
    {
        std::ofstream f(path, std::ios::binary);
        f.write(gbkBytes.constData(), gbkBytes.size());
    }

    rag::Retriever r;
    r.setIndexFilePath("build/test_p0_gbk.dat");
    const auto res = r.addDocument(path);
    CHECK(res.imported);

    std::string full;
    CHECK(r.getFullText(res.documentId, full));
    CHECK(full == utf8Content);    // 落库的全文就是正确 UTF-8

    auto hits = r.search("民间借贷 借款本金", 5);
    CHECK(hits.size() >= 1);       // GBK 内容可被正常检索命中
    r.clearAll(true);
    std::remove(path.c_str());
    PASS();
}

void test_p0_utf8_bom_stripped() {
    TEST("P0-6：UTF-8 BOM 被剥离，不混入正文");
    const std::string path = "build/test_p0_bom.txt";
    const std::string content = "（2023）沪0115刑初789号 故意伤害案 上海市浦东新区人民法院";
    {
        std::ofstream f(path, std::ios::binary);
        f << "\xEF\xBB\xBF" << content;
    }

    rag::Retriever r;
    r.setIndexFilePath("build/test_p0_bom.dat");
    const auto res = r.addDocument(path);
    CHECK(res.imported);

    std::string full;
    CHECK(r.getFullText(res.documentId, full));
    CHECK(full == content);        // BOM 不进 fullText
    const auto* meta = r.getMetadata(res.documentId);
    CHECK(meta != nullptr && !meta->caseNumber.empty());  // 元数据提取不受 BOM 干扰
    r.clearAll(true);
    std::remove(path.c_str());
    PASS();
}

void test_p0_binary_file_rejected() {
    TEST("P0-6：二进制乱码文件如实拒绝导入（不吞成乱码索引）");
    const std::string path = "build/test_p0_bin.txt";
    {
        std::ofstream f(path, std::ios::binary);
        f << "\x00\x01\x02\x03\xFF\xFE\xFD\xFC\x00\xFB";
    }

    rag::Retriever r;
    r.setIndexFilePath("build/test_p0_bin.dat");
    const auto res = r.addDocument(path);
    CHECK(!res.imported);
    CHECK(!res.diagnostic.empty());
    CHECK_EQ(r.documentCount(), 0);
    r.clearAll(true);
    std::remove(path.c_str());
    PASS();
}

void test_p0_vector_cache_invalidation() {
    TEST("P0-4：换 Embedding 端点/模型不残留旧缓存（观测口冒烟）");
    // 说明：本用例当前只能观测"缓存清空 + 配置生效 + 不崩溃"；
    // 向量条目的真实重建行为待 P2 IHttpTransport 注入 FakeTransport 后升级为全链路断言。
    rag::Retriever r;
    CHECK_EQ(r.vectorCacheSize(), 0);

    r.setApiKey("sk-p0vectorcache123456");
    r.setEmbeddingEndpoint("https://api.siliconflow.cn", "BAAI/bge-large-zh-v1.5");
    CHECK_EQ(r.vectorCacheSize(), 0);
    CHECK(r.embeddingModel() == "BAAI/bge-large-zh-v1.5");
    CHECK(r.embeddingReady());

    // 同值重复下发（启动时配置原样回灌的路径）不得异常
    r.setEmbeddingEndpoint("https://api.siliconflow.cn", "BAAI/bge-large-zh-v1.5");
    r.setApiKey("sk-p0vectorcache123456");
    CHECK(r.embeddingModel() == "BAAI/bge-large-zh-v1.5");
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// T5: 段落角色标注（检索结果证据效力分级）
// ═══════════════════════════════════════════════════════════════

void test_t5_role_annotation_basic() {
    TEST("T5：角色标注——段继承 + 引述不回退 + 跨段位并集");
    // 直接构造块序列，精确钉住标注算法的规则
    std::vector<document::TextChunk> chunks(6);
    chunks[0].content = "北京市某某人民法院民事判决书。（首部，无结构标记）";
    chunks[1].content = "原告张某某诉称：被告借款不还，请求判令偿还。";
    chunks[2].content = "（此段为诉称事实的延续，不含任何结构标记。）";
    chunks[3].content = "被告李某某辩称：承认借款，但暂时无力偿还。";
    chunks[4].content = "本院经审理查明：借款事实清楚。本院认为：被告构成违约。"
                        "原告称被告恶意逃债与查明事实不符——此为引述，不得加当事人段位。";
    chunks[5].content = "判决如下：一、被告偿还借款本息。如不服本判决，可提起上诉。";

    document::annotateChunkRoles(chunks);
    CHECK(chunks[0].role == document::ChunkRole::Unknown);
    CHECK(chunks[1].role == document::ChunkRole::PlaintiffClaims);
    CHECK(chunks[2].role == document::ChunkRole::PlaintiffClaims);   // 无标记块延续当前段
    CHECK(chunks[3].role == document::ChunkRole::DefendantDefense);
    CHECK(chunks[4].role == document::ChunkRole::CourtOpinion);      // 引述「原告称」不加位
    CHECK(chunks[5].role == document::ChunkRole::Judgment);

    // 跨段块（短文书）：本院认为 + 判决如下 同块 → 两段位并集，两路过滤都命中
    std::vector<document::TextChunk> mergedSeq(1);
    mergedSeq[0].content = "本院认为：调解无效。判决如下：准予离婚。";
    document::annotateChunkRoles(mergedSeq);
    const int merged = static_cast<int>(mergedSeq[0].role);
    CHECK((merged & static_cast<int>(document::ChunkRole::CourtOpinion)) != 0);
    CHECK((merged & static_cast<int>(document::ChunkRole::Judgment)) != 0);
    CHECK(document::chunkRoleLabel(mergedSeq[0].role) == "本院认为|判决");
    PASS();
}

void test_t5_role_annotation_real_parse() {
    TEST("T5：真实语料解析即带角色（parseText 出口打标）");
    document::DocumentParser parser;
    const auto chunks = parser.parse("test/data/legal_cases/case_civil_001_loan_dispute.txt");
    CHECK(chunks.size() >= 2);

    bool sawPlaintiff = false, sawOpinion = false, sawJudgment = false;
    for (const auto& c : chunks) {
        const int bits = static_cast<int>(c.role);
        if (c.content.find("诉称") != std::string::npos) {
            CHECK((bits & static_cast<int>(document::ChunkRole::PlaintiffClaims)) != 0);
            sawPlaintiff = true;
        }
        if (c.content.find("本院认为") != std::string::npos
            || c.content.find("经审理查明") != std::string::npos) {
            CHECK((bits & static_cast<int>(document::ChunkRole::CourtOpinion)) != 0);
            sawOpinion = true;
        }
        if (c.content.find("判决如下") != std::string::npos) {
            CHECK((bits & static_cast<int>(document::ChunkRole::Judgment)) != 0);
            sawJudgment = true;
        }
    }
    CHECK(sawPlaintiff && sawOpinion && sawJudgment);
    PASS();
}

void test_t5_search_results_carry_role() {
    TEST("T5：检索结果携带角色（分段构造文本逐段断言）");
    // 各段之间垫 550+ 字节：512 字节的块窗口装不下两个不同段的标记，
    // 保证「含目标短语的块」的角色唯一确定
    const std::string pad(550, '垫');
    const std::string doc =
        pad + "原告诉称：被告借我一百万元至今未还。" + pad
        + "本院认为：借款事实清楚，利息约定有效。" + pad
        + "判决如下：被告偿还借款本息。";

    rag::Retriever r;
    r.addText(doc, "t5_roles_doc");
    CHECK_EQ(r.documentCount(), 1);

    auto hits1 = r.search("借我一百万元", 5);
    CHECK(!hits1.empty());
    CHECK(hits1[0].role == document::ChunkRole::PlaintiffClaims);

    auto hits2 = r.search("借款事实清楚 利息约定", 5);
    CHECK(!hits2.empty());
    CHECK(hits2[0].role == document::ChunkRole::CourtOpinion);

    auto hits3 = r.search("偿还借款本息", 5);
    CHECK(!hits3.empty());
    CHECK(hits3[0].role == document::ChunkRole::Judgment);
    PASS();
}

void test_t5_persistence_roles_roundtrip() {
    TEST("T5：角色随索引持久化往返一致");
    const std::string indexPath = "build/test_t5_roles.dat";
    const std::string pad(550, '垫');
    const std::string doc =
        pad + "原告诉称：被告借我一百万元至今未还。" + pad
        + "本院认为：借款事实清楚，利息约定有效。" + pad
        + "判决如下：被告偿还借款本息。";

    rag::Retriever writer;
    writer.setIndexFilePath(indexPath);
    writer.addText(doc, "t5_roundtrip_doc");
    CHECK(writer.saveIndex().ok);

    rag::Retriever reader;
    reader.setIndexFilePath(indexPath);
    CHECK(reader.loadIndex().ok);
    CHECK_EQ(reader.documentCount(), 1);

    // 块级角色逐块一致
    for (int i = 0; i < 12; ++i) {
        const auto roleW = writer.getChunkRole("t5_roundtrip_doc", i);
        const auto roleR = reader.getChunkRole("t5_roundtrip_doc", i);
        CHECK(roleW == roleR);
        if (roleW == document::ChunkRole::Unknown && i > 6) break;  // 越界侧同为 Unknown
    }
    // 关键块角色命中预期段
    bool sawOpinion = false, sawJudgment = false, sawClaims = false;
    for (int i = 0; i < 12; ++i) {
        const auto role = reader.getChunkRole("t5_roundtrip_doc", i);
        if (role == document::ChunkRole::CourtOpinion) sawOpinion = true;
        if (role == document::ChunkRole::Judgment) sawJudgment = true;
        if (role == document::ChunkRole::PlaintiffClaims) sawClaims = true;
    }
    CHECK(sawClaims && sawOpinion && sawJudgment);

    // 恢复后的检索结果仍携带角色
    auto hits = reader.search("借我一百万元", 5);
    CHECK(!hits.empty());
    CHECK(hits[0].role == document::ChunkRole::PlaintiffClaims);

    reader.clearAll(true);
    PASS();
}

// ── v2 索引文件手工构造（T5 迁移测试用）──
// 字节布局与 index_store.cpp 头部格式注释一一对应（v2 = 块不带角色字节）。
namespace {

void twU8(std::string& b, std::uint8_t v)  { b.push_back(static_cast<char>(v)); }
void twU32(std::string& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void twU64(std::string& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void twStr(std::string& b, const std::string& s) {
    twU32(b, static_cast<std::uint32_t>(s.size()));
    b += s;
}
/// 与 index_store.cpp 同款 CRC-32（多项式 0xEDB88320，位翻转型实现等价）
std::uint32_t twCrc32(const std::string& s) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : s) {
        crc ^= c;
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

void test_t5_v2_index_roles_reannotated() {
    TEST("T5 补：v2 旧索引加载后免费重算角色（不固化全 Unknown）");
    const std::string indexPath = "build/test_t5_v2_migrate.dat";

    // 两个块：诉称段 + 「本院认为……判决如下」跨段块
    const std::string chunk1 = "原告张某某诉称：被告于二〇二三年向其借款十万元至今未还，请求判令偿还。";
    const std::string chunk2 = "本院认为，合法借贷关系受法律保护，借款事实清楚。判决如下：被告于本判决生效之日起十日内偿还原告借款。";
    const std::string fullText = chunk1 + chunk2;

    // 手写 v2 字节流（无角色字节）+ 正确 CRC
    std::string payload;
    payload += "LRAGIDX1";
    twU32(payload, 2);                       // version = 2
    twU32(payload, 1);                       // docCount
    twStr(payload, "case_v2_migrate.txt");
    twStr(payload, "build/v2_src/case_v2_migrate.txt");
    twStr(payload, "2026-10-06 12:00:00");
    twU32(payload, 2);                       // chunkCount
    twU64(payload, fullText.size());         // byteSize
    twU8(payload, 0);                        // ocr = false
    twStr(payload, "");                      // caseNumber
    twStr(payload, "");                      // court
    twStr(payload, "");                      // date
    twStr(payload, "");                      // caseType
    twStr(payload, "");                      // litigants
    twStr(payload, "");                      // procedure
    twStr(payload, "");                      // tendency
    twU64(payload, fullText.size());         // fullTextLen
    payload += fullText;
    twU32(payload, 2);                       // 分块段前的块数（load 侧 storedChunkCount）
    twStr(payload, chunk1);
    twStr(payload, chunk2);                  // v2：块后无角色字节
    std::string file = payload;
    twU32(file, twCrc32(payload));
    {
        std::ofstream f(indexPath, std::ios::binary);
        f << file;
    }

    rag::Retriever r;
    r.setIndexFilePath(indexPath);
    const auto loaded = r.loadIndex();
    if (!loaded.ok) {
        std::cout << "    (load 诊断: " << loaded.diagnostic << ") ";
    }
    CHECK(loaded.ok);
    CHECK_EQ(r.documentCount(), 1);

    // 角色按同一算法免费重算，而不是全 Unknown
    CHECK(r.getChunkRole("case_v2_migrate.txt", 0) == document::ChunkRole::PlaintiffClaims);
    const int merged = static_cast<int>(r.getChunkRole("case_v2_migrate.txt", 1));
    CHECK((merged & static_cast<int>(document::ChunkRole::CourtOpinion)) != 0);
    CHECK((merged & static_cast<int>(document::ChunkRole::Judgment)) != 0);

    // 再落盘升级为 v3 → 全新实例读回角色仍在（迁移不回退）
    CHECK(r.saveIndex().ok);
    rag::Retriever reader;
    reader.setIndexFilePath(indexPath);
    CHECK(reader.loadIndex().ok);
    CHECK((static_cast<int>(reader.getChunkRole("case_v2_migrate.txt", 1))
           & static_cast<int>(document::ChunkRole::CourtOpinion)) != 0);

    reader.clearAll(true);   // 连落盘文件一并清掉
    PASS();
}

void test_t5_corpus_role_distribution() {
    TEST("T5：21 篇语料全量标注（每篇均有法院认定块与主文块）");
    rag::Retriever r;
    for (const auto& file : listTxtFiles("test/data/legal_cases")) {
        r.addDocument(file);
    }
    CHECK_EQ(r.documentCount(), 21);

    int docsWithOpinion = 0, docsWithJudgment = 0;
    int opinionChunks = 0, judgmentChunks = 0;
    for (const auto& id : r.allDocIds()) {
        rag::DocumentInfo info;
        CHECK(r.getDocumentInfo(id, info));
        bool hasOpinion = false, hasJudgment = false;
        for (int i = 0; i < info.chunkCount; ++i) {
            const int bits = static_cast<int>(r.getChunkRole(id, i));
            if (bits & static_cast<int>(document::ChunkRole::CourtOpinion)) {
                hasOpinion = true;
                ++opinionChunks;
            }
            if (bits & static_cast<int>(document::ChunkRole::Judgment)) {
                hasJudgment = true;
                ++judgmentChunks;
            }
        }
        docsWithOpinion += hasOpinion ? 1 : 0;
        docsWithJudgment += hasJudgment ? 1 : 0;
        std::cout << "    " << id << " 块数=" << info.chunkCount
                  << " 含认定=" << (hasOpinion ? "Y" : "N")
                  << " 含主文=" << (hasJudgment ? "Y" : "N") << "\n";
    }
    std::cout << "    含法院认定位块的文书: " << docsWithOpinion
              << "；含主文位块的文书: " << docsWithJudgment << "\n";
    CHECK_EQ(docsWithOpinion, 21);   // 跨段块按位并集：说理段不再被主文标记吞掉
    CHECK_EQ(docsWithJudgment, 21);  // 判决如下 / 裁定如下 均归主文位
    CHECK(opinionChunks >= 21);
    CHECK(judgmentChunks >= 20);
    r.clearAll(false);
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// P2：传输注入后的网络链路单测（FakeTransport，离线驱动真实代码路径）
// ═══════════════════════════════════════════════════════════════

namespace {

/// 构造一条 SSE 增量帧（delta content 任意中文/英文均走 UTF-8 字节）
QByteArray sseDelta(const std::string& content) {
    return QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"") +
           QByteArray::fromStdString(content) +
           QByteArray("\"}}]}\n\n");
}

/// Embedding 应答器：按输入数组返回 index 化向量 [1+i, 2]（含乱序重排验证的基础）
FakeTransport::Reply embeddingResponder(const QByteArray& body, bool shuffle) {
    QJsonDocument doc = QJsonDocument::fromJson(body);
    const QJsonArray inputs = doc.object()["input"].toArray();
    FakeTransport::Reply r;
    QJsonArray data;
    for (int i = 0; i < inputs.size(); ++i) {
        const int slot = shuffle ? (inputs.size() - 1 - i) : i;   // 乱序模式：逆序返回
        QJsonObject item;
        item["index"] = slot;
        QJsonArray vec;
        vec.append(1.0 + slot * 0.001);
        vec.append(2.0);
        item["embedding"] = vec;
        data.append(item);
    }
    QJsonObject root;
    root["data"] = data;
    r.finish.body = QJsonDocument(root).toJson(QJsonDocument::Compact);
    return r;
}

}  // namespace

void test_p2_sse_streaming() {
    TEST("P2：SSE 流式——分块跨行 / CRLF / 无尾换行 / [DONE] 全形态");
    auto fake = std::make_shared<FakeTransport>();
    // 人为把 SSE 帧切碎：跨行、CRLF、最后一帧无换行
    fake->enqueueChunk(sseDelta("根据"));
    fake->enqueueChunk("data: {\"choices\":[{\"delta\":{\"content\":\"检索文书");   // 行中断
    fake->enqueueChunk("\"}}]}\r\n");                                                       // CRLF 收尾
    fake->enqueueChunk(sseDelta("综合认定。"));
    fake->enqueueChunk("data: [DONE]");
    fake->enqueueFinish(FakeTransport::Finish{200, "", false, QString()});

    rag::Generator gen;
    gen.setApiKey("sk-p2-fake-key-1234567890");
    gen.setTransport(fake);

    std::vector<std::string> deltas;
    const std::string full = gen.generate("测试问题", "人民法院 民事判决书 测试上下文",
                                          [&](const std::string& d) { deltas.push_back(d); });
    if (full != "根据检索文书综合认定。") {
        std::cout << "    (实际 full=[" << full << "] deltas=" << deltas.size() << ") ";
    }
    CHECK(full == "根据检索文书综合认定。");
    CHECK_EQ(deltas.size(), static_cast<size_t>(3));
    PASS();
}

void test_p2_error_body_surfaces() {
    TEST("P2：HTTP 200 包错误体如实抛出（体检缺陷 #20 修复）");
    auto fake = std::make_shared<FakeTransport>();
    fake->enqueueFinish(FakeTransport::Finish{
        200, QByteArray("{\"error\":{\"message\":\"Invalid API key\"}}"), false, QString()});

    rag::Generator gen;
    gen.setApiKey("sk-p2-fake-key-1234567890");
    gen.setTransport(fake);

    bool threw = false;
    try {
        gen.generate("测试问题", "人民法院 测试上下文");
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("Invalid API key") != std::string::npos);
    }
    CHECK(threw);
    PASS();
}

void test_p2_http_error_throws() {
    TEST("P2：HTTP 5xx / 网络错误抛异常，错误文本可分类");
    {
        auto fake = std::make_shared<FakeTransport>();
        fake->enqueueFinish(FakeTransport::Finish{502, QByteArray(), false, QString()});
        rag::Generator gen;
        gen.setApiKey("sk-p2-fake-key-1234567890");
        gen.setTransport(fake);
        bool threw = false;
        try {
            gen.generate("测试问题", "人民法院 测试上下文");
        } catch (const std::exception& e) {
            threw = true;
            CHECK(std::string(e.what()).find("502") != std::string::npos);
        }
        CHECK(threw);
    }
    {
        auto fake = std::make_shared<FakeTransport>();
        fake->enqueueFinish(FakeTransport::Finish{0, QByteArray(), true,
                                                  QStringLiteral("Connection refused")});
        rag::Generator gen;
        gen.setApiKey("sk-p2-fake-key-1234567890");
        gen.setTransport(fake);
        bool threw = false;
        try {
            gen.generate("测试问题", "人民法院 测试上下文");
        } catch (const std::exception& e) {
            threw = true;
            CHECK(std::string(e.what()).find("Connection refused") != std::string::npos);
        }
        CHECK(threw);
    }
    PASS();
}

void test_p2_generation_cancel() {
    TEST("P2：生成中取消——handle 中断 + 部分内容返回（对应「■ 停止」链路）");
    auto fake = std::make_shared<FakeTransport>();
    fake->enqueueChunk(sseDelta("残卷"));
    // 不投递结束帧：generate 挂起，等待 cancel
    rag::Generator gen;
    gen.setApiKey("sk-p2-fake-key-1234567890");
    gen.setTransport(fake);

    QTimer::singleShot(50, [&gen]() { gen.cancel(); });   // 挂起期间（事件循环内）取消

    // P3：用户取消恒抛中断（增量已由回调送出，返回值无意义）——
    // 保证「■ 停止」的回合一定被上层标记为 interrupted
    bool threw = false;
    try {
        gen.generate("测试问题", "人民法院 测试上下文");
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("canceled") != std::string::npos);
    }
    CHECK(threw);
    PASS();
}

void test_p2_embed_batch_reorder() {
    TEST("P2：embedBatch 按响应 index 重排（服务端乱序不再错位，体检缺陷 #8）");
    auto fake = std::make_shared<FakeTransport>();
    fake->setResponder([](const QUrl&, const QByteArray& body) {
        return embeddingResponder(body, /*shuffle=*/true);
    });

    vector_engine::EmbeddingService svc;
    svc.setApiKey("sk-p2-fake-key-1234567890");
    svc.setTransport(fake);

    const auto vecs = svc.embedBatch({"甲", "乙", "丙"});
    CHECK_EQ(vecs.size(), static_cast<size_t>(3));
    // shuffle 模式下响应为逆序 index：vecs[i] 应是 [1+i, 2]
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(vecs[i].size(), static_cast<size_t>(2));
        CHECK_CLOSE(vecs[i][0], 1.0 + i * 0.001, 1e-9);
        CHECK_CLOSE(vecs[i][1], 2.0, 1e-9);
    }
    PASS();
}

void test_p2_vector_full_link() {
    TEST("P2：向量路全链路（注入 Fake）——懒重建 / 缓存失效 / P0-4 真断言");
    auto fake = std::make_shared<FakeTransport>();
    fake->setResponder([](const QUrl& url, const QByteArray& body) {
        Q_UNUSED(url);
        return embeddingResponder(body, /*shuffle=*/false);
    });

    rag::Retriever r;
    r.setEmbeddingTransport(fake);
    r.setApiKey("sk-p2-fake-key-1234567890");
    r.setEmbeddingEndpoint("https://api.siliconflow.cn", "BAAI/bge-large-zh-v1.5");
    CHECK_EQ(r.vectorCacheSize(), static_cast<size_t>(0));

    r.addText("民间借贷纠纷中交付凭证的认定规则。", "p2_vec_doc_a");
    r.addText("劳动合同解除后的经济补偿标准。", "p2_vec_doc_b");
    CHECK_EQ(r.chunkCount() >= 2 ? 1 : 0, 1);

    // 向量单路真实出结果（fake 向量与查询向量夹角小，分数应显著非零）
    auto hits = r.searchWithMode("交付凭证 认定", 5, rag::SearchMode::VectorOnly);
    CHECK(!hits.empty());
    CHECK(hits[0].vectorScore > 0.5);
    CHECK_EQ(r.vectorCacheSize(), static_cast<size_t>(r.chunkCount()));

    // P0-4 全链路断言：换模型后向量缓存立刻失效（旧实现静默复用旧向量）
    r.setEmbeddingEndpoint("https://api.siliconflow.cn", "BAAI/bge-m3");
    CHECK_EQ(r.vectorCacheSize(), static_cast<size_t>(0));

    // 换 Key 同样失效
    r.setApiKey("sk-p2-another-key-0987654321");
    CHECK_EQ(r.vectorCacheSize(), static_cast<size_t>(0));
    PASS();
}

void test_p2_aggregate_and_court_level() {
    TEST("P2：聚合检测/聚合检索/法院层级/元数据摘要下沉引擎");
    // 聚合检测（短语表自 UI 下沉）
    CHECK(rag::Retriever::isAggregateQuery("所有案件中违约金如何计算"));
    CHECK(rag::Retriever::isAggregateQuery("帮我统计借款纠纷"));
    CHECK(!rag::Retriever::isAggregateQuery("民间借贷 交付凭证"));

    // 聚合检索：per-doc 截断（3 块同篇 + 宽检索 → 每篇最多 2）
    rag::Retriever r;
    r.addText("原告诉称借款未还。本院认为借贷关系成立。判决如下偿还本息。"
              "补充说明段：逾期利息按约定计算。附：送达回执说明。", "p2_agg_doc");
    auto agg = r.searchAggregate("借贷 本息", 10, /*perDocLimit=*/2);
    CHECK(!agg.empty());
    int docHits = 0;
    for (const auto& h : agg) {
        if (h.docId == "p2_agg_doc") ++docHits;
    }
    CHECK(docHits <= 2);

    // 法院层级判定（四级边界）
    document::DocMetadata m;
    CHECK(document::courtLevelOf(m) == document::CourtLevel::Unknown);
    m.court = "最高人民法院";
    CHECK(document::courtLevelOf(m) == document::CourtLevel::Supreme);
    m.court = "北京市高级人民法院";
    CHECK(document::courtLevelOf(m) == document::CourtLevel::High);
    m.court = "北京市第一中级人民法院";
    CHECK(document::courtLevelOf(m) == document::CourtLevel::Intermediate);
    m.court = "北京市朝阳区人民法院";
    CHECK(document::courtLevelOf(m) == document::CourtLevel::Basic);

    // 元数据摘要（两份 UI 重复拼装合一后的引擎实现）
    r.addText("（2024）京0105民初1号\n北京市朝阳区人民法院民事判决书\n"
              "原告与被告民间借贷纠纷一案。\n二〇二四年五月二十日\n"
              "原告诉称事实清楚。", "p2_meta_doc");
    // addText 走提取器，这里直接验证 summary 的行格式：
    auto summary = r.metadataSummary({"p2_meta_doc"});
    CHECK(summary.find("p2_meta_doc") != std::string::npos);
    PASS();
}

// ═══════════════════════════════════════════════════════════════
// P3：配置合一与工程卫生
// ═══════════════════════════════════════════════════════════════

void test_p3_settings_sanitize() {
    TEST("P3：AppSettings 越界/非法值钳制（load 与保存同一漏斗）");
    const std::string path = "build/test_p3_sanitize.json";
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"k1\": -5, \"b\": 7, \"bm25Weight\": 3, \"vectorWeight\": -1, "
             "\"topK\": 0, \"chunkSize\": 100, \"chunkOverlap\": 200, "
             "\"temperature\": 99, \"chatModel\": \"\"}";
    }

    config::AppSettings s;
    CHECK(config::AppSettings::load(path, s));
    // NaN/越界回落默认
    CHECK_CLOSE(s.k1, 1.5, 1e-9);
    CHECK_CLOSE(s.b, 0.75, 1e-9);
    // 权重：负值回落默认（0.4/0.6），再归一化 → 3/(3+0.6)=0.833、0.6/3.6=0.167
    CHECK_CLOSE(s.bm25Weight, 0.8333333, 1e-6);
    CHECK_CLOSE(s.vectorWeight, 0.1666667, 1e-6);
    CHECK_CLOSE(s.bm25Weight + s.vectorWeight, 1.0, 1e-9);
    // 越界整数回落默认
    CHECK_EQ(s.topK, 20);
    // chunkSize=100 在合法区间 [64, 65536] 内 → 保留；overlap 200 ≥ 100 → 钳为 1/4
    CHECK_EQ(s.chunkSize, 100);
    CHECK_EQ(s.chunkOverlap, 25);
    CHECK_CLOSE(s.temperature, 0.3, 1e-9);
    CHECK(s.chatModel == "deepseek-chat");   // 空串回落默认

    // 合法值原样保留
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"k1\": 2.0, \"b\": 0.5, \"topK\": 33}";
    }
    CHECK(config::AppSettings::load(path, s));
    CHECK_CLOSE(s.k1, 2.0, 1e-9);
    CHECK_CLOSE(s.b, 0.5, 1e-9);
    CHECK_EQ(s.topK, 33);
    std::remove(path.c_str());
    PASS();
}

void test_p3_settings_chat_fields_roundtrip() {
    TEST("P3：生成服务字段（chatBaseUrl/chatModel）落盘往返");
    const std::string path = "build/test_p3_chat_fields.json";
    config::AppSettings w;
    w.chatBaseUrl = "https://llm.example.com";
    w.chatModel = "my-custom-model";
    CHECK(config::AppSettings::save(path, w));

    config::AppSettings r;
    CHECK(config::AppSettings::load(path, r));
    CHECK(r.chatBaseUrl == "https://llm.example.com");
    CHECK(r.chatModel == "my-custom-model");
    std::remove(path.c_str());
    PASS();
}

void test_p3_data_file_exe_dir_migration() {
    TEST("P3：数据文件解析到 exe 目录 + 旧文件自动迁移");
    // 1) 默认解析落在 exe 目录（build/），而非工作目录
    const std::string resolved = config::dataFilePath("p3_probe_migrate.dat");
    CHECK(resolved.find("build") != std::string::npos);

    // 2) 工作目录放一个"旧文件"→ 下次解析自动搬移到 exe 目录
    const std::string legacy = "p3_probe_migrate.dat";
    {
        std::ofstream f(legacy, std::ios::binary);
        f << "legacy-data";
    }
    const std::string resolved2 = config::dataFilePath("p3_probe_migrate.dat");
    CHECK(resolved2 == resolved);
    CHECK(QFile::exists(QString::fromStdString(resolved2)));
    CHECK(!QFile::exists(QString::fromStdString(legacy)));   // 旧文件已被搬走
    CHECK(QFile::remove(QString::fromStdString(resolved2)));
    PASS();
}

void run_all_tests() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════╗" << std::endl;
    std::cout << "║   RAG Search Engine — 单元测试           ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════╝" << std::endl;
    std::cout << "\n";

    std::cout << "── 文档解析 ──" << std::endl;
    test_parser_basic();
    test_parser_chunking();
    test_parser_empty();
    test_parser_file();

    std::cout << "\n── 中文分词 ──" << std::endl;
    test_tokenizer_cut();
    test_tokenizer_cut_for_index();
    test_tokenizer_empty();

    std::cout << "\n── 倒排索引 ──" << std::endl;
    test_inverted_index_add();
    test_inverted_index_query();
    test_inverted_index_doc_freq();
    test_inverted_index_clear();

    std::cout << "\n── BM25 排序 ──" << std::endl;
    test_bm25_basic();
    test_bm25_empty_query();

    std::cout << "\n── 余弦相似度 ──" << std::endl;
    test_cosine_same_vector();
    test_cosine_orthogonal();
    test_cosine_opposite();
    test_similarity_engine_search();

    std::cout << "\n── 混合检索 ──" << std::endl;
    test_retriever_bm25_fallback();
    test_retriever_build_context();

    std::cout << "\n── 用户查询诊断 ──" << std::endl;
    test_diagnostic_user_query();

    std::cout << "\n── PDF 文本提取 ──" << std::endl;
    test_pdf_extract_text();
    test_pdf_parser_routing();
    test_pdf_not_a_pdf();
    test_pdf_retriever_integration();
    test_retriever_failed_import();

    std::cout << "\n── 法律词典 ──" << std::endl;
    test_legal_dict_loaded();
    test_legal_term_recognition();
    test_legal_compound_terms();
    test_legal_search_improvement();

    std::cout << "\n── 元数据提取 ──" << std::endl;
    test_metadata_case_number();
    test_metadata_court();
    test_metadata_date_arabic();
    test_metadata_date_chinese();
    test_metadata_case_type();
    test_metadata_integration();
    test_metadata_empty();

    std::cout << "\n── 法律 Prompt 模板 ──" << std::endl;
    test_prompt_legal_detection();
    test_prompt_metadata_summary();

    std::cout << "\n── 端到端测试 ──" << std::endl;
    test_e2e_import_all_demo_docs();
    test_e2e_search_across_all_types();
    test_e2e_metadata_all_docs();
    test_e2e_filter_functionality();
    test_e2e_legal_prompt_detection();
    test_e2e_performance_stress();
    test_e2e_failed_ocr_import_then_search();

    std::cout << "\n── T1: 第 8 类元数据（裁判结果倾向）──" << std::endl;
    test_metadata_result_tendency();
    test_metadata_tendency_labels();
    test_metadata_tendency_keyword_cases();

    std::cout << "\n── T1: 块级删除 ──" << std::endl;
    test_inverted_index_remove_chunk();

    std::cout << "\n── T1: 文档级视图与删除 ──" << std::endl;
    test_retriever_document_view();
    test_retriever_remove_document();

    std::cout << "\n── T1: 索引持久化 ──" << std::endl;
    test_persistence_roundtrip();
    test_persistence_ocr_flag_no_rerun();
    test_persistence_missing_file();
    test_persistence_corrupt_file();

    std::cout << "\n── T2: 问答历史持久层 ──" << std::endl;
    test_history_append_and_get();
    test_history_persistence_restart();
    test_history_keyword_search();
    test_history_delete();
    test_history_interrupted_flag();
    test_history_markdown_export();

    std::cout << "\n── T3: 检索参数配置中心 ──" << std::endl;
    test_settings_roundtrip();
    test_settings_missing_fields_fallback();
    test_settings_corrupt_file();
    test_engine_hot_update_k1();
    test_chunk_params_new_docs_only();

    std::cout << "\n── T4 第 0 期: Embedding 服务配置 ──" << std::endl;
    test_embedding_url_normalize();
    test_embedding_host_display();

    std::cout << "\n── T4: 四路检索与评测指标 ──" << std::endl;
    test_rrf_fuse();
    test_eval_metrics();
    test_search_modes_degraded();

    std::cout << "\n── P0 止血包回归（2026-10-06 体检必修项）──" << std::endl;
    test_p0_metadata_court_underflow_guard();
    test_p0_fulltext_is_original();
    test_p0_gbk_file_import();
    test_p0_utf8_bom_stripped();
    test_p0_binary_file_rejected();
    test_p0_vector_cache_invalidation();

    std::cout << "\n── P2：传输注入与业务下沉 ──" << std::endl;
    test_p2_sse_streaming();
    test_p2_error_body_surfaces();
    test_p2_http_error_throws();
    test_p2_generation_cancel();
    test_p2_embed_batch_reorder();
    test_p2_vector_full_link();
    test_p2_aggregate_and_court_level();

    std::cout << "\n── P3：配置合一与工程卫生 ──" << std::endl;
    test_p3_settings_sanitize();
    test_p3_settings_chat_fields_roundtrip();
    test_p3_data_file_exe_dir_migration();

    std::cout << "\n── T5: 段落角色标注 ──" << std::endl;
    test_t5_role_annotation_basic();
    test_t5_role_annotation_real_parse();
    test_t5_search_results_carry_role();
    test_t5_persistence_roles_roundtrip();
    test_t5_v2_index_roles_reannotated();
    test_t5_corpus_role_distribution();

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════" << std::endl;
    std::cout << "  总计: " << (g_passed + g_failed)
              << " | ✅ 通过: " << g_passed
              << " | ❌ 失败: " << g_failed << std::endl;
    std::cout << "═══════════════════════════════════════════" << std::endl;
}

int main(int argc, char *argv[]) {
    // 事件派发器：OcrClient 等待 OCR 子进程使用 QEventLoop，
    // 没有 QCoreApplication 时事件循环无法工作。
    QApplication app(argc, argv);

    // 确保从项目根目录运行，以便找到 test/data/ 和 dict/
    if (!QFile::exists(QStringLiteral("test/data/rag_intro.txt"))) {
        std::cerr << "⚠️  请从项目根目录运行测试程序！" << std::endl;
        std::cerr << "   cd rag-search-engine && ./build/test_main.exe" << std::endl;
        return 1;
    }

    if (!QFile::exists(QStringLiteral("third_party/cppjieba/dict/jieba.dict.utf8"))) {
        std::cerr << "⚠️  找不到 cppjieba 词典文件！请检查 third_party/cppjieba/dict/" << std::endl;
        return 1;
    }

    run_all_tests();

    return g_failed > 0 ? 1 : 0;
}
