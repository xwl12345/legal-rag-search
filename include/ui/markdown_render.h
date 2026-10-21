#pragma once
#include <QString>
#include <QTextEdit>

namespace markdown {

/// T13：把 Markdown 文本渲染成富文本（Typora 式排版），零第三方依赖。
///
/// 用 Qt 内置 QTextDocument::setMarkdown（GitHub 方言：标题分级/粗斜体/
/// 列表/代码块/表格/链接）。不支持 LaTeX 数学公式——$...$ 按纯文本显示，
/// 法律问答场景基本不涉及，如实标注为已知限制。
///
/// 流式语义：调用方每收到增量就以「当前累计全文」调本函数（全量重渲染，
/// 回答为 KB 级文本，重排开销可忽略）；滚动位置若在底部则继续跟随底部
/// （生成中自动下滚），用户已上翻查看时不拽回。
///
/// 解耦约定：本函数无状态、不持有页面指针；检索页与历史页共用，
/// 不得在函数内引用任何业务对象。

/// 把 markdown 渲染进只读文本视图（替换全部内容，保留滚动跟随语义）
void renderMarkdown(QTextEdit& view, const QString& markdown);

}  // namespace markdown
