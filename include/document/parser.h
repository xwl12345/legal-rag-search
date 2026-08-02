#pragma once
#include <string>
#include <vector>
#include <string_view>

namespace document {

/// 文本块：文档解析后的基本检索单元
struct TextChunk {
    std::string docId;       // 来源文档 ID（文件名）
    int chunkIndex = 0;      // 在文档中的块序号
    std::string content;     // 文本内容
    int startPos = 0;        // 在原文档中的起始位置（字符偏移）
};

/// 文档解析器：支持 .txt / .md 文件
class DocumentParser {
public:
    /// 读取文件并切分成文本块
    std::vector<TextChunk> parse(const std::string& filePath);

    /// 从纯文本字符串解析
    std::vector<TextChunk> parseText(std::string_view text, const std::string& docId);

private:
    /// 将长文本按 maxChunkSize 切分（带 overlap）
    static std::vector<std::string> splitChunks(std::string_view text,
                                                 int maxSize = 512,
                                                 int overlap = 50);
};

} // namespace document
