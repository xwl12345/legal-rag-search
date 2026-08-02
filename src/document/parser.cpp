#include "document/parser.h"
#include "document/pdf_extractor.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace document {

std::vector<TextChunk> DocumentParser::parse(const std::string& filePath) {
    // 提取文件名作为 docId
    fs::path path(filePath);
    std::string docId = path.filename().string();
    std::string ext = path.extension().string();

    // 转为小写用于比较
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string content;

    if (ext == ".pdf") {
        // PDF 文件：使用 PdfExtractor 提取文本
        content = PdfExtractor::extractText(filePath);
        if (content.empty()) {
            return {};  // PDF 解析失败或为扫描版（无文本层）
        }
    } else {
        // 普通文本文件
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            return {};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        content = buffer.str();
    }

    return parseText(content, docId);
}

std::vector<TextChunk> DocumentParser::parseText(std::string_view text,
                                                  const std::string& docId) {
    std::vector<TextChunk> chunks;
    auto rawChunks = splitChunks(text);

    int startPos = 0;
    for (size_t i = 0; i < rawChunks.size(); ++i) {
        TextChunk chunk;
        chunk.docId = docId;
        chunk.chunkIndex = static_cast<int>(i);
        chunk.content = rawChunks[i];
        chunk.startPos = startPos;

        chunks.push_back(std::move(chunk));

        // 计算下一个块的起始位置（考虑 overlap）
        startPos += static_cast<int>(rawChunks[i].size());
        if (i + 1 < rawChunks.size()) {
            // overlap 区域不计入 startPos 跳跃
        }
    }

    return chunks;
}

std::vector<std::string> DocumentParser::splitChunks(std::string_view text,
                                                      int maxSize,
                                                      int overlap) {
    std::vector<std::string> result;
    if (text.empty()) return result;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = std::min(pos + maxSize, text.size());

        // 尝试在句号、换行等自然断点处切割
        if (end < text.size()) {
            // 回退到最近的自然断点
            size_t breakPoint = end;
            for (size_t j = end; j > pos + maxSize / 2; --j) {
                char c = text[j];
                // Check for natural break points (sentence endings, newlines)
                if (c == '\n' || c == '\r' ||
                    c == '.'  || c == '!'  || c == '?') {
                    breakPoint = j + 1;
                    break;
                }
            }
            end = breakPoint;
        }

        result.emplace_back(text.substr(pos, end - pos));

        // 下一个块的起始位置（考虑 overlap）
        size_t nextPos = end;
        if (overlap > 0 && end < text.size()) {
            nextPos = (pos + maxSize > overlap) ? (pos + maxSize - overlap) : end;
        }
        pos = nextPos;
    }

    return result;
}

} // namespace document
