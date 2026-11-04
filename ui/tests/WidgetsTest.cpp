#include "model/TradingModels.h"
#include "widgets/MarketToolBar.h"
#include "widgets/OrderBookPanel.h"
#include "widgets/OrderTicketPanel.h"
#include "widgets/QuoteSnapshotPanel.h"
#include "widgets/TradeTapePanel.h"
#include "widgets/TradingPanels.h"

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QLineEdit>
#include <QtTest>

using namespace tqsdk::ui;

class WidgetsTest : public QObject {
    Q_OBJECT

private slots:
    void quotePanelFormatsAndClears();
    void orderTicketEmitsActions();
    void accountPanelShowsAccount();
    void orderBookShowsFiveLevels();
    void positionDetailShowsSelectedPosition();
    void tradingPanelsRemainIndependent();
    void tradeTapePanelOwnsBoundedView();
    void marketToolBarEmitsSearchText();
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

    auto* last   = panel.findChild<QLabel*>(QStringLiteral("quoteLast"));
    auto* change = panel.findChild<QLabel*>(QStringLiteral("quoteChange"));
    auto* pct    = panel.findChild<QLabel*>(QStringLiteral("quoteChangePct"));
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

void WidgetsTest::accountPanelShowsAccount() {
    AccountPanel panel;
    panel.setAccount(
        Account{.balance = 1000000.0, .available = 800000.0, .margin = 200000.0, .riskRatio = 0.2});

    auto* balance   = panel.findChild<QLabel*>(QStringLiteral("accountBalance"));
    auto* available = panel.findChild<QLabel*>(QStringLiteral("accountAvailable"));
    auto* margin    = panel.findChild<QLabel*>(QStringLiteral("accountMargin"));
    auto* risk      = panel.findChild<QLabel*>(QStringLiteral("accountRisk"));
    QVERIFY(balance != nullptr);
    QVERIFY(available != nullptr);
    QVERIFY(margin != nullptr);
    QVERIFY(risk != nullptr);
    QCOMPARE(balance->text(), QStringLiteral("1000000.00"));
    QCOMPARE(available->text(), QStringLiteral("800000.00"));
    QCOMPARE(margin->text(), QStringLiteral("200000.00"));
    QCOMPARE(risk->text(), QStringLiteral("20.00%"));
}

void WidgetsTest::orderBookShowsFiveLevels() {
    OrderBookPanel panel;
    Quote          quote;
    quote.bidPrice1     = 2999.0;
    quote.askPrice1     = 3001.0;
    quote.bidPrice2     = 2998.0;
    quote.askPrice2     = 3002.0;
    quote.bidVolume1    = 11;
    quote.askVolume1    = 12;
    quote.bidVolume2    = 21;
    quote.askVolume2    = 22;
    quote.lastPrice     = 3000.0;
    quote.preSettlement = 2995.0;
    panel.setQuote(&quote);

    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("bookBidPrice1"))->text(),
             QStringLiteral("2999.00"));
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("bookAskPrice1"))->text(),
             QStringLiteral("3001.00"));
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("bookBidVolume2"))->text(),
             QStringLiteral("21"));
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("bookAskVolume2"))->text(),
             QStringLiteral("22"));
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("bookLast"))->text(),
             QStringLiteral("3000.00"));
}

void WidgetsTest::positionDetailShowsSelectedPosition() {
    PositionDetailPanel panel;
    Position            position;
    position.instrumentId   = QStringLiteral("SHFE.rb2701");
    position.volumeLong     = 2;
    position.volumeShort    = 1;
    position.openPriceLong  = 3000.0;
    position.openPriceShort = 3010.0;
    position.lastPrice      = 3005.0;
    position.floatProfit    = 100.0;
    position.margin         = 6000.0;
    panel.setPosition(&position);
    QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("positionDetailSymbol"))->text(),
             QStringLiteral("SHFE.rb2701"));
    panel.clear();
}

void WidgetsTest::tradingPanelsRemainIndependent() {
    PositionTableModel positions;
    OrderTableModel    orders;
    TradeTableModel    trades;

    Position position;
    position.instrumentId = QStringLiteral("SHFE.rb2701");
    position.volumeLong   = 1;
    positions.applyPositions({position});

    Order order;
    order.orderId      = QStringLiteral("1001");
    order.instrumentId = QStringLiteral("SHFE.rb2701");
    order.direction    = QStringLiteral("BUY");
    order.offset       = QStringLiteral("OPEN");
    order.status       = QStringLiteral("ALIVE");
    orders.applyOrders({order});

    PositionsPanel positionsPanel(&positions);
    OrdersPanel    ordersPanel(&orders);
    TradesPanel    tradesPanel(&trades);
    QVERIFY(positionsPanel.view() != nullptr);
    QVERIFY(ordersPanel.view() != nullptr);
    QVERIFY(tradesPanel.view() != nullptr);

    ordersPanel.view()->selectRow(0);
    QCOMPARE(ordersPanel.selectedOrderId(), QStringLiteral("1001"));
}

void WidgetsTest::tradeTapePanelOwnsBoundedView() {
    TradeTapeModel model;
    TradeTapePanel panel(&model);
    QVERIFY(panel.view() != nullptr);
    QCOMPARE(panel.view()->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    QCOMPARE(panel.view()->selectionMode(), QAbstractItemView::NoSelection);
}

void WidgetsTest::marketToolBarEmitsSearchText() {
    MarketToolBar toolbar;
    QSignalSpy    search(&toolbar, &MarketToolBar::searchTextChanged);
    auto*         edit = toolbar.findChild<QLineEdit*>(QStringLiteral("marketSearch"));
    QVERIFY(edit != nullptr);
    edit->setText(QStringLiteral("rb"));
    QCOMPARE(search.count(), 1);
    QCOMPARE(search.first().first().toString(), QStringLiteral("rb"));
    toolbar.setCompletions({QStringLiteral("SHFE.rb2701  螺纹钢2701")});
}

QTEST_MAIN(WidgetsTest)
#include "WidgetsTest.moc"
