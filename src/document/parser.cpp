#include "document/parser.h"
#include "document/pdf_extractor.h"
#include "document/ocr_client.h"
#include <QFile>
#include <QFileInfo>
#include <QString>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QStringDecoder>
#else
#include <QTextCodec>
#endif
#include <sstream>
#include <algorithm>
#include <cstring>

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

// ── 文本编码识别 → UTF-8（P0-6）──
// 旧实现按原始字节直接进索引：中文 Windows 记事本「ANSI」（GB18030）保存的文件
// 会全链路乱码且无任何报错。现在按序识别：
//   1. UTF-16 BOM（FF FE / FE FF）→ 按对应端序解码；
//   2. UTF-8 校验通过（含纯 ASCII）→ 原样保留；
//   3. GB18030 解码后「非法字符数」少于 UTF-8 → 按GB18030 转码；
//   4. 都解不动（典型：二进制文件被改成 .txt）→ 如实拒绝，导入报错而非吞成乱码。
bool decodeToUtf8(const QByteArray& raw, std::string& out, std::string& diag) {
    if (raw.isEmpty()) {
        out.clear();
        return true;
    }

    // UTF-8 BOM 先剥掉（U+FEFF 是合法 UTF-8，不剥会混进首块文本与元数据提取）
    QByteArray body = raw;
    bool hadUtf8Bom = body.startsWith("\xEF\xBB\xBF");
    if (hadUtf8Bom) {
        body.remove(0, 3);
        if (body.isEmpty()) {
            out.clear();
            return true;
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    auto tryDecode = [&body](const char* name, bool* ok) -> QString {
        QStringDecoder dec(name);
        *ok = dec.isValid();
        if (!*ok) return {};
        const QString decoded = dec(body);
        *ok = !dec.hasError();
        return decoded;
    };

    bool ok = false;
    if (body.startsWith("\xFF\xFE")) {
        const QString s = tryDecode("UTF-16LE", &ok);
        if (ok) { out = s.toUtf8().toStdString(); return true; }
    } else if (body.startsWith("\xFE\xFF")) {
        const QString s = tryDecode("UTF-16BE", &ok);
        if (ok) { out = s.toUtf8().toStdString(); return true; }
    } else {
        const QString asUtf8 = tryDecode("UTF-8", &ok);
        if (ok && !asUtf8.contains(QChar(0xFFFD))) {
            out = asUtf8.toUtf8().toStdString();
            return true;
        }
        const QString asGb = tryDecode("GB18030", &ok);
        if (ok) { out = asGb.toUtf8().toStdString(); return true; }
    }
#else
    // Qt5（当前工具链）：QTextCodec::ConverterState::invalidChars 精确计数
    auto decodeWith = [&body](const char* name, int* invalidChars) -> QString {
        QTextCodec* codec = QTextCodec::codecForName(name);
        QTextCodec::ConverterState state;
        const QString decoded = codec->toUnicode(body.constData(), body.size(), &state);
        *invalidChars = static_cast<int>(state.invalidChars);
        return decoded;
    };

    int utf16Invalid = -1;
    if (body.startsWith("\xFF\xFE")) {
        const QString s = decodeWith("UTF-16LE", &utf16Invalid);
        if (utf16Invalid == 0) { out = s.toUtf8().toStdString(); return true; }
    } else if (body.startsWith("\xFE\xFF")) {
        const QString s = decodeWith("UTF-16BE", &utf16Invalid);
        if (utf16Invalid == 0) { out = s.toUtf8().toStdString(); return true; }
    }

    // 二进制哨兵：正文文本不含 NUL（无 BOM 的 UTF-16 会走到这里被拦下）
    if (body.contains('\0')) {
        diag = "文件含二进制内容（NUL 字节），不是可检索的文本文档";
        return false;
    }

    int utf8Invalid = 0;
    decodeWith("UTF-8", &utf8Invalid);
    if (utf8Invalid == 0) {
        out.assign(body.constData(), static_cast<size_t>(body.size()));  // 校验通过，保留原字节
        return true;
    }

    int gbInvalid = 0;
    const QString asGb = decodeWith("GB18030", &gbInvalid);
    if (gbInvalid < utf8Invalid) {
        out = asGb.toUtf8().toStdString();
        return true;
    }
#endif

    diag = "无法识别的文本编码（非 UTF-8 / GB18030 / UTF-16），可能是不含文本的二进制文件";
    return false;
}

} // namespace

namespace document {

ParseResult DocumentParser::parseWithResult(const std::string& filePath,
                                            const std::function<bool()>& cancelled,
                                            const std::function<void(int, int)>& onPage) {
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
            const auto ocrResult = OcrClient::extractText(filePath, 600000, cancelled, onPage);
            if (ocrResult.status == OcrStatus::Cancelled) {
                return {ParseStatus::OcrCancelled, source, {}, ocrResult.diagnostic};
            }
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
        // P0-6：先做编码识别统一转成 UTF-8（GBK 的「ANSI」文件 / UTF-16 BOM 自动转码，
        // 二进制乱码文件如实拒绝），后续分词、索引、展示都只见 UTF-8。
        const QByteArray raw = file.readAll();
        file.close();
        std::string decoded, encodingDiag;
        if (!decodeToUtf8(raw, decoded, encodingDiag)) {
            return {ParseStatus::UnsupportedEncoding, source, {}, encodingDiag};
        }
        content = std::move(decoded);
        source = ParseSource::TextFile;
    }

    if (QString::fromStdString(content).trimmed().isEmpty()) {
        return {ParseStatus::NoTextExtracted, source, {}, "文件不包含可检索文本"};
    }

    auto chunks = parseText(content, docId);
    if (chunks.empty()) {
        return {ParseStatus::NoTextExtracted, source, {}, "文件不包含可检索文本"};
    }

    return {ParseStatus::Success, source, std::move(chunks), "", content};
}

std::vector<TextChunk> DocumentParser::parse(const std::string& filePath) {
    return parseWithResult(filePath).chunks;
}

std::vector<TextChunk> DocumentParser::parseText(std::string_view text,
                                                  const std::string& docId) {
    std::vector<TextChunk> chunks;
    auto rawChunks = splitChunks(text, chunkMaxSize_, chunkOverlap_);

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

    // T5：解析时即按文书结构打角色标签（无结构文本全部落 Unknown，零开销语义）
    annotateChunkRoles(chunks);

    return chunks;
}

// ── T5 段落角色标注 ──────────────────────────────────────────────

std::string chunkRoleLabel(ChunkRole role) {
    // 按 诉称 → 辩称 → 本院认为 → 判决 的规范序列出命中段，跨段块用 "|" 连接
    static const std::pair<ChunkRole, const char*> kLabels[] = {
        {ChunkRole::PlaintiffClaims,  "诉称"},
        {ChunkRole::DefendantDefense, "辩称"},
        {ChunkRole::CourtOpinion,     "本院认为"},
        {ChunkRole::Judgment,         "判决"},
    };
    const int bits = static_cast<int>(role);
    std::string label;
    for (const auto& [flag, text] : kLabels) {
        if (bits & static_cast<int>(flag)) {
            if (!label.empty()) label += "|";
            label += text;
        }
    }
    return label;
}

void annotateChunkRoles(std::vector<TextChunk>& chunks) {
    // 结构标记按规范序排列（见 parser.h 注释）；"诉称/辩称"用短标记，
    // 使「原告张某某诉称」「被上诉人辩称」等语料变体同样命中。
    struct SectionMarker {
        ChunkRole role;
        const char* text;
    };
    static const SectionMarker kMarkers[] = {
        {ChunkRole::PlaintiffClaims,   "诉称"},
        {ChunkRole::DefendantDefense,  "辩称"},
        {ChunkRole::CourtOpinion,      "经审理查明"},
        {ChunkRole::CourtOpinion,      "本院查明"},
        {ChunkRole::CourtOpinion,      "本院认为"},
        {ChunkRole::Judgment,          "判决如下"},
        {ChunkRole::Judgment,          "裁定如下"},
    };

    int current = 0;   // 已进入的最深段落（单调，引述不回退）
    for (auto& chunk : chunks) {
        int bits = 0;
        int maxSeen = current;
        for (const auto& marker : kMarkers) {
            if (std::search(chunk.content.begin(), chunk.content.end(),
                            marker.text, marker.text + std::strlen(marker.text))
                    != chunk.content.end()) {
                const int rank = static_cast<int>(marker.role);
                if (rank > current) {
                    bits |= rank;   // 只认推进段标记：本院认为里引述「诉称」不加位
                }
                maxSeen = std::max(maxSeen, rank);
            }
        }
        if (bits == 0) {
            bits = current;   // 无新段 → 整块延续当前段
        }
        chunk.role = static_cast<ChunkRole>(bits);
        current = maxSeen;
    }
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
