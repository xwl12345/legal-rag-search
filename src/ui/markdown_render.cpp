#include "ui/markdown_render.h"

#include <QScrollBar>

namespace markdown {

void renderMarkdown(QTextEdit& view, const QString& markdown) {
    // 渲染前判断是否贴底（4px 容差吸收取整误差）：
    // 在底部 → 渲染后继续跟随底部；用户上翻了 → 不拽回
    QScrollBar* bar = view.verticalScrollBar();
    const bool stickToBottom = !bar || bar->value() >= bar->maximum() - 4;

    view.document()->setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);

    if (stickToBottom && bar) {
        bar->setValue(bar->maximum());
    }
}

}  // namespace markdown
