#pragma once
#include <QFrame>

class QLabel;

/// 通用占位页：页标题 + 副标题 + 一张虚线卡片写明该模块的开发阶段。
///
/// T0 阶段「文档库 / 问答历史 / 算法评测 / 设置」四页先由此占位，
/// 后续 T1–T4 逐个替换为真实实现。
class PlaceholderPage : public QFrame {
    Q_OBJECT

public:
    /// @param title     页面大标题（18px）
    /// @param subtitle  标题右侧灰色说明（11px）
    /// @param badge     计划任务号，如 "T1"
    /// @param body      卡片正文（支持纯文本，自动换行）
    PlaceholderPage(const QString& title,
                    const QString& subtitle,
                    const QString& badge,
                    const QString& body,
                    QWidget* parent = nullptr);
};
