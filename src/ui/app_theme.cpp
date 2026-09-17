#include "ui/app_theme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QRegularExpression>
#include <QStyle>

namespace {

constexpr int kBaseScale = 100;

/// 首次 apply() 时冻结的应用字体，之后所有缩放都以它为基准，
/// 避免连续缩放时误差累积。
QFont g_baseFont;
bool g_baseFontCaptured = false;

}  // namespace

QString AppTheme::loadSource() {
    QFile file(QStringLiteral(":/theme/app.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString AppTheme::scaledSource(int scalePercent) {
    const QString source = loadSource();
    if (scalePercent == kBaseScale) {
        return source;
    }

    // 只缩放显式写死的 px 字号；跟随应用字体的控件由 setFont 处理
    static const QRegularExpression re(QStringLiteral("(font-size\\s*:\\s*)(\\d+)(px)"));

    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(source);
    while (it.hasNext()) {
        const auto match = it.next();
        out += source.mid(last, match.capturedStart() - last);
        bool ok = false;
        const int value = match.captured(2).toInt(&ok);
        const int scaled = ok ? qMax(8, value * scalePercent / 100) : value;
        out += match.captured(1) + QString::number(scaled) + QStringLiteral("px");
        last = match.capturedEnd();
    }
    out += source.mid(last);
    return out;
}

void AppTheme::apply(int scalePercent) {
    if (!g_baseFontCaptured) {
        g_baseFont = QApplication::font();
        g_baseFontCaptured = true;
    }

    QFont font = g_baseFont;
    font.setPointSizeF(g_baseFont.pointSizeF() * scalePercent / 100.0);
    QApplication::setFont(font);

    // ── 全项目唯一的 setStyleSheet 调用点 ──
    qApp->setStyleSheet(scaledSource(scalePercent));
}

void AppTheme::setStatusFlag(QLabel* label, bool ok) {
    if (!label) {
        return;
    }
    label->setProperty("status", ok ? "ok" : "error");
    if (label->style()) {
        label->style()->polish(label);
    }
    label->update();
}
