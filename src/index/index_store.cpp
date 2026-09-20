#include "index/index_store.h"
#include "config/app_config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <cstring>

namespace index_store {

namespace {

// ── 文件格式 ───────────────────────────────────────────────
// magic(8) | version(u32) | docCount(u32) | [docRecord]*docCount | crc32(u32)
//
// docRecord:
//   str docId | str sourcePath | str importedAt
//   u32 chunkCount | u64 byteSize | u8 ocr
//   str caseNumber | str court | str date | str caseType | str litigants
//   str procedure | str tendency
//   u64 fullTextLen | fullTextBytes
//   [u32 chunkLen | chunkBytes] * chunkCount
//
// str  = u32 长度 + 原始字节（UTF-8），不写入 '\0'
constexpr char kMagic[8] = {'L', 'R', 'A', 'G', 'I', 'D', 'X', '1'};
constexpr std::uint32_t kVersion = 2;   // v2: 增加 fullText（T10 全文阅读）

// 单个字符串上限 512 MB，防止坏文件导致巨额分配
constexpr std::uint32_t kMaxStringBytes = 512u * 1024u * 1024u;

/// 顺序写入缓冲区（小端序固定，跨平台一致）
class Writer {
public:
    void u8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }

    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        }
    }

    void raw(const void* data, std::size_t size) {
        const char* p = static_cast<const char*>(data);
        buf_.append(p, p + size);
    }

    /// 长度前缀字符串
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        if (!s.empty()) raw(s.data(), s.size());
    }

    const std::string& data() const { return buf_; }

private:
    std::string buf_;
};

/// 顺序读取器；任何越界立即置 failed 并由调用方统一处理
class Reader {
public:
    explicit Reader(const char* data, std::size_t size) : p_(data), end_(data + size) {}

    bool failed() const { return failed_; }
    std::size_t remaining() const {
        return failed_ ? 0 : static_cast<std::size_t>(end_ - p_);
    }

    std::uint8_t u8() {
        if (remaining() < 1) { failed_ = true; return 0; }
        return static_cast<std::uint8_t>(*p_++);
    }

    std::uint32_t u32() {
        if (remaining() < 4) { failed_ = true; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(static_cast<unsigned char>(*p_++)) << (8 * i);
        }
        return v;
    }

    std::uint64_t u64() {
        if (remaining() < 8) { failed_ = true; return 0; }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(*p_++)) << (8 * i);
        }
        return v;
    }

    std::string str() {
        const std::uint32_t len = u32();
        if (failed_ || len > kMaxStringBytes || remaining() < len) {
            failed_ = true;
            return {};
        }
        std::string s(p_, len);
        p_ += len;
        return s;
    }

    /// 读取 len 字节的定长字符串（用于全文原文；无长度前缀）
    std::string fixedString(std::size_t len) {
        if (failed_ || remaining() < len) {
            failed_ = true;
            return {};
        }
        std::string s(p_, len);
        p_ += len;
        return s;
    }

private:
    const char* p_ = nullptr;
    const char* end_ = nullptr;
    bool failed_ = false;
};

/// CRC-32（多项式 0xEDB88320），用于校验索引文件完整性
std::uint32_t crc32(const char* data, std::size_t size) {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t b = static_cast<std::uint8_t>(data[i]);
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

std::string nowTimestamp() {
    return QDateTime::currentDateTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        .toStdString();
}

IndexStore::IndexStore(std::string filePath)
    : filePath_(filePath.empty() ? std::string(config::INDEX_FILE) : std::move(filePath)) {}

bool IndexStore::exists() const {
    const QFileInfo info(QString::fromStdString(filePath_));
    return info.isFile() && info.size() > 0;
}

std::uint64_t IndexStore::fileSize() const {
    const QFileInfo info(QString::fromStdString(filePath_));
    return info.isFile() ? static_cast<std::uint64_t>(info.size()) : 0;
}

bool IndexStore::remove() const {
    const QString path = QString::fromStdString(filePath_);
    if (!QFile::exists(path)) {
        return true;
    }
    return QFile::remove(path);
}

IndexStore::SaveResult IndexStore::save(const std::vector<StoredDocument>& documents) {
    SaveResult result;

    Writer w;
    w.raw(kMagic, sizeof(kMagic));
    w.u32(kVersion);
    w.u32(static_cast<std::uint32_t>(documents.size()));

    for (const auto& doc : documents) {
        w.str(doc.docId);
        w.str(doc.sourcePath);
        w.str(doc.importedAt);
        w.u32(static_cast<std::uint32_t>(doc.chunkCount));
        w.u64(doc.byteSize);
        w.u8(doc.ocr ? 1 : 0);
        w.str(doc.metadata.caseNumber);
        w.str(doc.metadata.court);
        w.str(doc.metadata.date);
        w.str(doc.metadata.caseType);
        w.str(doc.metadata.litigants);
        w.str(doc.metadata.procedure);
        w.str(document::resultTendencyLabel(doc.metadata.tendency));

        w.u64(static_cast<std::uint64_t>(doc.fullText.size()));
        if (!doc.fullText.empty()) {
            w.raw(doc.fullText.data(), doc.fullText.size());
        }

        // 以存储的 chunks 为准写块数，保证内存中的 chunkCount 与落盘一致
        w.u32(static_cast<std::uint32_t>(doc.chunks.size()));
        for (const auto& chunk : doc.chunks) {
            w.str(chunk);
        }
    }

    const std::string& payload = w.data();
    const std::uint32_t checksum = crc32(payload.data(), payload.size());

    // 先写临时文件，flush + 原子替换：中途断电/磁盘满都不会破坏已有索引
    const QString target = QString::fromStdString(filePath_);
    const QFileInfo targetInfo(target);
    if (!targetInfo.absoluteDir().exists()) {
        QDir().mkpath(targetInfo.absolutePath());
    }

    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.diagnostic = "无法写入索引文件：" + target.toStdString();
        return result;
    }
    if (file.write(payload.data(), static_cast<qint64>(payload.size())) !=
        static_cast<qint64>(payload.size())) {
        result.diagnostic = "索引写入不完整（磁盘空间不足？）";
        file.cancelWriting();
        return result;
    }
    char crcBytes[4];
    for (int i = 0; i < 4; ++i) {
        crcBytes[i] = static_cast<char>((checksum >> (8 * i)) & 0xFF);
    }
    if (file.write(crcBytes, 4) != 4) {
        result.diagnostic = "索引校验字段写入失败";
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.diagnostic = "索引文件提交失败：" + file.errorString().toStdString();
        return result;
    }

    result.ok = true;
    result.bytesWritten = static_cast<std::uint64_t>(payload.size()) + 4;
    return result;
}

