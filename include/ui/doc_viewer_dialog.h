#pragma once
#include <QDialog>
#include <vector>

#include "ui/doc_viewer_logic.h"

class QLabel;
class QListWidget;
class QPlainTextEdit;

namespace rag {
class Retriever;
}

/// 全文阅读弹窗（T10 / M8）：整篇文书 + 命中块高亮定位。
///
/// 解耦约定：
///   - 只读消费注入的 Retriever（getFullText / getChunk），不写索引、
///     不知道任何页面存在；删除本弹窗 = 删三个文件 + CMake 两行，
///     检索链路零影响。
///   - 检索页 / 收藏页各自构造本弹窗（跨页禁止互相引用的约定不变），
///     弹窗本身不属于七页骨架，是附属动作的模态/非模态视图。
///
/// 高亮实现说明：QPlainTextEdit 的 ExtraSelections 是逐片段富文本格式，
/// QSS 无法描述（QSS 只能配控件级外观），因此高亮底色在本实现内用常量
/// 给出——不新增任何 setStyleSheet 调用（全项目唯一调用点仍是
/// AppTheme::apply()），控件其余外观走 objectName 匹配全局 QSS。
class DocViewerDialog : public QDialog {
    Q_OBJECT
public:
    /// @param retriever       只读数据源（不持有，生命周期由 MainWindow 保证）
    /// @param docId           目标文书
    /// @param hitChunkIndexes 本次检索命中的块号（内部升序去重后定位）
    explicit DocViewerDialog(rag::Retriever* retriever,
                             const QString& docId,
                             std::vector<int> hitChunkIndexes,
                             QWidget* parent = nullptr);

    /// 文书标题（窗口标题与状态栏展示，E2E 断言用）
    const QString& documentTitle() const { return title_; }

    /// 成功定位的命中块数（= 高亮条数，E2E 断言用）
    int locatedCount() const { return static_cast<int>(marks_.size()); }

private slots:
    /// 右侧命中列表选中 → 正文滚动到对应位置
    void onHitSelected();

private:
    void setupUi();
    void applyHighlights();
    void jumpToMark(const doc_viewer::HitMark& mark);

    rag::Retriever* retriever_ = nullptr;
    QString docId_;
    QString title_;
    QString fullText_;
    std::vector<doc_viewer::HitMark> marks_;

    QPlainTextEdit* textView_ = nullptr;
    QListWidget* hitList_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};
