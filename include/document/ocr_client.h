#pragma once
#include <string>
#include <optional>

namespace document {

enum class OcrStatus {
    Success,
    PdfNotFound,
    ScriptNotFound,
    PythonNotFound,
    PythonStartFailed,
    TimedOut,
    ProcessFailed,
    NoText
};

/// OCR 执行结果，保留供界面展示的失败原因。
struct OcrResult {
    OcrStatus status = OcrStatus::NoText;
    std::string text;
    std::string diagnostic;

    bool isSuccess() const { return status == OcrStatus::Success; }
};

/// PDF OCR 客户端 — 通过 QProcess 调用 Python OCR 脚本
///
/// 当 PdfExtractor 对图片型 PDF 返回空文本时，回退到此 OCR 流程。
class OcrClient {
public:
    /// 对 PDF 文件执行 OCR
    /// @param pdfPath  PDF 文件路径
    /// @param timeoutMs 超时时间（毫秒），默认 5 分钟
    /// @return 识别文本及失败原因
    static OcrResult extractText(const std::string& pdfPath,
                                 int timeoutMs = 300000);

    /// 检查 OCR 环境是否可用（Python + PyMuPDF）
    static bool isAvailable();

private:
    /// 查找可运行 PyMuPDF 的 Python 解释器
    static std::optional<std::string> findPython();
    /// 查找 OCR 脚本路径
    static std::string findOcrScript();
};

} // namespace document
