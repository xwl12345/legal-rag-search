#pragma once
#include <string>
#include <regex>
#include <optional>

namespace document {

/// 法律文档结构化元数据
struct DocMetadata {
    std::string caseNumber;    // 案号，如 (2024)京0105民初12345号
    std::string court;         // 审理法院，如 北京市朝阳区人民法院
    std::string date;          // 裁判日期，如 2024-03-15
    std::string caseType;      // 案件类型：民事/刑事/行政/知识产权/商事/其他
    std::string litigants;     // 当事人摘要，如 原告张三诉被告李四
    std::string procedure;     // 审判程序：一审/二审/再审

    /// 是否提取到任何有效元数据
    bool isEmpty() const {
        return caseNumber.empty() && court.empty() && date.empty();
    }
};

/// 法律文档元数据提取器
///
/// 从裁判文书文本中通过正则表达式提取结构化元数据。
/// 支持中国法院裁判文书的标准格式（案号、法院、日期等）。
class MetadataExtractor {
public:
    /// 从文本中提取元数据
    static DocMetadata extract(const std::string& text);

    /// 仅提取案号
    static std::optional<std::string> extractCaseNumber(const std::string& text);

private:
    /// 提取审理法院
    static std::optional<std::string> extractCourt(const std::string& text);

    /// 提取裁判日期（支持多种中文日期格式）
    static std::optional<std::string> extractDate(const std::string& text);

    /// 从案号推导案件类型
    static std::string deriveCaseType(const std::string& caseNumber);

    /// 提取当事人信息
    static std::optional<std::string> extractLitigants(const std::string& text);

    /// 判断审判程序
    static std::string deriveProcedure(const std::string& caseNumber,
                                       const std::string& text,
                                       const std::string& court);

    // ── 中文数字转阿拉伯数字 ──
    static int chineseNumToInt(const std::string& cn);
};

} // namespace document
