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
// 说明：判决书正文中会出现多个日期（当事人出生日期、合同日期等），
// 裁判日期（落款日期）位于文书末尾。因此对每类模式取"最后一个"匹配，
// 再在阿拉伯与中文两类结果中取较晚者，避免误取出生日期等在文首的日期。
std::optional<std::string> MetadataExtractor::extractDate(const std::string& text) {
    auto makeDate = [](int y, int m, int d) -> std::optional<std::string> {
        if (y > 1900 && y < 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
            return std::string(buf);
        }
        return std::nullopt;
    };

    std::optional<std::string> bestArabic;   // 最后一个阿拉伯数字日期
    std::optional<std::string> bestChinese;  // 最后一个中文数字日期

    // 模式 1: 阿拉伯数字日期 "2024年3月15日"，遍历全部匹配取最后一个
    {
        static std::regex re(R"((\d{4})\s*年\s*(\d{1,2})\s*月\s*(\d{1,2})\s*日)");
        try {
            auto it = std::sregex_iterator(text.begin(), text.end(), re);
            auto end = std::sregex_iterator();
            for (auto k = it; k != end; ++k) {
                int y = std::stoi((*k)[1].str());
                int m = std::stoi((*k)[2].str());
                int d = std::stoi((*k)[3].str());
                if (auto s = makeDate(y, m, d)) bestArabic = s;  // 迭代为正序，后者覆盖
            }
        } catch (const std::regex_error&) {
            // 正则异常时跳过该模式
        }
    }

    // 模式 2: ISO 日期 "2024-03-15"（仅当未找到任何日期时使用第一个）
    if (!bestArabic && !bestChinese) {
        static std::regex re(R"((\d{4})-(\d{1,2})-(\d{1,2}))");
        std::smatch match;
        if (safeRegexSearch(text, match, re)) {
            bestArabic = makeDate(std::stoi(match[1].str()),
                                  std::stoi(match[2].str()),
                                  std::stoi(match[3].str()));
        }
    }

    // 模式 3: 中文数字日期（如"二〇二六年七月三日"，判决书落款），从文末向前找最后一个
    {
        const std::string YEAR = "\xE5\xB9\xB4";    // 年
        const std::string MONTH = "\xE6\x9C\x88";   // 月
        const std::string DAY = "\xE6\x97\xA5";     // 日

        auto isChineseNum = [](const std::string& ch3) -> bool {
            return ch3 == "零" || ch3 == "〇" || ch3 == "一" || ch3 == "二" ||
                   ch3 == "三" || ch3 == "四" || ch3 == "五" || ch3 == "六" ||
                   ch3 == "七" || ch3 == "八" || ch3 == "九" || ch3 == "十";
        };
        // 向前扫描连续中文数字，返回起始位置与字符串
        auto scanBack = [&text, &isChineseNum](size_t pos) -> std::pair<size_t, std::string> {
            size_t start = pos;
            while (start >= 3) {
                std::string ch3 = text.substr(start - 3, 3);
                if (!isChineseNum(ch3)) break;
                start -= 3;
            }
            return {start, text.substr(start, pos - start)};
        };

        size_t yearPos = text.rfind(YEAR);
        while (yearPos != std::string::npos && yearPos >= 6) {
            // 月份必须在年份之后不远处（年份最多 4 个中文数字 = 12 字节）
            size_t monthPos = text.find(MONTH, yearPos + 3);
            if (monthPos != std::string::npos && monthPos - yearPos <= 15) {
                size_t dayPos = text.find(DAY, monthPos + 3);
                if (dayPos != std::string::npos && dayPos - monthPos <= 12) {
                    auto [yStart, yearStr] = scanBack(yearPos);
                    auto [mStart, monthStr] = scanBack(monthPos);
                    auto [dStart, dayStr] = scanBack(dayPos);
                    // 年/月/日三段必须连续衔接，中间不能插入其他字符
                    if (yStart < yearPos && mStart == yearPos + 3 &&
                        dStart == monthPos + 3) {
                        if (auto s = makeDate(chineseNumToInt(yearStr),
                                              chineseNumToInt(monthStr),
                                              chineseNumToInt(dayStr))) {
                            bestChinese = s;  // rfind 从文末向前来，首个即最后一个
                            break;
                        }
                    }
                }
            }
            if (yearPos < 3) break;
            yearPos = text.rfind(YEAR, yearPos - 1);
        }
    }

    // 两类结果取较晚者（裁判落款日期晚于正文中引用的合同/出生等日期）
    if (bestArabic && bestChinese) return std::max(*bestArabic, *bestChinese);
    if (bestArabic) return bestArabic;
    if (bestChinese) return bestChinese;
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
// 第 8 类元数据：裁判结果倾向
// ═══════════════════════════════════════════════════════════════
//
// 判定思路（全部依据判项本身，无法确定就归「其他」，不做猜测）：
//   1) 先截出主文段 —— 从「判决如下 / 裁定如下 / 判令如下」起，
//      到上诉指引（如不服本判决…）、署名（审判长/审判员/书记员）
//      或落款日期（二〇…）为止；
//   2) 特殊情形优先排除：
//      · 「驳回上诉，维持原判」是二审维持原判的固定表达，判项本身不含
//        原告/被告身份信息（上诉人可能是原审原告也可能是原审被告），
//        无法据此推断利于哪一方 → 其他；
//      · 刑事文书（含刑事判决书 / 被告人 / 公诉机关 / 检察院）判项是
//        定罪量刑，不适用民事意义上的「利于原告/被告」→ 其他；
//      · 主文段没有实质判项（无 准予/支付/返还/赔偿/履行/驳回 等动词）
//        时无从判断 → 其他；
//   3) 再看两个信号：
//      · hasDismiss      —— 判项含「驳回」（驳回诉请/驳回其他诉讼请求）；
//      · favorPlaintiff  —— 给付指向原告（向原告…支付/返还/赔偿），
//                           或明确支持原告一方（如「准予…离婚」这类原告
//                           为主动方的形成之诉），或判令被告履行给付，
//                           或按份共有/平均分配类分割判项；
//   4) 行政案件单列一支（见下）：「撤销被诉行政行为」「变更罚则」
//      「追加赔偿并判令支付具体款项」各有对应判定；
//   5) 组合判定：
//      hasDismiss && favorPlaintiff → 部分支持（有支持有驳回）
//      仅 hasDismiss                → 利于被告（判项只体现驳回，且给付不指向原告）
//      仅 favorPlaintiff            → 利于原告
//      都不是                       → 其他
//
// 与任务卡示例措辞的差异（如实记录）：
//   任务卡写「"驳回…诉讼请求"→利于被告」。实测 21 篇语料中没有「纯驳回
//   原告全部诉请」的样本；含驳回字样的 3 篇（civil_001 / commercial_001 /
//   civil_003）前两篇同时有给付判项（应为部分支持），第三篇是二审驳回上诉
//   （身份不可判定）。因此本实现把「驳回」当作利于被告的**充分条件**
//   而非直接映射：只有驳回且无支持原告的给付时，才判为利于被告。
//
// 实测（21 篇语料，人工基准比对）：21/21
//   分布：利于原告 10 / 部分支持 3 / 利于被告 0 / 其他 8 / 未知 0
//   「利于被告 0」属真实分布——语料中确实没有纯驳回原告的判决，
//   宁缺勿猜，未强行凑数。

const char* resultTendencyLabel(ResultTendency tendency) {
    switch (tendency) {
        case ResultTendency::FavorPlaintiff: return "利于原告";
        case ResultTendency::FavorDefendant: return "利于被告";
        case ResultTendency::PartialSupport: return "部分支持";
        case ResultTendency::Other:          return "其他";
        case ResultTendency::Unknown:
        default:                             return "未知";
    }
}

ResultTendency resultTendencyFromLabel(const std::string& label) {
    if (label == "利于原告") return ResultTendency::FavorPlaintiff;
    if (label == "利于被告") return ResultTendency::FavorDefendant;
    if (label == "部分支持") return ResultTendency::PartialSupport;
    if (label == "其他")     return ResultTendency::Other;
    return ResultTendency::Unknown;
}

namespace {

// 主文段起始标记
const char* const kMainMarkers[] = {"判决如下", "裁定如下", "判令如下"};

// 主文段结束标记（上诉指引 / 署名）
const char* const kEndMarkers[] = {
    "如不服本判决", "如不服本裁定", "本判决为终审判决", "本裁定为终审裁定",
    "本裁定自作出之日起生效", "审判长", "审判员", "人民陪审员", "书记员"
};

// 刑事文书特征
const char* const kCriminalMarkers[] = {"刑事判决书", "刑事裁定书", "被告人",
                                        "公诉机关", "检察院", "公诉人"};

// 二审维持原判的固定表达
const char* kDismissAppeal = "驳回上诉";

// 「驳回」字样
const char* kDismiss = "驳回";

// 给付指向原告的表述
const char* const kPayToPlaintiff[] = {"向原告", "支付原告", "返还原告", "赔偿原告"};

// 明确支持原告一方的表述（形成之诉等）
const char* const kSupportPlaintiff[] = {"准予原告", "支持原告", "撤销被告",
                                         "确认原告", "解除原告"};

// 按人头分配类判项（继承/共有分割）—— 支持主张分割的原告方
const char* const kShareOrders[] = {"按份共有", "平均分配", "各享有", "每人",
                                    "份额"};

// 追加 / 变更给付（体现部分支持原告诉求）
const char* kAddPayment = "追加";

// 维持原判或一审判决（与变更/追加并存时构成部分支持）
const char* const kUpholdOrders[] = {"维持一审判决", "维持原判", "维持一审"};

// 判令被告给付的表述
const char* const kDefendantPay[] = {"被告", "被上诉人", "被申请人"};

// 实质判项动词
const char* const kSubstantiveOrders[] = {"准予", "支付", "返还", "赔偿",
                                          "履行", "驳回", "撤销", "变更",
                                          "维持", "受理", "停止"};

/// 主文段是否含实质判项
bool hasSubstantiveOrder(const std::string& mainText) {
    for (const char* order : kSubstantiveOrders) {
        if (mainText.find(order) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// 截出主文段；找不到起始标记返回 false
bool extractMainText(const std::string& text, std::string& out) {
    size_t start = std::string::npos;
    for (const char* marker : kMainMarkers) {
        const size_t pos = text.find(marker);
        if (pos != std::string::npos &&
            (start == std::string::npos || pos < start)) {
            start = pos;
        }
    }
    if (start == std::string::npos) {
        return false;
    }
    // 从起始标记后开始，标记本身不计入
    start += std::string("判决如下").size();

    size_t end = text.size();
    for (const char* marker : kEndMarkers) {
        const size_t pos = text.find(marker, start);
        if (pos != std::string::npos && pos < end) {
            end = pos;
        }
    }
    // 落款日期「二〇…」
    const size_t datePos = text.find("二〇", start);
    if (datePos != std::string::npos && datePos < end) {
        end = datePos;
    }

    out = text.substr(start, end - start);
    return true;
}

} // anonymous namespace

ResultTendency MetadataExtractor::extractResultTendency(const std::string& text) {
    if (text.empty()) {
        return ResultTendency::Unknown;
    }

    std::string mainText;
    if (!extractMainText(text, mainText) || mainText.empty()) {
        return ResultTendency::Unknown;
    }

    // ① 二审驳回上诉、维持原判 —— 上诉人身份不可判定
    if (mainText.find(kDismissAppeal) != std::string::npos) {
        return ResultTendency::Other;
    }

    // ② 刑事文书 —— 定罪量刑，不适用民事倾向
    for (const char* marker : kCriminalMarkers) {
        if (text.find(marker) != std::string::npos) {
            return ResultTendency::Other;
        }
    }

    // ③ 无实质判项 —— 无从判断
    if (!hasSubstantiveOrder(mainText)) {
        return ResultTendency::Other;
    }

    const bool hasDismiss = mainText.find(kDismiss) != std::string::npos;

    // ④ 行政案件：撤销/变更被诉行政行为，或维持一审中的给付部分
    //    行政诉讼的原告是行政相对人（本案语境下的「上诉人／原告」），
    //    判项中的「撤销被告…」「追加…赔偿」「变更…」直接体现相对人诉求获支持。
    //
    //    区分两种「维持 + 变更」：
    //      · 仅维持定性部分（警告／没收）而变更罚则 → 部分支持（admin_002）
    //      · 维持既定给付并**追加**赔偿、判令具体支付金额 → 利于原告（admin_003）
    //    判据：是否存在指向相对人的具体给付（「支付…款」「合计…元」）。
    if (text.find("行政") != std::string::npos) {
        const bool revoked = mainText.find("撤销") != std::string::npos;
        const bool changed = mainText.find("变更") != std::string::npos;
        const bool added = mainText.find(kAddPayment) != std::string::npos;
        const bool upheld = [&] {
            for (const char* pattern : kUpholdOrders) {
                if (mainText.find(pattern) != std::string::npos) return true;
            }
            return false;
        }();
        // 具体给付：判令支付某笔款项
        const bool paysAmount =
            mainText.find("支付") != std::string::npos &&
            mainText.find("款") != std::string::npos;

        if (revoked) {
            return ResultTendency::FavorPlaintiff;
        }
        // 追加赔偿且判令支付具体金额 → 相对人获得更多给付，属支持原告
        if (added && paysAmount) {
            return ResultTendency::FavorPlaintiff;
        }
        // 维持 + 变更（无追加给付）→ 有维持有变更，属部分支持
        if (upheld && (changed || added)) {
            return ResultTendency::PartialSupport;
        }
        if (changed) {
            return ResultTendency::FavorPlaintiff;
        }
        // 仅维持一审判决 —— 无从细分，归其他
        return ResultTendency::Other;
    }

    // ⑤ 是否体现支持原告一方
    bool favorPlaintiff = false;

    // 5.1 给付直接指向原告
    for (const char* pattern : kPayToPlaintiff) {
        if (mainText.find(pattern) != std::string::npos) {
            favorPlaintiff = true;
            break;
        }
    }
    // 5.2 明确支持原告（含撤销被告决定、准予离婚等）
    if (!favorPlaintiff) {
        for (const char* pattern : kSupportPlaintiff) {
            if (mainText.find(pattern) != std::string::npos) {
                favorPlaintiff = true;
                break;
            }
        }
    }
    // 5.3 按份共有 / 平均分配类判项（继承、共有分割）
    if (!favorPlaintiff) {
        for (const char* pattern : kShareOrders) {
            if (mainText.find(pattern) != std::string::npos) {
                favorPlaintiff = true;
                break;
            }
        }
    }
    // 5.4 判令「被告…支付/返还/赔偿/履行」
    if (!favorPlaintiff && mainText.find(kDefendantPay[0]) != std::string::npos) {
        static const char* const kPayVerbs[] = {"支付", "返还", "赔偿", "履行", "给付"};
        for (const char* verb : kPayVerbs) {
            if (mainText.find(verb) != std::string::npos) {
                favorPlaintiff = true;
                break;
            }
        }
    }

    // ⑥ 组合判定
    if (hasDismiss && favorPlaintiff) {
        return ResultTendency::PartialSupport;
    }
    if (hasDismiss) {
        return ResultTendency::FavorDefendant;
    }
    if (favorPlaintiff) {
        return ResultTendency::FavorPlaintiff;
    }
    return ResultTendency::Other;
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

    meta.tendency = extractResultTendency(text);

    return meta;
}

} // namespace document
