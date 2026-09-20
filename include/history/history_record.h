#pragma once
#include <QMetaType>
#include <string>
#include <vector>

namespace history {

/// 一条问答记录里引用的命中来源。
///
/// 只存「足以回溯」的最小集合（文档 + 块号 + 得分 + 片段预览），
/// 刻意不存整段原文 —— 原文始终以索引为准，避免两处副本不一致。
struct SourceItem {
    std::string docId;
    int chunkIndex = 0;
    double finalScore = 0.0;
    std::string snippet;   // 片段预览（换行已折叠为空格）
};

/// 一次问答回合的完整记录（对应 history 表一行）。
///
/// interrupted 用于区分「正常收尾」与「异常终止」：
/// 见开发工作计划 T2 的产能决策 —— 只要本次回合已经产出可见内容，
/// 哪怕生成中途失败也如实落库并标记，绝不让复盘时凭空消失；
/// 唯一例外是「一个字都没吐出来」的回合（不产生任何信息，不入库）。
struct HistoryRecord {
    long long id = 0;              // 主键（0 = 尚未入库）
    std::string createdAt;         // yyyy-MM-dd HH:mm:ss
    std::string query;             // 用户问题
    std::string answer;            // 界面上最终呈现的回答原文（含失败提示）
    std::vector<SourceItem> sources;
    bool interrupted = false;      // 回答异常终止（网络中断 / 生成失败…）
    std::string note;              // 终止原因短语（可空）

    /// 命中块数（等于 sources.size()，取签名表达业务语义）
    int hitCount() const { return static_cast<int>(sources.size()); }
};

}  // namespace history

Q_DECLARE_METATYPE(history::HistoryRecord)
