#pragma once
#include <QString>
#include <utility>
#include <vector>

// T10 全文阅读·定位纯逻辑（与 UI 解耦，供单测直接覆盖）。
//
// 设计说明（为什么是纯函数头文件）：
//   run_tests 目标只链接 SRC_ENGINE / SRC_STORE / SRC_CONFIG，不含 UI 层。
//   把「命中块 → 全文位置」的定位逻辑收进无 Q_OBJECT 的 header-only 纯函数，
//   测试直接 include 本头即可覆盖核心换算，无需链接对话框实现；
//   DocViewerDialog 只做展示消费，不承担逻辑。

namespace doc_viewer {

/// 命中块在全文中的位置标记（QString 字符偏移，非 UTF-8 字节偏移）。
/// 索引层不存分块偏移（index_store.h 的 StoredDocument 只有块文本），
/// 因此位置由「全文 indexOf」动态求得。
struct HitMark {
    int chunkIndex = -1;  // 对应传入的块号
    int charPos = 0;      // 块文本在全文中的起始字符位置
    int charLen = 0;      // 块文本长度（字符数）
};

/// 在全文中依序定位各命中块。
///
/// 匹配单调推进：每次从上一命中位置 +1 开始查找。+1 而非「跳过上一块
/// 全长」是刻意的——相邻分块带重叠区（50 字节），下一块的真实起点通常
/// 落在上一块内部，跳过全长会漏掉唯一命中（先用中文数字九/十的教训：
/// 这类区间单调性掩盖的错，必须靠边界单测暴露）。
///
/// 找不到的块直接跳过（不出现在返回列表），不中断后续定位——
/// 全文做过规范化而块文本没同步的场景下，部分定位仍优于全军覆没。
inline std::vector<HitMark> locateHits(
    const QString& fullText,
    const std::vector<std::pair<int, QString>>& hits) {
    std::vector<HitMark> marks;
    int searchFrom = 0;
    for (const auto& hit : hits) {
        if (hit.second.isEmpty()) {
            continue;
        }
        const int pos = fullText.indexOf(hit.second, searchFrom);
        if (pos < 0) {
            continue;
        }
        marks.push_back({hit.first, pos, hit.second.length()});
        searchFrom = pos + 1;
    }
    return marks;
}

}  // namespace doc_viewer
