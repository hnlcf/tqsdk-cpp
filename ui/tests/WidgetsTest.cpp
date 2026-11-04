#include "model/TradingModels.h"
#include "widgets/OrderTicketPanel.h"
#include "widgets/QuoteSnapshotPanel.h"
#include "widgets/TradingTablesPanel.h"

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace tqsdk::ui;

class WidgetsTest : public QObject {
    Q_OBJECT

private slots:
    void quotePanelFormatsAndClears();
    void orderTicketEmitsActions();
    void tradingPanelShowsAccount();
};

void WidgetsTest::quotePanelFormatsAndClears() {
    QuoteSnapshotPanel panel;
    panel.setSymbol(QStringLiteral("SHFE.rb2701"));

    Quote quote;
    quote.instrumentId  = QStringLiteral("SHFE.rb2701");
    quote.lastPrice     = 3010.5;
    quote.preSettlement = 3000.0;
    quote.bidPrice1     = 3010.0;
    quote.askPrice1     = 3011.0;
    quote.volume        = 42;
    quote.high          = 3020.0;
    quote.low           = 2990.0;
    panel.setQuote(&quote, QStringLiteral("螺纹钢2701"));

    auto* last = panel.findChild<QLabel*>(QStringLiteral("quoteLast"));
    auto* change = panel.findChild<QLabel*>(QStringLiteral("quoteChange"));
    auto* pct = panel.findChild<QLabel*>(QStringLiteral("quoteChangePct"));
    QVERIFY(last != nullptr);
    QVERIFY(change != nullptr);
    QVERIFY(pct != nullptr);
    QCOMPARE(last->text(), QStringLiteral("3010.50"));
    QCOMPARE(change->text(), QStringLiteral("+10.50"));
    QCOMPARE(pct->text(), QStringLiteral("+0.35%"));

    panel.setQuote(nullptr, QString{});
    QCOMPARE(last->text(), QStringLiteral("—"));
    QCOMPARE(change->text(), QStringLiteral("—"));
}

void WidgetsTest::orderTicketEmitsActions() {
    OrderTicketPanel panel;
    panel.setSymbol(QStringLiteral("SHFE.rb2701"));
    panel.setTradingEnabled(true);
    panel.setCloseEnabled(true);
    panel.setPriceIfEmpty(3010.5);
    panel.show();
    QTest::qWait(10);

    QCOMPARE(panel.symbolText(), QStringLiteral("SHFE.rb2701"));
    QCOMPARE(panel.price(), 3010.5);
    QCOMPARE(panel.volume(), 1);

    QSignalSpy buy(&panel, &OrderTicketPanel::buyOpenRequested);
    QSignalSpy sell(&panel, &OrderTicketPanel::sellOpenRequested);
    QSignalSpy close(&panel, &OrderTicketPanel::closeRequested);
    QTest::mouseClick(panel.findChild<QPushButton*>(QStringLiteral("orderBuy")), Qt::LeftButton);
    QTest::mouseClick(panel.findChild<QPushButton*>(QStringLiteral("orderSell")), Qt::LeftButton);
    QTest::mouseClick(panel.findChild<QPushButton*>(QStringLiteral("orderClose")), Qt::LeftButton);
    QCOMPARE(buy.count(), 1);
    QCOMPARE(sell.count(), 1);
    QCOMPARE(close.count(), 1);
}

void WidgetsTest::tradingPanelShowsAccount() {
    PositionTableModel positions;
    OrderTableModel    orders;
    TradeTableModel    trades;
    TradingTablesPanel panel(&positions, &orders, &trades);
    panel.setAccount(Account{.balance = 1000000.0,
                             .available = 800000.0,
                             .margin = 200000.0,
                             .riskRatio = 0.2});

    auto* balance = panel.findChild<QLabel*>(QStringLiteral("accountBalance"));
    auto* available = panel.findChild<QLabel*>(QStringLiteral("accountAvailable"));
    auto* margin = panel.findChild<QLabel*>(QStringLiteral("accountMargin"));
    auto* risk = panel.findChild<QLabel*>(QStringLiteral("accountRisk"));
    QVERIFY(balance != nullptr);
    QVERIFY(available != nullptr);
    QVERIFY(margin != nullptr);
    QVERIFY(risk != nullptr);
    QCOMPARE(balance->text(), QStringLiteral("1000000.00"));
    QCOMPARE(available->text(), QStringLiteral("800000.00"));
    QCOMPARE(margin->text(), QStringLiteral("200000.00"));
    QCOMPARE(risk->text(), QStringLiteral("20.00%"));
}

QTEST_MAIN(WidgetsTest)
#include "WidgetsTest.moc"
