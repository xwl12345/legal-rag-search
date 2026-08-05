#pragma once
#include <string>

namespace document {

/// PDF OCR 客户端 — 通过 QProcess 调用 Python PaddleOCR 脚本
///
/// 当 PdfExtractor 对图片型 PDF 返回空文本时，回退到此 OCR 流程。
class OcrClient {
public:
    /// 对 PDF 文件执行 OCR，返回提取的文本
    /// @param pdfPath  PDF 文件路径
    /// @param timeoutMs 超时时间（毫秒），默认 5 分钟
    /// @return 识别出的文本，失败返回空字符串
    static std::string extractText(const std::string& pdfPath,
                                   int timeoutMs = 300000);

    /// 检查 OCR 环境是否可用（Python + PaddleOCR + PyMuPDF）
    static bool isAvailable();

private:
    /// 查找 Python 解释器路径
    static std::string findPython();
    /// 查找 OCR 脚本路径
    static std::string findOcrScript();
};

} // namespace document
