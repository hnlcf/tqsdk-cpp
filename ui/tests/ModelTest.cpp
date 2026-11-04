#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "model/TradingModels.h"

#include <QtTest>

using namespace tqsdk::ui;

class ModelTest : public QObject {
    Q_OBJECT

private slots:
    void marketStoreQuotesAndKlines();
    void quoteModelFormatsRows();
    void instrumentModelFormatsRows();
    void tradingModelsMerge();
    void tradeTapeFromQuoteDeltas();
    void quoteMergeRetainsSnapshotFields();
    void incrementalInstrumentLoad();
    void quoteModelReordersSymbols();
};

void ModelTest::marketStoreQuotesAndKlines() {
    MarketStore store;
    QSignalSpy  quoteSpy(&store, &MarketStore::quotesChanged);

    Quote quote;
    quote.instrumentId  = "SHFE.rb2701";
    quote.lastPrice     = 3010.5;
    quote.preSettlement = 3000.0;
    quote.bidPrice1     = 3010.0;
    quote.askPrice1     = 3011.0;
    quote.volume        = 123;
    store.applyQuotes({quote});
    QCOMPARE(quoteSpy.count(), 1);
    QVERIFY(store.quote("SHFE.rb2701") != nullptr);
    QCOMPARE(store.quote("SHFE.rb2701")->lastPrice, 3010.5);

    KlineFrame reset;
    reset.reset = true;
    reset.bars  = {{1'000'000'000LL, 10.0, 12.0, 9.0, 11.0, 5, 1, 2}};
    store.applyKline("SHFE.rb2701", 60'000'000'000LL, reset);
    QCOMPARE(store.bars("SHFE.rb2701", 60'000'000'000LL).size(), 1);

    KlineFrame tick;
    tick.bars = {{2'000'000'000LL, 11.0, 13.0, 10.0, 12.0, 6, 2, 3}};
    store.applyKline("SHFE.rb2701", 60'000'000'000LL, tick);
    auto bars = store.bars("SHFE.rb2701", 60'000'000'000LL);
    QCOMPARE(bars.size(), 2);
    QCOMPARE(bars.back().close, 12.0);

    KlineFrame update;
    update.bars = {{2'000'000'000LL, 11.0, 14.0, 10.0, 13.0, 7, 2, 4}};
    store.applyKline("SHFE.rb2701", 60'000'000'000LL, update);
    bars = store.bars("SHFE.rb2701", 60'000'000'000LL);
    QCOMPARE(bars.size(), 2);
    QCOMPARE(bars.back().close, 13.0);

    KlineFrame older;
    older.bars = {{500'000'000LL, 9.0, 10.0, 8.0, 9.5, 2, 0, 1}};
    store.applyKline("SHFE.rb2701", 60'000'000'000LL, older);
    bars = store.bars("SHFE.rb2701", 60'000'000'000LL);
    QCOMPARE(bars.size(), 3);
    QCOMPARE(bars.front().datetime, 500'000'000LL);
}

void ModelTest::quoteModelFormatsRows() {
    QuoteTableModel model;
    model.setSymbols({"SHFE.rb2701", "DCE.m2701"});
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.symbolAt(1), QString("DCE.m2701"));

    Quote quote;
    quote.instrumentId  = "SHFE.rb2701";
    quote.lastPrice     = 3010.5;
    quote.preSettlement = 3000.0;
    quote.bidPrice1     = 3010.0;
    quote.askPrice1     = 3011.0;
    quote.volume        = 42;
    model.applyQuotes({quote});
    QCOMPARE(model.data(model.index(0, QuoteTableModel::Last)).toString(), QString("3010.50"));
    QCOMPARE(model.data(model.index(0, QuoteTableModel::ChangePercent)).toString(),
             QString("+0.35%"));
    QCOMPARE(model.data(model.index(1, QuoteTableModel::Last)).toString(), QString("—"));
}

void ModelTest::tradingModelsMerge() {
    PositionTableModel positions;
    Position           position;
    position.instrumentId  = "SHFE.rb2701";
    position.volumeLong    = 2;
    position.openPriceLong = 3000.0;
    position.lastPrice     = 3010.0;
    position.floatProfit   = 200.0;
    QSignalSpy inserted(&positions, &QAbstractItemModel::rowsInserted);
    positions.applyPositions({position});
    QCOMPARE(positions.rowCount(), 1);
    QCOMPARE(inserted.count(), 1);
    QCOMPARE(positions.data(positions.index(0, PositionTableModel::Symbol)).toString(),
             QString("SHFE.rb2701"));
    position.floatProfit = 210.0;
    positions.applyPositions({position});
    QCOMPARE(positions.rowCount(), 1);
    QCOMPARE(positions.data(positions.index(0, PositionTableModel::FloatProfit)).toString(),
             QString("210.00"));

    OrderTableModel orders;
    Order           order;
    order.orderId      = "1001";
    order.instrumentId = "SHFE.rb2701";
    order.direction    = "BUY";
    order.offset       = "OPEN";
    order.limitPrice   = 3000.0;
    order.volumeOrigin = 1;
    order.volumeLeft   = 1;
    order.status       = "ALIVE";
    order.isDead       = false;
    orders.applyOrders({order});
    QCOMPARE(orders.rowCount(), 1);
    QCOMPARE(orders.orderIdAt(0), QString("1001"));
    QVERIFY(orders.isOrderAlive(0));
    order.isDead = true;
    order.status = "FINISHED";
    orders.applyOrders({order});
    QCOMPARE(orders.rowCount(), 1);
    QVERIFY(!orders.isOrderAlive(0));

    TradeTableModel trades;
    Trade           trade;
    trade.tradeId      = "2001";
    trade.instrumentId = "SHFE.rb2701";
    trade.direction    = "BUY";
    trade.price        = 3000.0;
    trade.volume       = 1;
    trades.applyTrades({trade});
    QCOMPARE(trades.rowCount(), 1);
    QCOMPARE(trades.data(trades.index(0, TradeTableModel::TradeId)).toString(), QString("2001"));
}

void ModelTest::tradeTapeFromQuoteDeltas() {
    TradeTapeModel tape;
    Quote          quote;
    quote.instrumentId = "SHFE.rb2701";
    quote.datetime     = 1'700'000'000'000'000'000LL;
    quote.lastPrice    = 3010.0;
    quote.volume       = 100;
    tape.applyQuote(quote);
    QCOMPARE(tape.rowCount(), 0);

    quote.lastPrice = 3011.0;
    quote.volume    = 105;
    tape.applyQuote(quote);
    QCOMPARE(tape.rowCount(), 1);
    QCOMPARE(tape.data(tape.index(0, TradeTapeModel::Volume)).toString(), QString("5"));
    QCOMPARE(tape.data(tape.index(0, TradeTapeModel::Direction)).toString(), QString("买"));

    quote.lastPrice = 3009.0;
    quote.volume    = 109;
    tape.applyQuote(quote);
    QCOMPARE(tape.rowCount(), 2);
    QCOMPARE(tape.data(tape.index(0, TradeTapeModel::Direction)).toString(), QString("卖"));

    tape.clear();
    QCOMPARE(tape.rowCount(), 0);
}

void ModelTest::quoteMergeRetainsSnapshotFields() {
    MarketStore store;
    Quote       full;
    full.instrumentId    = "SHFE.rb2701";
    full.lastPrice       = 3000.0;
    full.preSettlement   = 2980.0;
    full.volume          = 100;
    full.openInterest    = 200;
    full.preOpenInterest = 190;
    full.upperLimit      = 3150.0;
    store.applyQuotes({full});

    Quote patch;
    patch.instrumentId = "SHFE.rb2701";
    patch.lastPrice    = 3010.0;
    patch.volume       = 101;
    store.applyQuotes({patch});

    const Quote* merged = store.quote("SHFE.rb2701");
    QVERIFY(merged != nullptr);
    QVERIFY(qFuzzyCompare(merged->lastPrice, 3010.0));
    QVERIFY(qFuzzyCompare(merged->preSettlement, 2980.0));
    QCOMPARE(merged->volume, qint64(101));
    QVERIFY(qFuzzyCompare(merged->openInterest, 200.0));
    QVERIFY(qFuzzyCompare(merged->upperLimit, 3150.0));
}

void ModelTest::instrumentModelFormatsRows() {
    InstrumentTableModel model;
    Instrument           instrument;
    instrument.instrumentId   = "SHFE.rb2701";
    instrument.name           = "螺纹2701";
    instrument.exchangeId     = "SHFE";
    instrument.product        = "SHFE.rb";
    instrument.lastPrice      = 3010.0;
    instrument.priceTick      = 1.0;
    instrument.volumeMultiple = 10.0;
    model.setInstruments({instrument});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.symbolAt(0), QString("SHFE.rb2701"));
    QCOMPARE(model.data(model.index(0, InstrumentTableModel::Name)).toString(),
             QString("螺纹2701"));
    QCOMPARE(model.data(model.index(0, InstrumentTableModel::Multiplier)).toString(),
             QString("10"));

    Quote quote;
    quote.instrumentId  = "SHFE.rb2701";
    quote.lastPrice     = 3011.0;
    quote.preSettlement = 3000.0;
    quote.volume        = 77;
    model.applyQuotes({quote});
    QCOMPARE(model.data(model.index(0, InstrumentTableModel::LastPrice)).toString(),
             QString("3011.00"));
    QCOMPARE(model.data(model.index(0, InstrumentTableModel::ChangePercent)).toString(),
             QString("+0.37%"));
    QCOMPARE(model.data(model.index(0, InstrumentTableModel::Volume)).toString(), QString("77"));
}

void ModelTest::incrementalInstrumentLoad() {
    InstrumentTableModel model;
    QVector<Instrument>  instruments;
    for (int i = 0; i < 20000; ++i) {
        Instrument instrument;
        instrument.instrumentId = QStringLiteral("SHFE.rb%1").arg(2700 + i);
        instruments.push_back(instrument);
    }
    QSignalSpy loaded(&model, &InstrumentTableModel::instrumentsLoaded);
    model.setInstrumentsIncremental(instruments, 512);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 20000, 5000);
    QCOMPARE(loaded.count(), 1);
    QCOMPARE(model.symbolAt(19999), QStringLiteral("SHFE.rb22699"));
}

void ModelTest::quoteModelReordersSymbols() {
    QuoteTableModel model;
    model.setSymbols({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    QSignalSpy       reordered(&model, &QuoteTableModel::symbolsReordered);
    const QMimeData* mime = model.mimeData({model.index(2, 0)});
    QVERIFY(mime != nullptr);
    QVERIFY(model.dropMimeData(mime, Qt::MoveAction, 0, 0, {}));
    delete mime;
    QCOMPARE(reordered.count(), 1);
    QCOMPARE(model.symbols(),
             (QStringList{QStringLiteral("C"), QStringLiteral("A"), QStringLiteral("B")}));
}

QTEST_GUILESS_MAIN(ModelTest)
#include "ModelTest.moc"
