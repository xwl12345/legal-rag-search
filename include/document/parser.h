#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>
#include <functional>

namespace document {

/// 文本块角色（T5 段落角色标注）：按裁判文书固定结构给块分级，支撑
/// 「检索结果证据效力分级」与「只看本院认为」过滤。
///
/// 语义：**位标志**。块所属的每个结构段置一位——正常块只属一段；跨段块
/// （如短文书的「本院认为……判决如下」同块）同时持有两段位，两个段位的
/// 过滤都能命中，不丢内容。段起点 = 全文中最近一次出现的结构标记；
/// 无标记的块延续当前段。角色随解析产生，与文本块一同进索引、随索引持久化。
enum class ChunkRole : std::uint8_t {
    Unknown = 0,           // 未识别（首部、尾部、无结构文书、法条等）
    PlaintiffClaims = 1,   // 当事人主张：原告诉称 / 上诉人诉称……
    DefendantDefense = 2,  // 当事人答辩：被告辩称 / 被上诉人辩称……
    CourtOpinion = 4,      // 法院认定：经审理查明 / 本院查明 / 本院认为
    Judgment = 8,          // 裁判主文：判决如下 / 裁定如下
};

/// 角色组合的展示标签：按 诉称/辩称/本院认为/判决 顺序列出命中段，
/// 多段以 "|" 连接（如「本院认为|判决」）；Unknown 返回空串，不显示。
std::string chunkRoleLabel(ChunkRole role);

/// 文本块：文档解析后的基本检索单元
struct TextChunk {
    std::string docId;       // 来源文档 ID（文件名）
    int chunkIndex = 0;      // 在文档中的块序号
    std::string content;     // 文本内容
    int startPos = 0;        // 在原文档中的起始位置（字节偏移；注意 overlap 会使其
                             // 不等于「去掉重叠后的净偏移」，当前无消费方）
    ChunkRole role = ChunkRole::Unknown;   // T5：结构段角色（annotateChunkRoles 填充）
};

/// 按裁判文书固定结构给有序文本块打角色标签（T5）。
///
/// 规则：
///   1. 识别标准结构标记：诉称 / 辩称 / 经审理查明 / 本院查明 / 本院认为 /
///      判决如下 / 裁定如下（语料中「原告张某某诉称」等变体同样命中）；
///   2. 段落按规范序单调推进：诉称 → 辩称 → 法院认定 → 主文。块内只把
///      「比当前段更靠后」的标记计为新段位——本院认为段落里复述
///      「原告诉称」属于引述，不会往块上添加当事人段位；
///   3. 块位 = 块内全部推进段标记的位并集；无推进标记则整块延续当前段。
///      跨段块因此同时持有两段位（如「……本院认为……判决如下……」
///      = 法院认定|主文），两个段的过滤都能命中。
void annotateChunkRoles(std::vector<TextChunk>& chunks);

/// 文档内容来源。
enum class ParseSource {
    None,
    TextFile,
    NativePdf,
    Ocr
};

/// 文档解析状态。
enum class ParseStatus {
    Success,
    FileOpenFailed,
    InvalidPdf,
    NoTextExtracted,
    UnsupportedEncoding,   // P0-6：文本编码无法识别（非 UTF-8 / GB18030 / UTF-16），拒绝导入而非吞成乱码
    OcrFailed,
    OcrCancelled
};

/// 文档解析结果，包含文本块和失败诊断。
struct ParseResult {
    ParseStatus status = ParseStatus::NoTextExtracted;
    ParseSource source = ParseSource::None;
    std::vector<TextChunk> chunks;
    std::string diagnostic;
    std::string content;   // P0-5：分块前的整篇原文（Retriever 存 fullText 用，含 overlap 的分块拼不出原文）

    bool isSuccess() const { return status == ParseStatus::Success; }
};

/// 文档解析器：支持 .txt / .md / .pdf 文件
class DocumentParser {
public:
    /// 运行时配置分块参数（T3 配置中心）。
    /// ⚠️ 只影响**之后解析**的文档：已建索引的块边界在建块时已固定，
    /// 改参数不会让现有文本块重新切（设置页有同样标注）。
    void setChunkParams(int maxSize, int overlap) {
        chunkMaxSize_ = maxSize;
        chunkOverlap_ = overlap;
    }

    /// 读取文件、提取文本并切分为文本块，同时返回诊断结果
    /// @param cancelled 每约 200ms 轮询一次，返回 true 时取消 OCR 回退流程
    /// @param onPage    OCR 逐页识别的进度回调（当前页号、总页号）
    ParseResult parseWithResult(const std::string& filePath,
                                const std::function<bool()>& cancelled = {},
                                const std::function<void(int, int)>& onPage = {});

    /// 读取文件并切分成文本块（兼容旧调用方）
    std::vector<TextChunk> parse(const std::string& filePath);

    /// 从纯文本字符串解析
    std::vector<TextChunk> parseText(std::string_view text, const std::string& docId);

private:
    /// 将长文本按 maxChunkSize 切分（带 overlap）
    static std::vector<std::string> splitChunks(std::string_view text,
                                                 int maxSize = 512,
                                                 int overlap = 50);

    int chunkMaxSize_ = 512;   // T3：可配置分块上限（默认与原行为一致）
    int chunkOverlap_ = 50;    // T3：可配置重叠字符数
};

} // namespace document
