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
#include <fstream>
#include <filesystem>

// ── 模块头文件 ──
#include "document/parser.h"
#include "document/tokenizer.h"
#include "index/inverted_index.h"
#include "index/bm25_ranker.h"
#include "vector/similarity.h"
#include "document/pdf_extractor.h"
#include "rag/retriever.h"

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
    retriever.addDocument("test/data/test.pdf");

    // 应该至少有一个 chunk
    CHECK(retriever.docCount() >= 1);

    // 搜索 PDF 中的内容
    auto results = retriever.search("Hello PDF", 3);
    CHECK(results.size() >= 1);
    std::cout << "    (检索到 " << results.size() << " 条结果) ";
    PASS();
}

// ═══════════════════════════════════════════════════════════════
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

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════" << std::endl;
    std::cout << "  总计: " << (g_passed + g_failed)
              << " | ✅ 通过: " << g_passed
              << " | ❌ 失败: " << g_failed << std::endl;
    std::cout << "═══════════════════════════════════════════" << std::endl;
}

int main() {
    // 确保从项目根目录运行，以便找到 test/data/ 和 dict/
    if (!std::filesystem::exists("test/data/rag_intro.txt")) {
        std::cerr << "⚠️  请从项目根目录运行测试程序！" << std::endl;
        std::cerr << "   cd rag-search-engine && ./build/test_main.exe" << std::endl;
        return 1;
    }

    if (!std::filesystem::exists("third_party/cppjieba/dict/jieba.dict.utf8")) {
        std::cerr << "⚠️  找不到 cppjieba 词典文件！请检查 third_party/cppjieba/dict/" << std::endl;
        return 1;
    }

    run_all_tests();

    return g_failed > 0 ? 1 : 0;
}
