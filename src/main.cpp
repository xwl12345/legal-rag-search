#include <QApplication>
#include "ui/main_window.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("RAG Search Engine");
    app.setApplicationVersion("1.0.0");

    // 全局字体设置
    QFont font = app.font();
    font.setPointSize(10);
    app.setFont(font);

    MainWindow window;
    window.show();

    return app.exec();
}
