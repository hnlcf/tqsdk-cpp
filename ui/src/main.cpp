#include "app/MainWindow.h"
#include "net/HttpWsNodeClient.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFont>
#include <QSettings>
#include <QStyleFactory>
#include <QUrl>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("TQSDK Terminal");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("TQSDK");

    QCommandLineParser parser;
    parser.setApplicationDescription("TQSDK Qt desktop terminal");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption nodeUrlOption(
        {"n", "node-url"},
        "tqsdk-node base URL",
        "url",
        qEnvironmentVariable("TQSDK_NODE_URL", "http://127.0.0.1:18081"));
    parser.addOption(nodeUrlOption);
    parser.process(app);

    // Fusion keeps the layout consistent across Linux/Windows/macOS; the
    // product theme is applied later via qss/palette.
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    QFont font = app.font();
    font.setPointSizeF(10.0);
    app.setFont(font);

    tqsdk::ui::HttpWsNodeClient nodeClient;
    tqsdk::ui::MainWindow       window(&nodeClient);
    QSettings                   settings;
    if (settings.contains(QStringLiteral("ui/windowGeometry"))) {
        window.show();
    } else {
        window.showMaximized();
    }
    nodeClient.connectToNode(QUrl(parser.value(nodeUrlOption)));
    return app.exec();
}
