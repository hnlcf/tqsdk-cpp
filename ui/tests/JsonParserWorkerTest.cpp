#include "net/JsonParserWorker.h"

#include <QSignalSpy>
#include <QtTest>

using namespace tqsdk::ui;

class JsonParserWorkerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<MarketUpdate>("tqsdk::ui::MarketUpdate");
        qRegisterMetaType<OptionChain>("tqsdk::ui::OptionChain");
        qRegisterMetaType<QVector<Instrument>>("QVector<tqsdk::ui::Instrument>");
        qRegisterMetaType<QVector<KlineBar>>("QVector<tqsdk::ui::KlineBar>");
    }

    void marketUpdatesAreCoalesced() {
        JsonParserWorker worker;
        QSignalSpy       updates(&worker, &JsonParserWorker::marketUpdateParsed);

        worker.parseWsMessage(QByteArray(
            R"json({"type":"snapshot","data":{"quotes":{"SHFE.rb2701":{"instrument_id":"SHFE.rb2701","last_price":3000.0,"pre_settlement":2980.0,"volume":100}},"klines":{}}})json"));
        worker.parseWsMessage(QByteArray(
            R"json({"type":"diff","data":{"quotes":{"SHFE.rb2701":{"instrument_id":"SHFE.rb2701","last_price":3010.0,"volume":101}},"klines":{}}})json"));

        QTRY_COMPARE_WITH_TIMEOUT(updates.count(), 1, 200);
        const auto update = qvariant_cast<MarketUpdate>(updates.first().first());
        QCOMPARE(update.quotes.size(), 1);
        QCOMPARE(update.quotes.first().lastPrice, 3010.0);
        QCOMPARE(update.quotes.first().preSettlement, 2980.0);
        QCOMPARE(update.quotes.first().volume, qint64(101));
    }

    void burstIsCoalesced() {
        JsonParserWorker worker;
        QSignalSpy       updates(&worker, &JsonParserWorker::marketUpdateParsed);
        worker.parseWsMessage(QByteArray(
            R"json({"type":"snapshot","data":{"quotes":{"SHFE.rb2701":{"last_price":3000.0,"pre_settlement":2980.0,"volume":100}},"klines":{}}})json"));
        for (int i = 0; i < 2000; ++i) {
            const QByteArray payload = QByteArray("{\"type\":\"diff\",\"data\":{\"quotes\":{\"SHFE."
                                                  "rb2701\":{\"last_price\":3001.0,\"volume\":") +
                                       QByteArray::number(101 + i) + QByteArray("}}}}");
            worker.parseWsMessage(payload);
        }
        QTRY_VERIFY_WITH_TIMEOUT(updates.count() <= 50, 500);
        QTRY_VERIFY_WITH_TIMEOUT(updates.count() >= 1, 500);
        const auto update = qvariant_cast<MarketUpdate>(updates.last().first());
        QCOMPARE(update.quotes.size(), 1);
        QCOMPARE(update.quotes.first().volume, qint64(2100));
    }

    void wsMessageCompletionIsAlwaysSignaled() {
        JsonParserWorker worker;
        QSignalSpy       finished(&worker, &JsonParserWorker::wsMessageFinished);

        worker.parseWsMessage(
            QByteArray(R"json({"type":"welcome","protocol":1,"heartbeat_interval_ms":30000})json"));
        worker.parseWsMessage(
            QByteArray(R"json({"type":"subscribed","symbols":[],"klines":[],"topics":[]})json"));
        worker.parseWsMessage(QByteArray(R"json({"type":"diff","data":{}})json"));
        worker.parseWsMessage(QByteArray("not-json"));

        QCOMPARE(finished.count(), 4);
    }

    void catalogPayloadIsParsedOffModelThread() {
        JsonParserWorker worker;
        QSignalSpy       catalogs(&worker, &JsonParserWorker::catalogParsed);
        worker.parseCatalogPayload(QByteArray(
            R"json({"instruments":[{"instrument_id":"SHFE.rb2701","instrument_name":"螺纹钢2701","exchange_id":"SHFE","product_id":"rb"}]})json"));

        QTRY_COMPARE_WITH_TIMEOUT(catalogs.count(), 1, 200);
        const auto instruments = qvariant_cast<QVector<Instrument>>(catalogs.first().first());
        QCOMPARE(instruments.size(), 1);
        QCOMPARE(instruments.first().instrumentId, QString("SHFE.rb2701"));
    }
};

QTEST_GUILESS_MAIN(JsonParserWorkerTest)
#include "JsonParserWorkerTest.moc"
