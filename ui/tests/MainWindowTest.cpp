#include "app/MainWindow.h"
#include "app/SymbolController.h"
#include "chart/KLineChartWidget.h"
#include "model/InstrumentTableModel.h"
#include "model/QuoteTableModel.h"
#include "net/NodeClient.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QtTest>

#include <algorithm>

using namespace tqsdk::ui;

namespace {

class FakeNodeClient final : public NodeClient {
public:
    using NodeClient::NodeClient;

    void connectToNode(const QUrl&) override {}
    void disconnectFromNode() override {}
    void setSubscription(const Subscription& subscription) override {
        subscription_ = subscription;
    }
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

    void publishQuotes(const QVector<Quote>& quotes) { emit quotesUpdated(quotes); }
    void publishCatalog(const QVector<Instrument>& instruments) { emit catalogLoaded(instruments); }
    void publishKline(const QString& symbol, qint64 durationNs, const KlineFrame& frame) {
        emit klineUpdated(symbol, durationNs, frame);
    }
    void publishOrderAck(const OrderAck& ack) { emit orderAckReceived(ack); }

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
    void liveQuoteSurvivesCatalogFallback();
    void latestKlineIsBuffered();
    void eventLoopRemainsResponsiveUnderBurst();
    void orderFailureShowsToast();
    void watchlistCanRemoveAndReorder();
    void perSymbolChartStateIsRestored();
    void indicatorsRenderThroughSidecar();
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

void MainWindowTest::liveQuoteSurvivesCatalogFallback() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"), QStringList{QStringLiteral("CZCE.SA701")});
    settings.setValue(QStringLiteral("ui/chart/symbol"), QStringLiteral("CZCE.SA701"));
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(30);

    Quote live;
    live.instrumentId = QStringLiteral("CZCE.SA701");
    live.lastPrice    = 990.0;
    live.bidPrice1    = 990.0;
    live.askPrice1    = 991.0;
    live.datetime     = 1'793'770'642'000'000'000LL;
    client.publishQuotes({live});
    QTest::qWait(30);

    auto* last = window.findChild<QLabel*>(QStringLiteral("quoteLast"));
    QVERIFY(last != nullptr);
    QCOMPARE(last->text(), QStringLiteral("990.00"));

    Instrument stale;
    stale.instrumentId = QStringLiteral("CZCE.SA701");
    stale.lastPrice    = 968.0;
    client.publishCatalog({stale});
    QTest::qWait(30);

