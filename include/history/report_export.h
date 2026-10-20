#pragma once
#include <QString>
#include <string>

#include "history/history_record.h"

namespace history {

/// T12 检索报告导出（核心层纯函数，零 UI 依赖）。
///
/// 报告结构（任务卡 T12）：标题 + 检索问题 + 命中文书表格（兼作来源清单）
/// + 命中段落节选（分块引用）+ AI 回答。
///
/// ⚠ 内容边界（诚实标注，见需求确认单 2026-10-20）：
///   历史记录只存 docId/块号/得分/片段快照（160 字符截断），不存案号/法院/日期
///   等元数据 —— 报告如实呈现这些字段，节选处明确标注「节选」，
///   不伪装成完整原文；中断回合在报告头部如实标注，不伪装成完整回答。
///
/// 解耦约定（同 history_store.h）：本层不得 include 任何 ui/ 头；
/// 页面对它单向调用，MainWindow 是唯一持有者。

/// 单条记录 → 报告 HTML 文本（供 QTextDocument 渲染，纯函数可单测）。
std::string toReportHtml(const HistoryRecord& record);

/// 报告 HTML → ODT 文件（Qt 内置 QTextDocumentWriter，ODF 1.0 规范容器）。
/// 失败返回 false 并把原因写进 errorOut。
bool writeOdtFile(const QString& path, const std::string& reportHtml,
                  QString* errorOut = nullptr);

/// 报告 HTML → PDF 文件（QPrinter PdfFormat 渲染）。
/// 失败返回 false 并把原因写进 errorOut。
bool writePdfFile(const QString& path, const std::string& reportHtml,
                  QString* errorOut = nullptr);

}  // namespace history
