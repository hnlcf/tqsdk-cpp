#include "app/MainWindow.h"

#include <QApplication>
#include <QFont>
#include <QStyleFactory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("TQSDK Terminal");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("TQSDK");

    // Fusion keeps the layout consistent across Linux/Windows/macOS; the
    // product theme is applied later via qss/palette.
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QFont font = app.font();
    font.setPointSizeF(10.0);
    app.setFont(font);

    MainWindow window;
    window.show();
    return app.exec();
}
