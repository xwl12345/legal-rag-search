#include <QApplication>
#include <QtGlobal>
#include "ui/main_window.h"

int main(int argc, char* argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Qt5：跟随系统 DPI 缩放（高分屏 125%/150% 下控件与字体保持舒适大小；
    // 100% 缩放的旧机器不受影响）。属性必须在 QApplication 构造前设置。
    // Qt6 高分屏缩放默认开启，这两个属性已弃用，无需（也不能）再设置。
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Legal RAG Search"));
    app.setApplicationVersion(QStringLiteral("2.0.0"));

    // 全局字体基准（AppTheme 的 Ctrl+滚轮缩放以此为 100%）
    QFont font = app.font();
    font.setPointSize(10);
    app.setFont(font);

    MainWindow window;
    window.show();

    return app.exec();
}
