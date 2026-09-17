#pragma once
#include <QFrame>

class QListWidget;

/// 左侧固定导航栏：藏青底 + 铜金选中项。
///
/// 五个页面容器的入口（检索问答 / 文档库 / 问答历史 / 算法评测 / 设置）。
class NavigationBar : public QFrame {
    Q_OBJECT

public:
    explicit NavigationBar(QWidget* parent = nullptr);

    int currentIndex() const;

signals:
    /// 用户点击导航项
    void currentIndexChanged(int index);

public slots:
    void setCurrentIndex(int index);

private:
    QListWidget* list_ = nullptr;
};
