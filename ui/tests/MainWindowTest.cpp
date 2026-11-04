#include "app/MainWindow.h"
#include "chart/KLineChartWidget.h"
#include "net/NodeClient.h"

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace tqsdk::ui;

namespace {

class FakeNodeClient final : public NodeClient {
public:
    using NodeClient::NodeClient;

    void connectToNode(const QUrl&) override {}
    void disconnectFromNode() override {}
    void setSubscription(const Subscription& subscription) override { subscription_ = subscription; }
    void requestCatalog(const CatalogQuery&) override {}
    void requestQuotes(const QStringList&) override {}
    void requestOptionChain(const QString&) override {}
    void requestKlineHistory(const QString&, qint64, int) override {}
    void requestAccountSnapshot() override {}
    void requestOrders() override {}
    void requestPositions() override {}
    void requestTrades() override {}
    void placeOrder(const OrderRequest&) override {}
    void cancelOrder(const QString&) override {}

private:
    Subscription subscription_;
};

} // namespace

class MainWindowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void chartStateAndSelectionAreRestored();
    void closingWindowPersistsChartState();
    void cleanup();

private:
    QTemporaryDir settingsDir_;
};

void MainWindowTest::initTestCase() {
    QVERIFY(settingsDir_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("TQSDK-UI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("MainWindowTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
}

void MainWindowTest::chartStateAndSelectionAreRestored() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"),
                      QStringList{QStringLiteral("SHFE.rb2701"), QStringLiteral("DCE.i2701")});
    settings.setValue(QStringLiteral("ui/chart/symbol"), QStringLiteral("DCE.i2701"));
    settings.setValue(QStringLiteral("ui/chart/interval"), QStringLiteral("1h"));
    settings.setValue(QStringLiteral("ui/chart/durationNs"), qint64(3'600'000'000'000LL));
    settings.setValue(QStringLiteral("ui/chart/visibleCount"), 40);
    settings.setValue(QStringLiteral("ui/chart/visibleStart"), 10);
    settings.setValue(QStringLiteral("ui/chart/followLatest"), false);
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(30);

    auto* chart = window.findChild<KLineChartWidget*>();
    QVERIFY(chart != nullptr);
    QCOMPARE(chart->symbol(), QStringLiteral("DCE.i2701"));
    QCOMPARE(chart->interval(), QStringLiteral("1h"));
    QCOMPARE(chart->visibleCount(), 40);
    QCOMPARE(chart->visibleStartIndex(), 10);
    QVERIFY(!chart->followLatest());
}

void MainWindowTest::closingWindowPersistsChartState() {
    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(20);

    auto* chart = window.findChild<KLineChartWidget*>();
    QVERIFY(chart != nullptr);
    chart->setViewport(66, 12, false);
    window.close();
    QSettings().sync();

    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("ui/chart/visibleCount")).toInt(), 66);
    QCOMPARE(settings.value(QStringLiteral("ui/chart/visibleStart")).toInt(), 12);
    QCOMPARE(settings.value(QStringLiteral("ui/chart/followLatest")).toBool(), false);
}

void MainWindowTest::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