    QCOMPARE(last->text(), QStringLiteral("990.00"));
}

void MainWindowTest::latestKlineIsBuffered() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"), QStringList{QStringLiteral("CZCE.SA701")});
    settings.setValue(QStringLiteral("ui/chart/symbol"), QStringLiteral("CZCE.SA701"));
    settings.setValue(QStringLiteral("ui/chart/interval"), QStringLiteral("1m"));
    settings.setValue(QStringLiteral("ui/chart/durationNs"), qint64(60'000'000'000LL));
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(30);

    auto* chart = window.findChild<KLineChartWidget*>();
    QVERIFY(chart != nullptr);
    QCOMPARE(chart->barCount(), 0);

    KlineFrame frame;
    frame.reset = true;
    KlineBar bar;
    bar.datetime = 1'793'770'620'000'000'000LL;
    bar.open = bar.high = bar.low = bar.close = 990.0;
    bar.volume                                = 1;
    frame.bars.push_back(bar);
    client.publishKline(QStringLiteral("CZCE.SA701"), 60'000'000'000LL, frame);

    QTest::qWait(100);
    QCOMPARE(chart->barCount(), 0);
    QTest::qWait(550);
    QCOMPARE(chart->barCount(), 1);
}

void MainWindowTest::eventLoopRemainsResponsiveUnderBurst() {
    QStringList symbols;
    for (int i = 0; i < 16; ++i) {
        symbols.push_back(QStringLiteral("SHFE.rb%1").arg(2701 + i));
    }
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"), symbols);
    settings.setValue(QStringLiteral("ui/chart/symbol"), symbols.front());
    settings.setValue(QStringLiteral("ui/chart/durationNs"), qint64(60'000'000'000LL));
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.resize(3840, 2160);
    window.show();
    QTest::qWait(30);

    auto* watch = window.findChild<QTableView*>(QStringLiteral("watchTable"));
    auto* chart = window.findChild<KLineChartWidget*>();
    QVERIFY(watch != nullptr);
    QVERIFY(chart != nullptr);

    QElapsedTimer loopClock;
    loopClock.start();
    qint64 lastProbe = loopClock.elapsed();
    qint64 maxGap    = 0;
    QTimer probe;
    probe.setInterval(10);
    connect(&probe, &QTimer::timeout, &probe, [&] {
        const qint64 now = loopClock.elapsed();
        maxGap           = std::max(maxGap, now - lastProbe - 10);
        lastProbe        = now;
    });
    probe.start();

    int    row = 0;
    QTimer clicks;
    clicks.setInterval(70);
    connect(&clicks, &QTimer::timeout, &clicks, [&] {
        if (!symbols.isEmpty()) {
            watch->selectRow(row % symbols.size());
            ++row;
        }
    });
    clicks.start();

    QTimer quotes;
    quotes.setInterval(10);
    int tick = 0;
    connect(&quotes, &QTimer::timeout, &quotes, [&] {
        QVector<Quote> batch;
        batch.reserve(symbols.size());
        for (const auto& symbol : symbols) {
            Quote quote;
            quote.instrumentId  = symbol;
            quote.lastPrice     = 3000.0 + (tick % 20);
            quote.bidPrice1     = quote.lastPrice - 1.0;
            quote.askPrice1     = quote.lastPrice + 1.0;
            quote.preSettlement = 2990.0;
            quote.volume        = 1000 + tick;
            quote.datetime      = 1'793'770'642'000'000'000LL + tick * 1'000'000;
            batch.push_back(quote);
        }
        ++tick;
        client.publishQuotes(batch);
    });
    quotes.start();

    QTimer mouse;
    mouse.setInterval(16);
    int mouseTick = 0;
    connect(&mouse, &QTimer::timeout, &mouse, [&, chart] {
        const int x = 120 + (mouseTick * 13) % std::max(200, chart->width() - 240);
        const int y = 140 + (mouseTick * 7) % std::max(160, chart->height() - 280);
        QTest::mouseMove(chart, QPoint(x, y));
        ++mouseTick;
    });
    mouse.start();

    QTest::qWait(2200);
    mouse.stop();
    quotes.stop();
    clicks.stop();
    probe.stop();

    QVERIFY2(maxGap < 500, qPrintable(QStringLiteral("max event loop gap: %1 ms").arg(maxGap)));
}

void MainWindowTest::orderFailureShowsToast() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/chart/symbol"), QStringLiteral("SHFE.rb2701"));
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(30);

    OrderAck ack;
    ack.success = false;
    ack.orderId = QStringLiteral("r1");
    ack.error   = QStringLiteral("CTP 31: 平仓量超过持仓量");
    client.publishOrderAck(ack);
    QTest::qWait(30);

    auto* toast = window.findChild<QLabel*>(QStringLiteral("toastMessage"));
    QVERIFY(toast != nullptr);
    QVERIFY(toast->isVisible());
    QVERIFY(toast->text().contains(QStringLiteral("CTP 31")));
}

void MainWindowTest::watchlistCanRemoveAndReorder() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"),
                      QStringList{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    settings.sync();

    QuoteTableModel      watch;
    InstrumentTableModel futures;
    InstrumentTableModel calls;
    InstrumentTableModel puts;
    SymbolController     controller(nullptr, &watch, &futures, &calls, &puts, nullptr);
    controller.loadSettings();
    QCOMPARE(controller.watchlist(), (QStringList{"A", "B", "C"}));

    QVERIFY(controller.moveWatchlistSymbol(QStringLiteral("C"), -1));
    QCOMPARE(controller.watchlist(), (QStringList{"A", "C", "B"}));
    QVERIFY(controller.removeFromWatchlist(QStringLiteral("C")));
    QCOMPARE(controller.watchlist(), (QStringList{"A", "B"}));
}

void MainWindowTest::perSymbolChartStateIsRestored() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"),
                      QStringList{QStringLiteral("A"), QStringLiteral("B")});
    settings.setValue(QStringLiteral("ui/chart/symbol"), QStringLiteral("A"));
    settings.setValue(QStringLiteral("ui/chart/interval"), QStringLiteral("1d"));
    settings.setValue(QStringLiteral("ui/chart/durationNs"), qint64(86'400'000'000'000LL));
    const QString prefix = QStringLiteral("ui/chart/symbols/%1/")
                               .arg(QString::fromLatin1(QUrl::toPercentEncoding("B")));
    settings.setValue(prefix + QStringLiteral("durationNs"), qint64(60'000'000'000LL));
    settings.setValue(prefix + QStringLiteral("interval"), QStringLiteral("1m"));
    settings.setValue(prefix + QStringLiteral("visibleCount"), 40);
    settings.setValue(prefix + QStringLiteral("visibleStart"), 5);
    settings.setValue(prefix + QStringLiteral("followLatest"), false);
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(30);
    auto* watch = window.findChild<QTableView*>(QStringLiteral("watchTable"));
    auto* chart = window.findChild<KLineChartWidget*>();
    QVERIFY(watch != nullptr);
    QVERIFY(chart != nullptr);
    watch->selectRow(1);
    QTest::qWait(250);

    QCOMPARE(chart->interval(), QStringLiteral("1m"));
    QCOMPARE(chart->visibleCount(), 40);
    QCOMPARE(chart->visibleStartIndex(), 5);
    QVERIFY(!chart->followLatest());
}

void MainWindowTest::indicatorsRenderThroughSidecar() {
    QSettings settings;
    settings.clear();
    settings.setValue(QStringLiteral("ui/watchlist"), QStringList{QStringLiteral("SHFE.rb2701")});
    settings.setValue(QStringLiteral("ui/chart/symbol"), QStringLiteral("SHFE.rb2701"));
    settings.setValue(QStringLiteral("ui/chart/durationNs"), qint64(60'000'000'000LL));
    settings.setValue(QStringLiteral("ui/indicators/enabled"), QStringList{QStringLiteral("ma")});
    settings.sync();

    FakeNodeClient client;
    MainWindow     window(&client);
    window.show();
    QTest::qWait(50);

    auto* chart = window.findChild<KLineChartWidget*>();
    QVERIFY(chart != nullptr);

    KlineFrame frame;
    frame.reset = true;
    for (int i = 0; i < 40; ++i) {
        KlineBar bar;
        bar.datetime = (1'793'770'620LL + i * 60) * 1'000'000'000LL;
        bar.open = bar.high = bar.low = bar.close = 100.0 + i;
        bar.volume                                = 1;
        frame.bars.push_back(bar);
    }
    client.publishKline(QStringLiteral("SHFE.rb2701"), 60'000'000'000LL, frame);
    QTRY_COMPARE_WITH_TIMEOUT(chart->indicatorCount(), 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(chart->barCount(), 40, 3000);
    QVERIFY(!chart->grab().isNull());
    settings.clear();
}

void MainWindowTest::cleanup() {
    QSettings settings;
    settings.clear();
    settings.sync();
}

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
