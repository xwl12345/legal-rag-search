#pragma once
#include <QDialog>

class QPlainTextEdit;

/// 片段查看弹窗（T10.1）：单击检索结果 → 看块原文，不再覆盖 AI 回答区。
///
/// 解耦约定：纯展示 + 动作转发——不持有 FavoriteStore / Retriever，
/// 「⭐ 收藏」「📄 查看全文」以信号发出，由 SearchPage 接线执行；
/// 打开方负责设置非模态 + WA_DeleteOnClose（关闭即销毁）。
///
/// 背景（用户实测反馈 2026-10-20）：原单击行为把片段灌进 aiAnswerArea_，
/// AI 回答被覆盖且找不回——本弹窗让片段与 AI 回答同屏对照成为可能。
class SnippetDialog : public QDialog {
    Q_OBJECT
public:
    /// @param docId     片段所属文书（窗口标题展示）
    /// @param chunkIndex 片段块号（窗口标题展示）
    /// @param chunkText  块原文（只读展示）
    explicit SnippetDialog(const QString& docId, int chunkIndex,
                           const QString& chunkText, QWidget* parent = nullptr);

    /// 弹窗正在展示的块内容（E2E 断言用）
    QString snippetText() const;

signals:
    /// 「⭐ 收藏到案例集…」——SearchPage 接线到既有收藏链路
    void favoriteRequested();
    /// 「📄 查看全文」——SearchPage 接线到 DocViewerDialog 打开链路
    void fullTextRequested();

private:
    QPlainTextEdit* textView_ = nullptr;
};
