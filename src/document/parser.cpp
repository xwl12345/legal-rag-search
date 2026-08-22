#include "document/parser.h"
#include "document/pdf_extractor.h"
#include "document/ocr_client.h"
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <sstream>
#include <algorithm>

namespace {

bool isUtf8ContinuationByte(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

size_t previousUtf8Boundary(std::string_view text, size_t position) {
    position = std::min(position, text.size());
    while (position > 0 && position < text.size() &&
           isUtf8ContinuationByte(static_cast<unsigned char>(text[position]))) {
        --position;
    }
    return position;
}

} // namespace

namespace document {

ParseResult DocumentParser::parseWithResult(const std::string& filePath) {
    // 使用 QFileInfo 而非 std::filesystem::path
    // — MinGW 的 std::filesystem::path 不能正确处理 UTF-8 中文路径
    QFileInfo fileInfo(QString::fromStdString(filePath));
    std::string docId = fileInfo.fileName().toStdString();
    std::string ext = fileInfo.suffix().toStdString();

    // 转为小写用于比较
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string content;
    ParseSource source = ParseSource::None;

    if (ext == "pdf") {
        if (!fileInfo.exists()) {
            return {ParseStatus::FileOpenFailed, source, {}, "找不到 PDF 文件"};
        }

        // PDF 文件：先尝试直接提取文本层
        content = PdfExtractor::extractText(filePath);
        if (!QString::fromStdString(content).trimmed().isEmpty()) {
            source = ParseSource::NativePdf;
        } else {
            // 文本层为空 → 可能是扫描件，回退到 OCR
            const auto ocrResult = OcrClient::extractText(filePath);
            if (!ocrResult.isSuccess()) {
                return {ParseStatus::OcrFailed, source, {}, ocrResult.diagnostic};
            }
            content = ocrResult.text;
            source = ParseSource::Ocr;
        }
    } else {
        // 普通文本文件（使用 QFile 支持 Unicode 路径）
        QFile file(QString::fromStdString(filePath));
        if (!file.open(QIODevice::ReadOnly)) {
            return {ParseStatus::FileOpenFailed, source, {}, "无法打开文件：" + docId};
        }
        content = file.readAll().toStdString();
        source = ParseSource::TextFile;
    }

    if (QString::fromStdString(content).trimmed().isEmpty()) {
        return {ParseStatus::NoTextExtracted, source, {}, "文件不包含可检索文本"};
    }

    auto chunks = parseText(content, docId);
    if (chunks.empty()) {
        return {ParseStatus::NoTextExtracted, source, {}, "文件不包含可检索文本"};
    }

    return {ParseStatus::Success, source, std::move(chunks), ""};
}

std::vector<TextChunk> DocumentParser::parse(const std::string& filePath) {
    return parseWithResult(filePath).chunks;
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
        const size_t requestedEnd = std::min(
            pos + static_cast<size_t>(std::max(maxSize, 1)), text.size());
        size_t end = previousUtf8Boundary(text, requestedEnd);
        if (end <= pos) {
            // 极小的字节预算也不能截断一个 UTF-8 字符。
            end = requestedEnd;
            while (end < text.size() &&
                   isUtf8ContinuationByte(static_cast<unsigned char>(text[end]))) {
                ++end;
            }
        }

        // 尝试在句号、换行等自然断点处切割
        if (end < text.size()) {
            const size_t minBreak = pos + static_cast<size_t>(std::max(maxSize, 1)) / 2;
            // 回退到最近的自然断点；只检查当前块内的字符。
            for (size_t j = end; j > minBreak; --j) {
                const char c = text[j - 1];
                // Check for natural break points (sentence endings, newlines)
                if (c == '\n' || c == '\r' ||
                    c == '.'  || c == '!'  || c == '?') {
                    end = j;
                    break;
                }
            }
        }

        result.emplace_back(text.substr(pos, end - pos));

        // 下一个块的起始位置（考虑 overlap）
        size_t nextPos = end;
        if (overlap > 0 && end < text.size()) {
            const size_t overlapSize = static_cast<size_t>(overlap);
            if (end > overlapSize) {
                nextPos = previousUtf8Boundary(text, end - overlapSize);
            }
        }
        // 防止因 overlap 或边界调整而停滞。
        pos = nextPos > pos ? nextPos : end;
    }

    return result;
}

} // namespace document
