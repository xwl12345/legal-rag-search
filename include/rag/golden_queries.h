#pragma once
#include <string>
#include <vector>

namespace rag {

/// 标注查询（qrels）：检索质量评测的黄金集（T4）。
/// 与语料（21 篇模拟裁判文书）配套：查询 → 相关文档 docId（文件名，含扩展名）。
/// 单一事实源：质量分析页批量评测、eval_retrieval 工具、单元测试共用本表，
/// 改动这里即三处同步（避免出现两份各改各的标注集）。
struct GoldenQuery {
    std::string query;
    std::vector<std::string> relevant;   // 相关文档 docId
    const char* type;                    // 查询类型：A 案情描述 / B 法律术语 / C 易混区分 / D 多相关
};

/// 23 条标注查询（人工标注，依据各文书案由与案情）
inline const std::vector<GoldenQuery>& goldenQueries() {
    static const std::vector<GoldenQuery> kGolden = {
        // ── 民事 ──
        {"民间借贷纠纷",                     {"case_civil_001_loan_dispute.txt"},      "B 法律术语"},
        {"借钱不还被起诉会有什么后果",       {"case_civil_001_loan_dispute.txt"},      "A 案情描述"},
        {"公司拖欠工资违法解除劳动合同怎么维权", {"case_civil_006_labor_dispute.txt"}, "A 案情描述"},
        {"劳动争议仲裁",                     {"case_civil_006_labor_dispute.txt"},     "B 法律术语"},
        {"夫妻感情破裂离婚孩子抚养权归属",   {"case_civil_004_divorce_case.txt"},      "A 案情描述"},
        {"法定继承遗产分配顺序",             {"case_civil_005_inheritance_dispute.txt"},"A 案情描述"},
        {"开发商逾期交房违约金 商品房预售合同", {"case_civil_007_property_dispute.txt"},"A 案情描述"},
        {"交通事故责任认定保险理赔",         {"case_civil_008_traffic_accident.txt"},  "A 案情描述"},
        {"业主在小区受伤物业管理公司责任",   {"case_civil_003_tort_dispute.txt"},      "C 易混区分"},
        {"技术服务合同纠纷违约责任",         {"case_civil_002_contract_dispute.txt"},  "B 法律术语"},
        // ── 刑事 ──
        {"醉驾血液酒精含量 危险驾驶罪",      {"case_criminal_005_drunk_driving.txt"},  "A 案情描述"},
        {"盗窃罪立案量刑标准",               {"case_criminal_002_theft.txt"},          "B 法律术语"},
        {"电信诈骗数额较大怎么判刑",         {"case_criminal_001_fraud.txt"},          "A 案情描述"},
        {"故意伤害罪附带民事诉讼赔偿",       {"case_criminal_003_assault.txt"},        "B 法律术语"},
        {"财务人员侵占公司资金",             {"case_criminal_004_embezzlement.txt"},   "C 易混区分"},
        // ── 知识产权 ──
        {"发明专利侵权损害赔偿",             {"case_ip_002_patent.txt"},               "B 法律术语"},
        {"电视剧信息网络传播权侵权",         {"case_ip_003_copyright.txt"},            "A 案情描述"},
        {"商标权纠纷",                       {"case_ip_001_trademark.txt",
                                              "case_admin_001_license_dispute.txt"},  "D 多相关"},
        // ── 商事/行政 ──
        {"股东起诉法定代表人损害公司利益",   {"case_commercial_001_company_dispute.txt"}, "A 案情描述"},
        {"资不抵债申请破产清算",             {"case_commercial_002_bankruptcy.txt"},   "A 案情描述"},
        {"对市场监督管理局行政处罚不服提起诉讼", {"case_admin_002_penalty_dispute.txt"},"A 案情描述"},
        {"行政赔偿请求",                     {"case_admin_003_compensation.txt"},      "B 法律术语"},
        {"商标无效宣告行政诉讼",             {"case_admin_001_license_dispute.txt"},   "C 易混区分"},
    };
    return kGolden;
}

} // namespace rag
