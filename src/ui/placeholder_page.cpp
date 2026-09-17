#include "ui/placeholder_page.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

PlaceholderPage::PlaceholderPage(const QString& title,
                                 const QString& subtitle,
                                 const QString& badge,
                                 const QString& body,
                                 QWidget* parent)
    : QFrame(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 16, 22, 16);
    root->setSpacing(12);

    // ── 页头 ──
    auto* head = new QHBoxLayout();
    head->setSpacing(12);

    auto* titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    head->addWidget(titleLabel);

    auto* subtitleLabel = new QLabel(subtitle, this);
    subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    head->addWidget(subtitleLabel);

    head->addStretch();
    root->addLayout(head);

    // ── 占位卡片 ──
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("placeholderCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(28, 34, 28, 34);
    cardLayout->setSpacing(14);

    auto* badgeRow = new QHBoxLayout();
    badgeRow->setSpacing(0);
    auto* badgeLabel = new QLabel(QStringLiteral("计划任务 %1 · 开发中").arg(badge), card);
    badgeLabel->setObjectName(QStringLiteral("placeholderBadge"));
    badgeLabel->setAlignment(Qt::AlignCenter);
    badgeRow->addWidget(badgeLabel);
    badgeRow->addStretch();
    cardLayout->addLayout(badgeRow);

    auto* bodyLabel = new QLabel(body, card);
    bodyLabel->setObjectName(QStringLiteral("placeholderBody"));
    bodyLabel->setWordWrap(true);
    cardLayout->addWidget(bodyLabel);

    cardLayout->addStretch();
    root->addWidget(card, 1);
}
