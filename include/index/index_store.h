#pragma once
#include "document/metadata.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace index_store {

/// 单篇文档的持久化记录（一个 docId 一条）。
///
/// 说明：全文原文（fullText）必须保存——T10 全文阅读页要读取整篇文书，
/// 而 512 字分块只够判断相关性。此处落盘后，T10 直接取用，无需重新解析。
struct StoredDocument {
    std::string docId;              // 文档 ID（文件名）
    std::string sourcePath;         // 原始文件绝对路径（供"打开原文"用）
    std::string importedAt;         // 导入时间，ISO-8601（yyyy-MM-dd HH:mm:ss）
    int chunkCount = 0;             // 文本块数
    std::uint64_t byteSize = 0;     // 原文 UTF-8 字节数
    bool ocr = false;               // 是否经 OCR 识别（扫描件不重跑 OCR）
    document::DocMetadata metadata; // 7 类元数据 + 第 8 类结果倾向

    std::string fullText;           // 整篇文书原文（T10 用）
    std::vector<std::string> chunks;  // 分块内容，chunks[i] 对应 chunkIndex=i
};

/// 索引持久化层：与 UI 完全解耦（不 include 任何 ui/ 头）。
///
/// 职责单一：把 Retriever 的内存索引切片写到磁盘、再从磁盘读回。
/// 采用"单文件 + 分节" 的自定义二进制格式，理由：
///   - 零第三方依赖（项目约定不引重型库）；
///   - 文本块含任意 UTF-8，用长度前缀写入，无需转义；
///   - 支持版本号与 CRC 校验，坏文件可被识别而非静默读成空索引。
class IndexStore {
public:
    /// 落盘结果
    struct SaveResult {
        bool ok = false;
        std::uint64_t bytesWritten = 0;
        std::string diagnostic;
    };

    /// 加载结果
    struct LoadResult {
        bool ok = false;
        int documentCount = 0;
        int chunkCount = 0;
        std::string diagnostic;
    };

    /// 指定索引文件路径（默认取 config::INDEX_FILE）
    explicit IndexStore(std::string filePath = {});

    /// 写出索引；函数内部先写临时文件再原子替换，避免中途失败留下半截文件
    SaveResult save(const std::vector<StoredDocument>& documents);

    /// 读回索引
    LoadResult load(std::vector<StoredDocument>& documents) const;

    /// 目标文件是否存在且非空
    bool exists() const;

    /// 目标文件字节数（不存在返回 0）
    std::uint64_t fileSize() const;

    /// 删除索引文件（不存在视为成功）
    bool remove() const;

    const std::string& filePath() const { return filePath_; }

private:
    std::string filePath_;
};

/// 当前时间戳，格式 yyyy-MM-dd HH:mm:ss（供导入时间落盘）
std::string nowTimestamp();

}  // namespace index_store
