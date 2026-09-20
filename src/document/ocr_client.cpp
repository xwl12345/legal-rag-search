#include "document/ocr_client.h"
#include <QProcess>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QDebug>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <vector>

namespace {

bool canImportFitz(const QString& pythonPath) {
    QProcess proc;
    proc.start(pythonPath, {"-c", "import fitz"});
    if (!proc.waitForStarted(5000)) {
        qDebug() << "[OCR] Ignoring unusable Python candidate:" << pythonPath
                 << proc.errorString();
        return false;
    }
    if (!proc.waitForFinished(10000)) {
        proc.kill();
        proc.waitForFinished(5000);
        qDebug() << "[OCR] Python capability probe timed out:" << pythonPath;
        return false;
    }
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

} // namespace

namespace document {

// ═══════════════════════════════════════════════════════════════
// 查找 Python 解释器
// ═══════════════════════════════════════════════════════════════

std::optional<std::string> OcrClient::findPython() {
    // 进程级缓存：探测每个候选需启动一个 python -c "import fitz" 子进程
    // （waitFor* 阻塞式等待，单个候选最长约 15 秒），且 OCR 环境在进程生命周期内
    // 不会变化——isAvailable() 与 extractText() 各探测一次纯属浪费，必须缓存。
    static const std::optional<std::string> cached = []() -> std::optional<std::string> {
        const QString appDir = QCoreApplication::applicationDirPath();
        const std::vector<QString> localCandidates = {
            appDir + "/venv/Scripts/python.exe",
            appDir + "/.venv/Scripts/python.exe",
            appDir + "/../venv/Scripts/python.exe",
            appDir + "/../.venv/Scripts/python.exe",
            "D:/python/Anaconda3/python.exe",
        };

        for (const auto& candidate : localCandidates) {
            if (QFileInfo(candidate).isFile() && canImportFitz(candidate)) {
                return candidate.toStdString();
            }
        }

        // 官方安装器 / winget 的标准安装位置（Python 310–313，版本号倒序优先）。
        // 放在 PATH 探测之前：PATH 上的 python.exe 可能是 Windows 商店占位程序，
        // 会导致真实安装的 Python 永远探测不到。
        const QStringList installBases = {
            QDir::homePath() + "/AppData/Local/Programs/Python",  // 每用户安装
            "C:/Program Files/Python",                            // 全机安装
        };
        for (const auto& base : installBases) {
            const QDir baseDir(base);
            if (!baseDir.exists()) continue;
            const auto versions = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                    QDir::Name | QDir::Reversed);
            for (const auto& ver : versions) {
                if (!ver.startsWith("Python")) continue;
                const QString candidate = base + "/" + ver + "/python.exe";
                if (QFileInfo(candidate).isFile() && canImportFitz(candidate)) {
                    return candidate.toStdString();
                }
            }
        }

        const QStringList pathCandidates = {
            QStandardPaths::findExecutable("python.exe"),
            QStandardPaths::findExecutable("python3.exe"),
        };
        for (const auto& candidate : pathCandidates) {
            if (!candidate.isEmpty() && canImportFitz(candidate)) {
                return candidate.toStdString();
            }
        }

        return std::nullopt;
    }();
    return cached;
}

// ═══════════════════════════════════════════════════════════════
// 查找 OCR 脚本
// ═══════════════════════════════════════════════════════════════

std::string OcrClient::findOcrScript() {
    const QString appDir = QCoreApplication::applicationDirPath();

    const QStringList candidates = {
        appDir + "/tools/pdf_ocr.py",
        appDir + "/../tools/pdf_ocr.py",
        appDir + "/../../tools/pdf_ocr.py",
        "tools/pdf_ocr.py",
        "../tools/pdf_ocr.py",
    };

    for (const auto& path : candidates) {
        QFileInfo fileInfo(path);
        if (fileInfo.isFile()) {
            return fileInfo.absoluteFilePath().toStdString();
        }
    }

    return "";
}

// ═══════════════════════════════════════════════════════════════
// 环境检查
// ═══════════════════════════════════════════════════════════════

bool OcrClient::isAvailable() {
    return findPython().has_value();
}

// ═══════════════════════════════════════════════════════════════
// 主入口
// ═══════════════════════════════════════════════════════════════

OcrResult OcrClient::extractText(const std::string& pdfPath,
                                 int timeoutMs,
                                 const std::function<bool()>& cancelled,
                                 const std::function<void(int, int)>& onPage) {
    const QString pdfPathQt = QString::fromStdString(pdfPath);
    const auto python = findPython();
    const std::string script = findOcrScript();

    if (!QFileInfo::exists(pdfPathQt)) {
        const std::string diagnostic = "找不到 PDF 文件";
        qWarning() << "[OCR] PDF file not found:" << QString::fromStdString(pdfPath);
        return {OcrStatus::PdfNotFound, "", diagnostic};
    }

    if (!python.has_value()) {
        return {OcrStatus::PythonNotFound, "",
                "未找到可运行 PyMuPDF（fitz）的 Python 解释器，请安装或配置 Python 环境"};
    }

    if (script.empty() || !QFileInfo::exists(QString::fromStdString(script))) {
        const std::string diagnostic = "未找到 OCR 辅助脚本 pdf_ocr.py，请重新部署程序";
        qWarning() << "[OCR] Script not found:" << QString::fromStdString(script);
        return {OcrStatus::ScriptNotFound, "", diagnostic};
    }

    QProcess proc;
    proc.start(QString::fromStdString(*python),
               {QString::fromStdString(script), pdfPathQt});

    if (!proc.waitForStarted(5000)) {
        const std::string diagnostic = "无法启动 Python OCR：" +
            proc.errorString().toStdString();
        qWarning() << "[OCR] Python start failed:" << QString::fromStdString(diagnostic);
        return {OcrStatus::PythonStartFailed, "", diagnostic};
    }

    // 用事件循环等待子进程结束：OCR 逐页识别可能持续数分钟，期间持续泵
    // 界面事件，避免主窗口被系统标记“未响应”。不屏蔽输入事件——调用方
    // （界面层）以模态进度对话框阻挡主窗口，使等待期间仅对话框可交互
    // （取消按钮），既消除“点击无反应”的假死感，又避免主界面重入。
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    // 持续读走子进程的 stdout/stderr：管道缓冲区有限，若识别文本（20 页
    // 扫描件可达数十 KB）或诊断输出塞满缓冲而父进程不读，子进程会阻塞在
    // write 上永不退出，父进程只能白等满超时——必须在等待期间异步排空。
    QString stdoutText;
    QString stderrText;
    const QRegularExpression pageProgress{R"(\[OCR\] Page (\d+)/(\d+) done)"};
    QObject::connect(&proc, &QProcess::readyReadStandardOutput, &loop, [&]() {
        stdoutText += QString::fromUtf8(proc.readAllStandardOutput());
    });
    QObject::connect(&proc, &QProcess::readyReadStandardError, &loop, [&]() {
        const QString delta = QString::fromUtf8(proc.readAllStandardError());
        stderrText += delta;
        // 从新增的 stderr 里提取最后一条页进度，向调用方汇报识别进度
        if (onPage) {
            QRegularExpressionMatch match;
            auto it = pageProgress.globalMatch(delta);
            while (it.hasNext()) match = it.next();
            if (match.hasMatch()) {
                onPage(match.captured(1).toInt(), match.captured(2).toInt());
            }
        }
    });

    // 取消轮询：每约 200ms 询问一次调用方是否要求终止
    bool userCancelled = false;
    QTimer poll;
    poll.setInterval(200);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (cancelled && cancelled()) {
            userCancelled = true;
            loop.quit();
        }
    });

    QObject::connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    poll.start();
    loop.exec();
    poll.stop();

    if (userCancelled) {
        qInfo() << "[OCR] Cancelled by user, killing process";
        proc.kill();
        proc.waitForFinished(5000);
        return {OcrStatus::Cancelled, "", "已取消 OCR 识别"};
    }

    if (!timer.isActive()) {  // 超时触发退出
        qWarning() << "[OCR] Timeout after" << timeoutMs << "ms, killing process";
        proc.kill();
        proc.waitForFinished(5000);
        return {OcrStatus::TimedOut, "", "OCR 识别超时，请尝试页数更少或更清晰的 PDF"};
    }
    timer.stop();

    const QString trimmedStderr = stderrText.trimmed();
    if (proc.exitCode() != 0) {
        QString detail = trimmedStderr;
        if (detail.size() > 300) detail = detail.left(300) + "...";
        const std::string diagnostic = detail.isEmpty()
            ? "PDF OCR 识别失败（Python 进程异常退出）"
            : "PDF OCR 识别失败：" + detail.toStdString();
        qWarning() << "[OCR] Failed (exit code" << proc.exitCode() << "):" << trimmedStderr;
        return {OcrStatus::ProcessFailed, "", diagnostic};
    }

    const std::string text = stdoutText.toStdString();
    if (QString::fromStdString(text).trimmed().isEmpty()) {
        return {OcrStatus::NoText, "", "未能从 PDF 中识别出可检索文本"};
    }

    return {OcrStatus::Success, text, ""};
}

} // namespace document
