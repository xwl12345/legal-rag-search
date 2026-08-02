#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace document {

/// 轻量级 PDF 文本提取器
///
/// 支持从文本型 PDF 中提取文字内容，处理 FlateDecode 压缩流。
/// 不依赖外部 PDF 库，仅需 zlib（MinGW 自带）。
///
/// 局限性：
///   - 不支持扫描版/图片型 PDF（需 OCR）
///   - 不支持加密 PDF
///   - 复杂排版（多栏、表格）的文本顺序可能不理想
///   - 不支持 PDF 1.5+ 的对象流（CrossRef streams）
class PdfExtractor {
public:
    /// 从 PDF 文件提取纯文本
    /// @return 提取的文本，失败时返回空字符串
    static std::string extractText(const std::string& filePath);

private:
    // ── 文件 I/O ──
    static std::vector<uint8_t> readFile(const std::string& path);

    // ── 流解压 ──
    /// zlib inflate 解压
    static std::vector<uint8_t> inflateData(const std::string& data);
    /// ASCII85Decode 解码（部分 PDF 使用）
    static std::vector<uint8_t> decodeAscii85(const std::string& data);

    // ── 内容流解析 ──
    /// 从 PDF 内容流中提取文本
    static std::string extractTextFromStream(const std::vector<uint8_t>& data);
    /// 解码 PDF 字符串（处理转义、括号配对、八进制等）
    static std::string decodePdfString(const std::string& raw, size_t& pos);
    /// 将 PDF 编码文本转为 UTF-8
    static std::string pdfTextToUtf8(const std::string& text);
};

} // namespace document
