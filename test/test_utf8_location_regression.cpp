#include <iostream>
#include <string>
#include <vector>

#include "document/parser.h"
#include "document/tokenizer.h"
#include "rag/retriever.h"

namespace {

bool isValidUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        size_t width = 0;
        if ((first & 0x80) == 0) {
            width = 1;
        } else if ((first & 0xE0) == 0xC0) {
            width = 2;
        } else if ((first & 0xF0) == 0xE0) {
            width = 3;
        } else if ((first & 0xF8) == 0xF0) {
            width = 4;
        } else {
            return false;
        }

        if (i + width > text.size()) return false;
        for (size_t j = 1; j < width; ++j) {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += width;
    }
    return true;
}

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

bool testUtf8Chunking() {
    std::string text;
    for (int i = 0; i < 220; ++i) {
        text += "这是用于验证中文 UTF-8 分块边界安全性的法律文书内容。";
    }

    document::DocumentParser parser;
    const auto chunks = parser.parseText(text, "utf8-boundary.txt");
    if (!check(chunks.size() > 1, "expected multiple chunks")) return false;

    document::Tokenizer tokenizer;
    for (const auto& chunk : chunks) {
        if (!check(isValidUtf8(chunk.content),
                   "chunk " + std::to_string(chunk.chunkIndex) + " is not valid UTF-8")) {
            return false;
        }
        if (!check(!tokenizer.cutForIndex(chunk.content).empty(),
                   "chunk " + std::to_string(chunk.chunkIndex) + " produced no tokens")) {
            return false;
        }
    }

    return true;
}

bool testLegalLocationRetrieval() {
    rag::Retriever retriever;
    retriever.addText(
        "被告：上饶市宸兴置业有限公司，住所地：江西省上饶市信州区广信大道36号3幢1-613。"
        "案涉商品房坐落于上饶市信州区德兴路16号地块。",
        "location-case.txt");

    const auto results = retriever.search("这个案子发生在哪里", 5);
    if (!check(!results.empty(), "location question returned no results")) return false;

    for (const auto& result : results) {
        if (result.content.find("住所地") != std::string::npos ||
            result.content.find("坐落于") != std::string::npos) {
            return check(result.bm25Score > 0.0, "location result has no BM25 score");
        }
    }

    return check(false, "location passage was not returned");
}

} // namespace

int main() {
    if (!testUtf8Chunking() || !testLegalLocationRetrieval()) {
        return 1;
    }

    std::cout << "UTF-8 chunking and legal location retrieval regression passed."
              << std::endl;
    return 0;
}