IndexStore::LoadResult IndexStore::load(std::vector<StoredDocument>& documents) const {
    LoadResult result;
    documents.clear();

    const QString path = QString::fromStdString(filePath_);
    QFile file(path);
    if (!file.exists()) {
        result.diagnostic = "索引文件不存在";
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        result.diagnostic = "无法打开索引文件";
        return result;
    }

    const QByteArray blob = file.readAll();
    file.close();
    if (blob.size() < static_cast<int>(sizeof(kMagic) + 4 + 4 + 4)) {
        result.diagnostic = "索引文件过短，已忽略";
        return result;
    }

    const std::size_t payloadSize = static_cast<std::size_t>(blob.size()) - 4;
    if (std::memcmp(blob.constData(), kMagic, sizeof(kMagic)) != 0) {
        result.diagnostic = "索引文件标识不匹配（非本程序生成）";
        return result;
    }

    // 校验 CRC
    const char* crcPtr = blob.constData() + payloadSize;
    std::uint32_t storedCrc = 0;
    for (int i = 0; i < 4; ++i) {
        storedCrc |= static_cast<std::uint32_t>(
                         static_cast<unsigned char>(crcPtr[i])) << (8 * i);
    }
    if (crc32(blob.constData(), payloadSize) != storedCrc) {
        result.diagnostic = "索引文件校验失败（内容损坏），已忽略并保留原文件";
        return result;
    }

    Reader r(blob.constData() + sizeof(kMagic),
             payloadSize - sizeof(kMagic));

    const std::uint32_t version = r.u32();
    if (r.failed() || version != kVersion) {
        result.diagnostic = "索引文件版本不兼容（期望 v" +
                            std::to_string(kVersion) + "，实际 v" +
                            std::to_string(version) + "）";
        return result;
    }

    const std::uint32_t docCount = r.u32();
    if (r.failed()) {
        result.diagnostic = "索引文件头损坏";
        return result;
    }
    documents.reserve(docCount);

    for (std::uint32_t i = 0; i < docCount; ++i) {
        StoredDocument doc;
        doc.docId = r.str();
        doc.sourcePath = r.str();
        doc.importedAt = r.str();
        doc.chunkCount = static_cast<int>(r.u32());
        doc.byteSize = r.u64();
        doc.ocr = (r.u8() != 0);
        doc.metadata.caseNumber = r.str();
        doc.metadata.court = r.str();
        doc.metadata.date = r.str();
        doc.metadata.caseType = r.str();
        doc.metadata.litigants = r.str();
        doc.metadata.procedure = r.str();
        doc.metadata.tendency = document::resultTendencyFromLabel(r.str());

        const std::uint64_t fullTextLen = r.u64();
        if (r.failed() || fullTextLen > kMaxStringBytes ||
            r.remaining() < fullTextLen) {
            result.diagnostic = "索引文件正文段损坏";
            documents.clear();
            return result;
        }
        doc.fullText = r.fixedString(static_cast<std::size_t>(fullTextLen));
        if (r.failed()) {
            result.diagnostic = "索引文件正文读取失败";
            documents.clear();
            return result;
        }

        const std::uint32_t storedChunkCount = r.u32();
        if (r.failed()) {
            result.diagnostic = "索引文件分块段损坏";
            documents.clear();
            return result;
        }
        doc.chunks.reserve(storedChunkCount);
        for (std::uint32_t c = 0; c < storedChunkCount; ++c) {
            doc.chunks.push_back(r.str());
            if (r.failed()) {
                result.diagnostic = "索引文件分块内容损坏";
                documents.clear();
                return result;
            }
        }
        // 以实际读出的块数为准（坏文件时不至于让 chunkCount 说谎）
        doc.chunkCount = static_cast<int>(doc.chunks.size());

        result.chunkCount += doc.chunkCount;
        documents.push_back(std::move(doc));
    }

    if (r.failed()) {
        documents.clear();
        result.diagnostic = "索引文件结构损坏";
        return result;
    }

    result.ok = true;
    result.documentCount = static_cast<int>(documents.size());
    return result;
}

}  // namespace index_store
