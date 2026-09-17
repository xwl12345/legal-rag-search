#include "ui/navigation_bar.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace {

/// 导航项：图标 + 文案。数组顺序即 QStackedWidget 的页序。
struct NavItem {
    QString icon;
    QString text;
};

const NavItem kNavItems[] = {
    { QStringLiteral("🔍"), QStringLiteral("检索问答") },
    { QStringLiteral("📚"), QStringLiteral("文档库") },
    { QStringLiteral("🕘"), QStringLiteral("问答历史") },
    { QStringLiteral("⚖️"), QStringLiteral("算法评测") },
    { QStringLiteral("⚙️"), QStringLiteral("设置") },
};

constexpr int kNavItemCount = static_cast<int>(sizeof(kNavItems) / sizeof(kNavItems[0]));

}  // namespace

NavigationBar::NavigationBar(QWidget* parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("navPanel"));
    setFixedWidth(190);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 16, 14, 14);
    layout->setSpacing(0);

    auto* brand = new QLabel(QStringLiteral("法律文档检索系统"), this);
    brand->setObjectName(QStringLiteral("navBrand"));
    layout->addWidget(brand);

    auto* brandSub = new QLabel(QStringLiteral("LEGAL RAG · v2.0"), this);
    brandSub->setObjectName(QStringLiteral("navBrandSub"));
    layout->addWidget(brandSub);

    layout->addSpacing(14);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("navList"));
    list_->setSpacing(0);
    // 导航是纯点击容器，去掉选中项的焦点框
    list_->setFocusPolicy(Qt::NoFocus);
    for (int i = 0; i < kNavItemCount; ++i) {
        auto* item = new QListWidgetItem(kNavItems[i].icon + QStringLiteral("  ")
                                         + kNavItems[i].text, list_);
        item->setSizeHint(QSize(0, 42));
        list_->addItem(item);
    }
    layout->addWidget(list_, 1);

    layout->addSpacing(10);

    auto* footer = new QLabel(QStringLiteral("Qt 5.15.2 · MinGW\n离线检索引擎"), this);
    footer->setObjectName(QStringLiteral("navFooter"));
    layout->addWidget(footer);

    connect(list_, &QListWidget::currentRowChanged,
            this, &NavigationBar::currentIndexChanged);
}

int NavigationBar::currentIndex() const {
    return list_ ? list_->currentRow() : -1;
}

void NavigationBar::setCurrentIndex(int index) {
    if (!list_) {
        return;
    }
    list_->setCurrentRow(index);
}
