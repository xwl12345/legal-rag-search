#include "document/ocr_client.h"
#include <QProcess>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace document {

// ═══════════════════════════════════════════════════════════════
// 查找 Python 解释器
// ═══════════════════════════════════════════════════════════════

std::string OcrClient::findPython() {
    // 1. 检查当前目录下的 venv
    std::string appDir = QCoreApplication::applicationDirPath().toStdString();
    std::vector<std::string> candidates = {
        appDir + "/venv/Scripts/python.exe",
        appDir + "/../venv/Scripts/python.exe",
    };

    // 2. 系统 PATH 中的 python
    candidates.push_back("python");
    candidates.push_back("python3");

    // 3. 常见 Anaconda 路径
    candidates.push_back("D:/python/Anaconda3/python.exe");

    for (const auto& py : candidates) {
        QProcess proc;
        proc.start(QString::fromStdString(py), {"--version"});
        proc.waitForFinished(5000);
        if (proc.exitCode() == 0) {
            return py;
        }
    }

    return "python";  // 回退默认值
}

// ═══════════════════════════════════════════════════════════════
// 查找 OCR 脚本
// ═══════════════════════════════════════════════════════════════

std::string OcrClient::findOcrScript() {
    std::string appDir = QCoreApplication::applicationDirPath().toStdString();

    std::vector<std::string> candidates = {
        appDir + "/../tools/pdf_ocr.py",
        appDir + "/../../tools/pdf_ocr.py",
        "tools/pdf_ocr.py",
        "../tools/pdf_ocr.py",
    };

    for (const auto& path : candidates) {
        if (QFileInfo::exists(QString::fromStdString(path))) {
            return std::filesystem::absolute(path).string();
        }
    }

    return "tools/pdf_ocr.py";
}

// ═══════════════════════════════════════════════════════════════
// 环境检查
// ═══════════════════════════════════════════════════════════════

bool OcrClient::isAvailable() {
    std::string python = findPython();
    QProcess proc;
    proc.start(QString::fromStdString(python),
               {"-c", "import paddleocr; import fitz"});
    proc.waitForFinished(10000);
    return proc.exitCode() == 0;
}

// ═══════════════════════════════════════════════════════════════
// 主入口
// ═══════════════════════════════════════════════════════════════

std::string OcrClient::extractText(const std::string& pdfPath, int timeoutMs) {
    std::string python = findPython();
    std::string script = findOcrScript();

    if (!QFileInfo::exists(QString::fromStdString(pdfPath))) {
        return "";
    }

    QProcess proc;
    proc.start(QString::fromStdString(python),
               {QString::fromStdString(script),
                QString::fromStdString(pdfPath)});

    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        return "";
    }

    if (proc.exitCode() != 0) {
        // OCR 失败（stderr 中会有错误信息）
        return "";
    }

    return proc.readAllStandardOutput().toStdString();
}

} // namespace document
