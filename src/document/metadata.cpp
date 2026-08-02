#include "document/metadata.h"
#include <algorithm>
#include <cctype>
#include <map>

namespace document {

// ── 安全正则搜索：捕获异常，失败返回 false ──
namespace {
    bool safeRegexSearch(const std::string& text, std::smatch& match,
                         const std::regex& re) {
        try {
            return std::regex_search(text, match, re);
        } catch (const std::regex_error&) {
            return false;
        }
    }
} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// 中文数字转换
// ═══════════════════════════════════════════════════════════════
int MetadataExtractor::chineseNumToInt(const std::string& cn) {
    static const std::map<std::string, int> CN_DIGIT = {
        {"零", 0}, {"〇", 0}, {"一", 1}, {"二", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}, {"十", 10},
    };

    // 先拆成单个中文字符
    std::vector<std::string> chars;
    for (size_t i = 0; i < cn.size(); ) {
        unsigned char c = static_cast<unsigned char>(cn[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        chars.push_back(cn.substr(i, len));
        i += len;
    }

    // 检测是否为位置记数（如"二〇二四"→2024）：包含"〇"或长度≥4
    bool hasZero = false;
    for (const auto& ch : chars) {
        if (ch == "〇" || ch == "零") { hasZero = true; break; }
    }

    if (hasZero || chars.size() >= 4) {
        // 位置记数：逐位解析，如"二〇二四"→2→0→2→4
        int result = 0;
        for (const auto& ch : chars) {
            auto it = CN_DIGIT.find(ch);
            if (it != CN_DIGIT.end() && it->second < 10) {
                result = result * 10 + it->second;
            }
        }
        return result;
    }

    // 叠加记数："十五"→15, "二十五"→25
    int result = 0;
    for (const auto& ch : chars) {
        auto it = CN_DIGIT.find(ch);
        if (it != CN_DIGIT.end()) {
            int val = it->second;
            if (val == 10) {
                if (result == 0) result = 10;
                else result *= 10;
            } else {
                result += val;
            }
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════
// 案号提取（手动解析，避免 GCC regex Unicode 问题）
// ═══════════════════════════════════════════════════════════════
std::optional<std::string> MetadataExtractor::extractCaseNumber(const std::string& text) {
    // 查找 "（" 或 "(" 后跟 4 位年份
    for (size_t i = 0; i + 6 < text.size(); ++i) {
        if ((text[i] == '(' || text[i] == '\xEF') &&
            (text[i] == '(' || (static_cast<unsigned char>(text[i]) == 0xEF &&
             static_cast<unsigned char>(text[i+1]) == 0xBC &&
             static_cast<unsigned char>(text[i+2]) == 0x88))) {
            // 找到了开括号，检查后面是否为 4 位数字年份
            size_t j = i + 1;
            if (text[i] == '\xEF') j = i + 3;  // 跳过 "（" UTF-8

            if (j + 4 <= text.size() &&
                std::isdigit(static_cast<unsigned char>(text[j])) &&
                std::isdigit(static_cast<unsigned char>(text[j+1])) &&
                std::isdigit(static_cast<unsigned char>(text[j+2])) &&
                std::isdigit(static_cast<unsigned char>(text[j+3]))) {

                // 查找 "号" 结束
                size_t end = text.find("\xE5\x8F\xB7", j);  // "号" UTF-8
                if (end == std::string::npos) {
                    // 尝试 ASCII 情况（不太可能但做 fallback）
                    end = text.find("号", j);
                }
                if (end != std::string::npos && end - i <= 40) {
                    return text.substr(i, end - i + 3);
                }
            }
        }
    }

    // 回退：纯 ASCII 括号格式
    size_t pos = text.find("(");
    if (pos != std::string::npos) {
        size_t end = text.find("号", pos);
        if (end != std::string::npos && end - pos <= 40) {
            std::string candidate = text.substr(pos, end - pos + 3);
            // 验证基本格式：(4digits)...type...number号
            if (candidate.size() >= 10 &&
                std::isdigit(static_cast<unsigned char>(candidate[1])) &&
                std::isdigit(static_cast<unsigned char>(candidate[2])) &&
                std::isdigit(static_cast<unsigned char>(candidate[3])) &&
                std::isdigit(static_cast<unsigned char>(candidate[4]))) {
                return candidate;
            }
        }
    }

    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// 法院名称提取（手动解析）
// ═══════════════════════════════════════════════════════════════
std::optional<std::string> MetadataExtractor::extractCourt(const std::string& text) {
    // "人民法院" UTF-8 编码
    const std::string COURT_SUFFIX = "\xE4\xBA\xBA\xE6\xB0\x91\xE6\xB3\x95\xE9\x99\xA2";

    size_t pos = 0;
    std::string bestMatch;

    while ((pos = text.find(COURT_SUFFIX, pos)) != std::string::npos) {
        // 向前查找法院全名（最多 20 字节，含"高级/中级/基层"）
        size_t start = (pos > 30) ? pos - 30 : 0;

        // 向前找到中文词开始的位置
        size_t nameStart = pos;
        while (nameStart > start) {
            unsigned char c = static_cast<unsigned char>(text[nameStart - 1]);
            // 如果前一个字符是 ASCII（非中文），停止
            if (c < 0x80) {
                break;
            }
            // 检查是否是中文标点或空白
            std::string prev3 = text.substr(nameStart - 3, 3);
            if (prev3 == "\xE3\x80\x82" ||  // 。
                prev3 == "\xEF\xBC\x8C" ||  // ，
                prev3 == "\xE3\x80\x81" ||  // 、
                prev3 == "\xEF\xBC\x9A") {  // ：
                break;
            }
            nameStart -= 3;  // 回退一个中文字符
        }

        std::string court = text.substr(nameStart, pos - nameStart + COURT_SUFFIX.size());

        // 至少 6 字节（2 个中文字符 + "人民法院"）
        if (court.size() >= 6 + COURT_SUFFIX.size()) {
            bestMatch = court;
        }

        pos += COURT_SUFFIX.size();
    }

    if (!bestMatch.empty()) return bestMatch;
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// 日期提取
// ═══════════════════════════════════════════════════════════════
std::optional<std::string> MetadataExtractor::extractDate(const std::string& text) {
    // 模式 1: 阿拉伯数字日期 "2024年3月15日"
    {
        static std::regex re(R"((\d{4})\s*年\s*(\d{1,2})\s*月\s*(\d{1,2})\s*日)");
        std::smatch match;
        if (safeRegexSearch(text, match, re)) {
            char buf[16];
            int y = std::stoi(match[1].str());
            int m = std::stoi(match[2].str());
            int d = std::stoi(match[3].str());
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
            return std::string(buf);
        }
    }

    // 模式 2: ISO 日期 "2024-03-15"
    {
        static std::regex re(R"((\d{4})-(\d{1,2})-(\d{1,2}))");
        std::smatch match;
        if (safeRegexSearch(text, match, re)) {
            char buf[16];
            int y = std::stoi(match[1].str());
            int m = std::stoi(match[2].str());
            int d = std::stoi(match[3].str());
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
            return std::string(buf);
        }
    }

    // 模式 3: 中文数字日期，手动解析（避免 regex Unicode 问题）
    {
        // "年" "月" "日" UTF-8
        const std::string YEAR = "\xE5\xB9\xB4";   // 年
        const std::string MONTH = "\xE6\x9C\x88";   // 月
        const std::string DAY = "\xE6\x97\xA5";     // 日

        size_t yearPos = text.find(YEAR);
        if (yearPos != std::string::npos && yearPos >= 6) {
            size_t monthPos = text.find(MONTH, yearPos + 3);
            if (monthPos != std::string::npos && monthPos > yearPos + 3) {
                size_t dayPos = text.find(DAY, monthPos + 3);
                if (dayPos != std::string::npos && dayPos > monthPos + 3) {
                    // 判断一个 UTF-8 中文字符是否为中文数字
                    auto isChineseNum = [](const std::string& ch3) -> bool {
                        return ch3 == "零" || ch3 == "〇" || ch3 == "一" || ch3 == "二" ||
                               ch3 == "三" || ch3 == "四" || ch3 == "五" || ch3 == "六" ||
                               ch3 == "七" || ch3 == "八" || ch3 == "九" || ch3 == "十";
                    };

                    // 向前扫描提取年份中文数字（只取连续的中文数字）
                    std::string yearStr;
                    size_t yStart = yearPos;
                    while (yStart >= 3) {
                        std::string ch3 = text.substr(yStart - 3, 3);
                        if (isChineseNum(ch3)) {
                            yStart -= 3;
                            yearStr = text.substr(yStart, yearPos - yStart);
                        } else {
                            break;
                        }
                    }

                    // 提取月份中文数字
                    std::string monthStr;
                    size_t mStart = monthPos;
                    while (mStart >= 3) {
                        std::string ch3 = text.substr(mStart - 3, 3);
                        if (isChineseNum(ch3)) {
                            mStart -= 3;
                            monthStr = text.substr(mStart, monthPos - mStart);
                        } else {
                            break;
                        }
                    }

                    // 提取日期中文数字
                    std::string dayStr;
                    size_t dStart = dayPos;
                    while (dStart >= 3) {
                        std::string ch3 = text.substr(dStart - 3, 3);
                        if (isChineseNum(ch3)) {
                            dStart -= 3;
                            dayStr = text.substr(dStart, dayPos - dStart);
                        } else {
                            break;
                        }
                    }

                    int y = chineseNumToInt(yearStr);
                    int m = chineseNumToInt(monthStr);
                    int d = chineseNumToInt(dayStr);
                    if (y > 1900 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                        char buf[16];
                        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
                        return std::string(buf);
                    }
                }
            }
        }
    }

    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// 案件类型推导
// ═══════════════════════════════════════════════════════════════
std::string MetadataExtractor::deriveCaseType(const std::string& caseNumber) {
    if (caseNumber.empty()) return "";

    // 在 ）或 ) 之后查找类型字
    size_t pos = caseNumber.find(')');
    if (pos == std::string::npos) pos = caseNumber.find("\xEF\xBC\x89");  // "）"
    if (pos == std::string::npos) return "";

    for (size_t i = pos + 1; i < caseNumber.size(); ) {
        unsigned char c = static_cast<unsigned char>(caseNumber[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;

        std::string ch = caseNumber.substr(i, len);
        if      (ch == "民") return "民事";
        else if (ch == "刑") return "刑事";
        else if (ch == "行") return "行政";
        else if (ch == "知") return "知识产权";
        else if (ch == "商") return "商事";
        else if (ch == "国") return "国家赔偿";
        else if (ch == "破") return "破产";
        else if (ch == "法") return "司法协助";
        else if (ch == "执") return "执行";

        i += len;
    }

    return "";
}

// ═══════════════════════════════════════════════════════════════
// 当事人提取（纯文本匹配，避免 regex）
// ═══════════════════════════════════════════════════════════════
std::optional<std::string> MetadataExtractor::extractLitigants(const std::string& text) {
    // 找到"原告"和"被告"之间的内容
    const std::string PLANT = "\xE5\x8E\x9F\xE5\x91\x8A";  // 原告
    const std::string DEF = "\xE8\xA2\xAB\xE5\x91\x8A";    // 被告

    size_t plantPos = text.find(PLANT);
    size_t defPos = text.find(DEF, plantPos != std::string::npos ? plantPos + 6 : 0);

    if (plantPos != std::string::npos && defPos != std::string::npos &&
        defPos - plantPos < 100) {
        // 提取原告姓名（原告后 2-4 个中文字符）
        std::string plantName;
        size_t p = plantPos + 6;
        while (p < defPos && plantName.size() < 12) {
            unsigned char c = static_cast<unsigned char>(text[p]);
            if (c >= 0xE0 && c < 0xF0) {  // 中文字符
                plantName += text.substr(p, 3);
                p += 3;
            } else if (c < 0x80) {
                break;  // 遇到 ASCII 字符停止
            } else {
                break;
            }
        }

        // 提取被告姓名
        std::string defName;
        p = defPos + 6;
        while (p < text.size() && defName.size() < 12) {
            unsigned char c = static_cast<unsigned char>(text[p]);
            if (c >= 0xE0 && c < 0xF0) {
                defName += text.substr(p, 3);
                p += 3;
            } else if (c < 0x80) {
                break;
            } else {
                break;
            }
        }

        if (!plantName.empty() && !defName.empty()) {
            return "原告" + plantName + " 诉 被告" + defName;
        }
    }

    // 公诉机关模式
    const std::string PROC = "\xE5\x85\xAC\xE8\xAF\x89\xE6\x9C\xBA\xE5\x85\xB3";  // 公诉机关
    size_t procPos = text.find(PROC);
    if (procPos != std::string::npos) {
        return "公诉机关";
    }

    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// 审判程序推导
// ═══════════════════════════════════════════════════════════════
std::string MetadataExtractor::deriveProcedure(const std::string& caseNumber,
                                                const std::string& text,
                                                const std::string& court) {
    if (!caseNumber.empty()) {
        // 查找程序标识字
        static const char* PROC_CHARS[] = {
            "初", "终", "再", "申", "复", "监", "赔", "特", "破"
        };
        static const char* PROC_NAMES[] = {
            "一审", "二审（终审）", "再审", "申请再审", "复议",
            "检察监督", "赔偿", "特别程序", "破产程序"
        };

        // 迭代 UTF-8 字符
        for (size_t i = 0; i < caseNumber.size(); ) {
            unsigned char c = static_cast<unsigned char>(caseNumber[i]);
            size_t len = 1;
            if (c >= 0xF0) len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;

            std::string ch = caseNumber.substr(i, len);
            for (int j = 0; j < 9; ++j) {
                if (ch == PROC_CHARS[j]) return PROC_NAMES[j];
            }
            i += len;
        }
    }

    // 从文本内容判断
    if (text.find("二审") != std::string::npos ||
        text.find("上诉") != std::string::npos ||
        text.find("终审") != std::string::npos) {
        return "二审";
    }
    if (text.find("再审") != std::string::npos ||
        text.find("审判监督") != std::string::npos) {
        return "再审";
    }
    if (text.find("一审") != std::string::npos) {
        return "一审";
    }

    if (!court.empty() && court.find("最高") != std::string::npos) {
        return "二审/再审";
    }

    return "";
}

// ═══════════════════════════════════════════════════════════════
// 主入口
// ═══════════════════════════════════════════════════════════════
DocMetadata MetadataExtractor::extract(const std::string& text) {
    DocMetadata meta;

    auto cn = extractCaseNumber(text);
    if (cn) meta.caseNumber = *cn;

    auto ct = extractCourt(text);
    if (ct) meta.court = *ct;

    auto dt = extractDate(text);
    if (dt) meta.date = *dt;

    meta.caseType = deriveCaseType(meta.caseNumber);

    auto lit = extractLitigants(text);
    if (lit) meta.litigants = *lit;

    meta.procedure = deriveProcedure(meta.caseNumber, text, meta.court);

    return meta;
}

} // namespace document
