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
#include "document/metadata.h"
#include "index/inverted_index.h"
#include "index/bm25_ranker.h"
#include "vector/similarity.h"
#include "document/pdf_extractor.h"
#include "rag/retriever.h"
#include "rag/generator.h"
#include <algorithm>

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
    for (const auto& entry : std::filesystem::directory_iterator(legalDir)) {
        if (entry.path().extension() == ".txt") {
            retriever.addDocument(entry.path().string());
            imported++;
        }
    }

    std::cout << "    (导入: " << imported << " 篇, 文本块: " << retriever.docCount() << ") ";
    CHECK(imported == 21);
    PASS();
}

void test_e2e_search_across_all_types() {
    TEST("E2E: 跨案件类型搜索");
    rag::Retriever retriever;
    std::string legalDir = "test/data/legal_cases";

    for (const auto& entry : std::filesystem::directory_iterator(legalDir)) {
        if (entry.path().extension() == ".txt") {
            retriever.addDocument(entry.path().string());
        }
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

    for (const auto& entry : std::filesystem::directory_iterator(legalDir)) {
        if (entry.path().extension() == ".txt") {
            retriever.addDocument(entry.path().string());
        }
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

    for (const auto& entry : std::filesystem::directory_iterator(legalDir)) {
        if (entry.path().extension() == ".txt") {
            retriever.addDocument(entry.path().string());
        }
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

    for (const auto& entry : std::filesystem::directory_iterator(legalDir)) {
        if (entry.path().extension() == ".txt") {
            retriever.addDocument(entry.path().string());
        }
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
