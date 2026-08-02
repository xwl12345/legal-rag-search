#include "document/pdf_extractor.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <zlib.h>

namespace document {

// ── 文件读取 ──

std::vector<uint8_t> PdfExtractor::readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

// ── zlib FlateDecode 解压 ──

std::vector<uint8_t> PdfExtractor::inflateData(const std::string& data) {
    if (data.empty()) return {};

    z_stream strm = {};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    strm.avail_in = static_cast<uInt>(data.size());

    // 初始化 inflate
    if (inflateInit(&strm) != Z_OK) return {};

    std::vector<uint8_t> out;
    const size_t CHUNK = 16384;
    std::vector<uint8_t> buffer(CHUNK);

    int ret;
    do {
        strm.next_out = buffer.data();
        strm.avail_out = static_cast<uInt>(CHUNK);
        ret = inflate(&strm, Z_NO_FLUSH);

        size_t have = CHUNK - strm.avail_out;
        out.insert(out.end(), buffer.begin(), buffer.begin() + have);
    } while (ret == Z_OK);

    inflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        // 解压失败（可能是非压缩数据），返回原始数据
        return std::vector<uint8_t>(data.begin(), data.end());
    }

    return out;
}

// ── ASCII85Decode 解码 ──

std::vector<uint8_t> PdfExtractor::decodeAscii85(const std::string& data) {
    std::vector<uint8_t> out;
    out.reserve(data.size() * 4 / 5);

    size_t i = 0;
    uint32_t group = 0;
    int groupLen = 0;

    while (i < data.size()) {
        char c = data[i];

        // 跳过空白
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            ++i;
            continue;
        }

        // ~> 结束标记
        if (c == '~') {
            // 处理剩余字节
            if (groupLen > 0) {
                for (int pad = groupLen; pad < 4; ++pad) {
                    group = group * 85 + 84;  // 填充最大值
                }
                // 输出 groupLen-1 个字节
                for (int j = 3; j > 4 - groupLen; --j) {
                    out.push_back(static_cast<uint8_t>((group >> (8 * j)) & 0xFF));
                }
            }
            break;
        }

        if (c < '!' || c > 'u') {
            ++i;
            continue;  // 跳过非法字符
        }

        group = group * 85 + static_cast<uint32_t>(c - '!');
        ++groupLen;

        if (groupLen == 5) {
            out.push_back(static_cast<uint8_t>((group >> 24) & 0xFF));
            out.push_back(static_cast<uint8_t>((group >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((group >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(group & 0xFF));
            group = 0;
            groupLen = 0;
        }

        ++i;
    }

    return out;
}

// ── PDF 字符串解码 ──

std::string PdfExtractor::decodePdfString(const std::string& raw, size_t& pos) {
    // 处理两种 PDF 字符串格式：
    // 1. 字面量字符串：(text)  — 以括号包围
    // 2. 十六进制字符串：<hex>  — 以尖括号包围
    if (pos >= raw.size()) return "";

    if (raw[pos] == '(') {
        // 字面量字符串：需要处理括号配对和转义
        std::string result;
        int depth = 1;
        ++pos;  // 跳过开括号

        while (pos < raw.size() && depth > 0) {
            char c = raw[pos];

            if (c == '\\' && pos + 1 < raw.size()) {
                ++pos;
                char next = raw[pos];
                switch (next) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case '(': case ')': case '\\':
                        result += next; break;
                    default:
                        // 八进制转义 \ddd
                        if (next >= '0' && next <= '7') {
                            int octal = next - '0';
                            if (pos + 1 < raw.size() && raw[pos + 1] >= '0' && raw[pos + 1] <= '7') {
                                ++pos;
                                octal = octal * 8 + (raw[pos] - '0');
                                if (pos + 1 < raw.size() && raw[pos + 1] >= '0' && raw[pos + 1] <= '7') {
                                    ++pos;
                                    octal = octal * 8 + (raw[pos] - '0');
                                }
                            }
                            result += static_cast<char>(octal);
                        } else {
                            result += next;  // 未知转义，保留原字符
                        }
                        break;
                }
            } else if (c == '(') {
                ++depth;
                result += c;
            } else if (c == ')') {
                --depth;
                if (depth > 0) result += c;
            } else {
                result += c;
            }
            ++pos;
        }

        return pdfTextToUtf8(result);
    }

    if (raw[pos] == '<') {
        // 十六进制字符串
        std::string hex;
        ++pos;  // 跳过 <
        while (pos < raw.size() && raw[pos] != '>') {
            if (std::isxdigit(static_cast<unsigned char>(raw[pos]))) {
                hex += raw[pos];
            }
            ++pos;
        }
        if (pos < raw.size()) ++pos;  // 跳过 >

        // 将十六进制转为字节序列
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };

        std::string bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            int b = (hexVal(hex[i]) << 4) | hexVal(hex[i + 1]);
            bytes += static_cast<char>(b);
        }
        if (hex.size() % 2 != 0) {
            // 奇数个十六进制字符，最后补 0
            bytes += static_cast<char>(hexVal(hex.back()) << 4);
        }

        return pdfTextToUtf8(bytes);
    }

    return "";
}

