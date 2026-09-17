#pragma once
#include <QString>

class QLabel;

/// 全局视觉主题入口（藏青 #14213D + 铜金 #B7791F）。
///
/// 职责单一：从 Qt 资源读取 src/ui/app.qss，按界面缩放比例重算 px 字号后
/// 应用到整个应用。全项目唯一的 setStyleSheet 调用就在本模块的 apply() 中，
/// 其余界面代码一律用 objectName / 动态属性（role、status）去 QSS 里匹配。
class AppTheme {
public:
    /// 资源内的 QSS 原文
    static QString loadSource();

    /// 按缩放百分比生成样式表（100 = 原始字号）
    static QString scaledSource(int scalePercent);

    /// 应用全局样式 + 应用字体。scalePercent 取值 60–250
    static void apply(int scalePercent);

    /// 给状态标签打 ok / error 标记，实际配色由 QSS 的属性选择器负责
    static void setStatusFlag(QLabel* label, bool ok);
};
