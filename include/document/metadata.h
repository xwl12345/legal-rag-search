#pragma once
#include <string>
#include <regex>
#include <optional>

namespace document {

/// 裁判结果倾向（第 8 类元数据，T1 / S1 第 1 层）
///
/// 由「判决如下 / 裁定如下」主文段的判项推导，用于文档库展示与四维筛选。
/// 判定规则与取舍见 metadata.cpp 中 extractResultTendency() 的函数头注释。
enum class ResultTendency {
    Unknown,          // 未能判定（无主文段 / 无实质判项）
    FavorPlaintiff,   // 利于原告
    FavorDefendant,   // 利于被告
    PartialSupport,   // 部分支持
    Other             // 其他（刑事定罪量刑、破产受理、撤销行政行为、二审驳回上诉等）
};

/// 结果倾向 → 中文标签（用于界面显示与筛选比对）
const char* resultTendencyLabel(ResultTendency tendency);

/// 中文标签 → 结果倾向（标签不匹配返回 Unknown）
ResultTendency resultTendencyFromLabel(const std::string& label);

/// 法律文档结构化元数据
struct DocMetadata {
    std::string caseNumber;    // 案号，如 (2024)京0105民初12345号
    std::string court;         // 审理法院，如 北京市朝阳区人民法院
    std::string date;          // 裁判日期，如 2024-03-15
    std::string caseType;      // 案件类型：民事/刑事/行政/知识产权/商事/其他
    std::string litigants;     // 当事人摘要，如 原告张三诉被告李四
    std::string procedure;     // 审判程序：一审/二审/再审

    ResultTendency tendency = ResultTendency::Unknown;  // 第 8 类：裁判结果倾向

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

    /// 提取裁判结果倾向（第 8 类元数据）
    ///
    /// 定位「判决如下 / 裁定如下 / 判令如下」主文段，按判项措辞判定倾向。
    /// 判定依据全部来自判项本身；无法确定时返回 Other 或 Unknown，不做猜测。
    static ResultTendency extractResultTendency(const std::string& text);

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