// ── PDF 编码文本 → UTF-8 ──

std::string PdfExtractor::pdfTextToUtf8(const std::string& text) {
    // PDF 文本通常使用 PDFDocEncoding 或 WinAnsiEncoding
    // 对于 ASCII 文本，直接返回；对于中文 PDF，通常使用 UTF-16BE BOM
    if (text.empty()) return text;

    // 检测 UTF-16BE BOM (\xFE\xFF)
    if (text.size() >= 2 &&
        static_cast<uint8_t>(text[0]) == 0xFE &&
        static_cast<uint8_t>(text[1]) == 0xFF) {
        // UTF-16BE → UTF-8
        std::string utf8;
        for (size_t i = 2; i + 1 < text.size(); i += 2) {
            uint16_t ch = (static_cast<uint8_t>(text[i]) << 8) |
                          static_cast<uint8_t>(text[i + 1]);
            if (ch < 0x80) {
                utf8 += static_cast<char>(ch);
            } else if (ch < 0x800) {
                utf8 += static_cast<char>(0xC0 | (ch >> 6));
                utf8 += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                utf8 += static_cast<char>(0xE0 | (ch >> 12));
                utf8 += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        return utf8;
    }

    // 纯 ASCII / PDFDocEncoding — 直接返回（PDFDocEncoding 与 ASCII 兼容）
    return text;
}

// ── 内容流文本提取 ──

std::string PdfExtractor::extractTextFromStream(const std::vector<uint8_t>& data) {
    std::string content(data.begin(), data.end());
    std::string result;

    size_t pos = 0;
    bool inTextBlock = false;

    while (pos < content.size()) {
        if (!inTextBlock) {
            // 寻找文本块开始标记 BT
            size_t bt = content.find("BT", pos);
            if (bt == std::string::npos) break;

            // 确认 BT 是独立的关键字（前后是空白或运算符边界）
            bool validBT = true;
            if (bt + 2 < content.size() && std::isalpha(static_cast<unsigned char>(content[bt + 2]))) {
                validBT = false;  // BT 后跟字母 → 不是独立关键字
            }
            if (validBT) {
                inTextBlock = true;
                pos = bt + 2;
            } else {
                pos = bt + 1;
                continue;
            }
        }

        if (inTextBlock) {
            // 寻找文本块结束标记 ET
            size_t et = content.find("ET", pos);
            if (et == std::string::npos) break;

            bool validET = true;
            if (et + 2 < content.size() && std::isalpha(static_cast<unsigned char>(content[et + 2]))) {
                validET = false;
            }
            if (!validET) {
                pos = et + 1;
                continue;
            }

            // 解析 pos 到 et 之间的文本操作符
            std::string block = content.substr(pos, et - pos);
            std::string blockText;
            size_t bp = 0;

            while (bp < block.size()) {
                // 跳过空白
                while (bp < block.size() && (block[bp] == ' ' || block[bp] == '\n' ||
                       block[bp] == '\r' || block[bp] == '\t')) {
                    ++bp;
                }

                if (bp >= block.size()) break;

                char c = block[bp];

                if (c == '(') {
                    // 字面量字符串 → 可能是 Tj / ' / " 的参数
                    std::string str = decodePdfString(block, bp);

                    // 查看字符串后的操作符
                    size_t opStart = bp;
                    while (opStart < block.size() &&
                           (block[opStart] == ' ' || block[opStart] == '\n' ||
                            block[opStart] == '\r' || block[opStart] == '\t')) {
                        ++opStart;
                    }
                    // 找到操作符结束
                    size_t opEnd = opStart;
                    while (opEnd < block.size() && std::isalpha(
                               static_cast<unsigned char>(block[opEnd]))) {
                        ++opEnd;
                    }
                    std::string op = block.substr(opStart, opEnd - opStart);

                    // Tj / ' / " → 添加文本
                    if (op == "Tj" || op == "'" || op == "\"") {
                        if (!blockText.empty() && !str.empty()) {
                            // 判断是否需要添加空格
                            char last = blockText.back();
                            if (last != ' ' && last != '\n' && last != '-') {
                                blockText += ' ';
                            }
                        }
                        blockText += str;
                    }
                } else if (c == '[') {
                    // TJ 操作符：数组参数 [(text1) num (text2) ...]
                    ++bp;  // 跳过 [
                    std::string tjText;

                    while (bp < block.size() && block[bp] != ']') {
                        // 跳过空白
                        while (bp < block.size() && (block[bp] == ' ' || block[bp] == '\n' ||
                               block[bp] == '\r' || block[bp] == '\t')) {
                            ++bp;
                        }
                        if (bp >= block.size() || block[bp] == ']') break;

                        if (block[bp] == '(') {
                            std::string str = decodePdfString(block, bp);
                            if (!tjText.empty() && !str.empty()) {
                                tjText += ' ';
                            }
                            tjText += str;
                        } else if (block[bp] == '<') {
                            // 十六进制字符串
                            std::string str = decodePdfString(block, bp);
                            if (!tjText.empty() && !str.empty()) {
                                tjText += ' ';
                            }
                            tjText += str;
                        } else {
                            // 数字（kerning），跳过
                            ++bp;
                        }
                    }
                    if (bp < block.size()) ++bp;  // 跳过 ]

                    // 查找后续的 TJ 操作符
                    size_t tjOpStart = bp;
                    while (tjOpStart < block.size() &&
                           (block[tjOpStart] == ' ' || block[tjOpStart] == '\n' ||
                            block[tjOpStart] == '\r' || block[tjOpStart] == '\t')) {
                        ++tjOpStart;
                    }
                    if (tjOpStart + 2 <= block.size() &&
                        block.substr(tjOpStart, 2) == "TJ") {
                        if (!blockText.empty() && !tjText.empty() &&
                            blockText.back() != ' ' && blockText.back() != '\n') {
                            blockText += ' ';
                        }
                        blockText += tjText;
                    }
                } else if (c == 'T') {
                    // Td / TD / T* / Tm → 换行
                    if (bp + 1 < block.size()) {
                        if (block[bp + 1] == 'd' || block[bp + 1] == 'D' ||
                            block[bp + 1] == '*' || block[bp + 1] == 'm') {
                            // 确认是独立的操作符
                            size_t after = bp + 2;
                            if (after >= block.size() || !std::isalpha(
                                    static_cast<unsigned char>(block[after]))) {
                                if (!blockText.empty() && blockText.back() != '\n') {
                                    blockText += '\n';
                                }
                            }
                        }
                    }
                    ++bp;
                } else {
                    ++bp;
                }
            }  // end while bp < block.size()

            if (!blockText.empty()) {
                if (!result.empty() && result.back() != '\n') {
                    result += '\n';
                }
                result += blockText;
            }

            pos = et + 2;
            inTextBlock = false;
        }
    }  // end while pos < content.size()

    return result;
}

// ── 主入口 ──

std::string PdfExtractor::extractText(const std::string& filePath) {
    auto raw = readFile(filePath);
    if (raw.empty()) return "";

    // 检查 PDF 头
    std::string header(raw.begin(), raw.begin() + std::min<size_t>(raw.size(), 8));
    if (header.find("%PDF-") != 0) {
        return "";  // 不是 PDF 文件
    }

    std::string content(raw.begin(), raw.end());
    std::vector<std::string> allTexts;

    // ── 遍历所有流对象 ──
    size_t pos = 0;
    while (pos < content.size()) {
        // 查找 "stream" 关键字
        size_t streamKey = content.find("stream", pos);
        if (streamKey == std::string::npos) break;

        // 确认是独立的 "stream" 关键字（前后是换行或空白）
        bool validStream = false;
        // 检查前面：要么是行首，要么前面是换行
        if (streamKey == 0 || content[streamKey - 1] == '\n' ||
            content[streamKey - 1] == '\r') {
            // 检查后面：必须是 \r 或 \n
            size_t afterKey = streamKey + 6;  // strlen("stream")
            if (afterKey < content.size()) {
                if (content[afterKey] == '\r') ++afterKey;
                if (afterKey < content.size() && content[afterKey] == '\n') {
                    validStream = true;
                }
            }
        }

        if (!validStream) {
            pos = streamKey + 6;
            continue;
        }

        // 找到 stream 数据起始位置（跳过 stream\r\n 或 stream\n）
        size_t dataStart = streamKey + 6;  // strlen("stream")
        if (dataStart < content.size() && content[dataStart] == '\r') ++dataStart;
        if (dataStart < content.size() && content[dataStart] == '\n') ++dataStart;

        // 查找 endstream
        size_t endStream = content.find("endstream", dataStart);
        if (endStream == std::string::npos) {
            pos = dataStart;
            break;
        }

        // 提取原始流数据
        // 注意：endstream 前可能有 \r\n，需要回退
        size_t dataEnd = endStream;
        while (dataEnd > dataStart &&
               (content[dataEnd - 1] == '\n' || content[dataEnd - 1] == '\r' ||
                content[dataEnd - 1] == ' ')) {
            --dataEnd;
        }
        std::string rawStream = content.substr(dataStart, dataEnd - dataStart);

        // ── 查找该流对应的字典（stream 前的内容）──
        // 回退查找 << ... >>
        std::string dictArea;
        {
            size_t dictSearchEnd = streamKey;
            size_t dictSearchStart = (dictSearchEnd > 2048) ? dictSearchEnd - 2048 : 0;
            std::string beforeStream = content.substr(dictSearchStart, dictSearchEnd - dictSearchStart);

            // 找最后一个 << 和对应的 >>
            size_t dictOpen = beforeStream.rfind("<<");
            if (dictOpen != std::string::npos) {
                // 简单匹配：查找随后的 >>
                size_t dictClose = beforeStream.find(">>", dictOpen);
                if (dictClose != std::string::npos) {
                    dictArea = beforeStream.substr(dictOpen, dictClose - dictOpen + 2);
                }
            }
        }

        // ── 确定过滤器 ──
        std::vector<uint8_t> decompressed;

        bool hasFlateDecode = (dictArea.find("FlateDecode") != std::string::npos);
        bool hasASCII85 = (dictArea.find("ASCII85Decode") != std::string::npos);

        if (hasASCII85 && hasFlateDecode) {
            // ASCII85 → FlateDecode（顺序：先 ASCII85 后 FlateDecode）
            auto a85 = decodeAscii85(rawStream);
            std::string a85Str(a85.begin(), a85.end());
            decompressed = inflateData(a85Str);
        } else if (hasFlateDecode) {
            decompressed = inflateData(rawStream);
        } else if (hasASCII85) {
            decompressed = decodeAscii85(rawStream);
        } else {
            // 无过滤器，直接使用原始数据
            decompressed.assign(rawStream.begin(), rawStream.end());
        }

        // ── 判断是否为文本内容流 ──
        std::string streamStr(decompressed.begin(), decompressed.end());
        if (streamStr.find("BT") != std::string::npos) {
            std::string text = extractTextFromStream(decompressed);
            if (!text.empty()) {
                allTexts.push_back(text);
            }
        }

        pos = endStream + 9;  // strlen("endstream")
    }

    // ── 合并所有文本 ──
    std::string result;
    for (const auto& t : allTexts) {
        // 去除首尾空白
        std::string trimmed = t;
        size_t start = 0;
        while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\n' ||
               trimmed[start] == '\r' || trimmed[start] == '\t')) {
            ++start;
        }
        size_t end = trimmed.size();
        while (end > start && (trimmed[end - 1] == ' ' || trimmed[end - 1] == '\n' ||
               trimmed[end - 1] == '\r' || trimmed[end - 1] == '\t')) {
            --end;
        }
        if (start < end) {
            if (!result.empty()) result += "\n";
            result += trimmed.substr(start, end - start);
        }
    }

    // 合并连续换行
    std::string clean;
    clean.reserve(result.size());
    for (char c : result) {
        if (c == '\n' && !clean.empty() && clean.back() == '\n') {
            continue;  // 跳过连续的换行
        }
        clean += c;
    }

    return clean;
}

} // namespace document
