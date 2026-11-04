#include "net/Protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <cmath>

using namespace tqsdk::ui;

class ProtocolTest : public QObject {
    Q_OBJECT

private slots:
    void parsesMarketData();
    void parsesCatalogAndHistory();
    void staleCatalogFallbackDoesNotOverwriteLiveQuote();
    void parsesOptionChain();
    void buildsRequests();
    void rejectsMalformedNumbers();
};

namespace {

QJsonObject objectFrom(const char* text) {
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

} // namespace

void ProtocolTest::parsesMarketData() {
    const auto data = objectFrom(R"({
      "quotes": {
        "SHFE.rb2701": {"datetime":"2026-11-04 13:40:22.000000","last_price":"2991.5","pre_settlement":2980,"bid_price1":2991,"ask_price1":2992,"bid_price2":2990,"ask_price2":2993,"bid_volume1":10,"ask_volume1":20,"bid_volume2":30,"ask_volume2":40,
                         "volume":702802,"open_interest":1878506,"pre_open_interest":1878000,
                         "highest":2999,"lowest":2970,"upper_limit":3100,"lower_limit":2900}
      },
      "klines": {
        "SHFE.rb2701": {
          "60000000000": {
            "reset": true,
            "bars": [
              {"datetime":1793689140000000000,"open":2980,"high":2995,"low":2975,
               "close":2991,"volume":10,"open_oi":20,"close_oi":21}
            ]
          }
        }
      },
      "account": {"currency":"CNY","balance":1000000,"available":800000,
                  "margin":200000,"float_profit":12.5},
      "orders": {"r1":{"instrument_id":"SHFE.rb2701","direction":"BUY","offset":"OPEN",
                        "price_type":"LIMIT","limit_price":2990,"volume_orign":1,
                        "volume_left":1,"status":"ALIVE"}},
      "positions": {"SHFE.rb2701":{"volume_long":1,"open_price_long":2980,"last_price":2991}},
      "trades": {"t1":{"instrument_id":"SHFE.rb2701","direction":"BUY","offset":"OPEN",
                       "price":2990,"volume":1}}
    })");

    const auto update = parseMarketData(data);
    QVERIFY(update.quotes.size() == 1);
    QVERIFY(qFuzzyCompare(update.quotes[0].lastPrice, 2991.5));
    QVERIFY(update.quotes[0].datetime > 0);
    QVERIFY(qFuzzyCompare(update.quotes[0].preSettlement, 2980.0));
    QVERIFY(qFuzzyCompare(update.quotes[0].preOpenInterest, 1'878'000.0));
    QVERIFY(qFuzzyCompare(update.quotes[0].high, 2999.0));
    QVERIFY(qFuzzyCompare(update.quotes[0].low, 2970.0));
    QVERIFY(qFuzzyCompare(update.quotes[0].upperLimit, 3100.0));
    QVERIFY(qFuzzyCompare(update.quotes[0].bidPrice2, 2990.0));
    QVERIFY(qFuzzyCompare(update.quotes[0].askPrice2, 2993.0));
    QVERIFY(update.quotes[0].bidVolume2 == 30);
    QVERIFY(update.quotes[0].askVolume2 == 40);
    QVERIFY(qFuzzyCompare(update.quotes[0].lowerLimit, 2900.0));
    QVERIFY(update.quotes[0].volume == 702802);
    QVERIFY(update.klines.size() == 1);
    QVERIFY(update.klines["SHFE.rb2701"].size() == 1);
    QVERIFY(update.klines["SHFE.rb2701"][60'000'000'000].reset);
    QVERIFY(update.klines["SHFE.rb2701"][60'000'000'000].bars.size() == 1);
    QVERIFY(qFuzzyCompare(update.klines["SHFE.rb2701"][60'000'000'000].bars[0].close, 2991.0));

    QVERIFY(update.hasAccount);
    QVERIFY(qFuzzyCompare(update.account.balance, 1'000'000.0));
    QVERIFY(update.orders.size() == 1);
    QVERIFY(update.orders[0].orderId == "r1");
    QVERIFY(update.orders[0].volumeOrigin == 1);
    QVERIFY(update.positions.size() == 1);
    QVERIFY(update.positions[0].instrumentId == "SHFE.rb2701");
    QVERIFY(update.trades.size() == 1);
    QVERIFY(update.trades[0].tradeId == "t1");
}

void ProtocolTest::parsesCatalogAndHistory() {
    const auto catalog     = objectFrom(R"({"instruments":[
      {"instrument_id":"SHFE.rb2701","exchange_id":"SHFE","name":"螺纹2701",
       "class":"FUTURE","product":"SHFE.rb","product_name":"螺纹钢",
       "last_price":"3017.5","price_tick":"1","volume_multiple":"10","price_decs":"1",
       "expired":"false"}
    ]})");
    const auto instruments = parseCatalog(catalog);
    QVERIFY(instruments.size() == 1);
    QVERIFY(instruments[0].instrumentId == "SHFE.rb2701");
    QVERIFY(instruments[0].className == "FUTURE");
    QVERIFY(qFuzzyCompare(instruments[0].lastPrice, 3017.5));
    QVERIFY(qFuzzyCompare(instruments[0].volumeMultiple, 10.0));
    QVERIFY(instruments[0].priceDecs == 1);
    QVERIFY(!instruments[0].expired);

    const auto history = objectFrom(R"({"bars":[
      {"datetime":1,"open":100,"high":101,"low":99,"close":100.5,"volume":3,
       "open_oi":4,"close_oi":5}
    ]})");
    const auto bars    = parseKlineHistory(history);
    QVERIFY(bars.size() == 1);
    QVERIFY(qFuzzyCompare(bars[0].close, 100.5));
    QVERIFY(bars[0].volume == 3u);
}

void ProtocolTest::parsesOptionChain() {
    const auto chain = parseOptionChain(objectFrom(R"({
      "underlying":"SHFE.rb2701",
      "calls":[{"instrument_id":"SHFE.rb2701C3000","name":"rb2701C3000",
                "option_class":"CALL","strike_price":"3000","last_price":"12.5"}],
      "puts":[{"instrument_id":"SHFE.rb2701P3000","name":"rb2701P3000",
               "option_class":"PUT","strike_price":3000,"last_price":8.0}]
    })"));
    QVERIFY(chain.has_value());
    QCOMPARE(chain->underlying, QString("SHFE.rb2701"));
    QCOMPARE(chain->calls.size(), 1);
    QCOMPARE(chain->calls.front().instrumentId, QString("SHFE.rb2701C3000"));
    QCOMPARE(chain->puts.size(), 1);
    QVERIFY(qFuzzyCompare(chain->puts.front().strikePrice, 3000.0));
}

void ProtocolTest::staleCatalogFallbackDoesNotOverwriteLiveQuote() {
    Quote live;
    live.instrumentId = "CZCE.SA701";
    live.lastPrice    = 989.0;
    live.datetime     = 1'793'770'642'000'000'000LL;

    Quote fallback;
    fallback.instrumentId = "CZCE.SA701";
    fallback.lastPrice    = 968.0;
    fallback.datetime     = 0;

    const Quote merged = mergeQuote(live, fallback);
    QCOMPARE(merged.lastPrice, 989.0);

    Quote newer;
    newer.instrumentId = "CZCE.SA701";
    newer.lastPrice    = 990.0;
    newer.datetime     = 1'793'770'643'000'000'000LL;
    QCOMPARE(mergeQuote(merged, newer).lastPrice, 990.0);
}

void ProtocolTest::buildsRequests() {
    Subscription subscription;
    subscription.symbols = {"SHFE.rb2701"};
    subscription.klines.push_back({"SHFE.rb2701", 60'000'000'000LL, 200});
    subscription.topics  = {"account", "positions"};
    const auto subscribe = makeSubscribe(subscription);
    QVERIFY(subscribe["op"].toString() == "subscribe");
    QVERIFY(subscribe["symbols"].toArray().size() == 1);
    QVERIFY(subscribe["klines"].toArray()[0].toObject()["symbol"].toString() == "SHFE.rb2701");
    QVERIFY(subscribe["topics"].toArray().size() == 2);

    OrderRequest request;
    request.instrumentId = "SHFE.rb2701";
    request.direction    = Direction::Sell;
    request.offset       = Offset::CloseToday;
    request.limitPrice   = 3010.5;
    request.volume       = 2;
    const auto order     = makeOrder(request);
    QVERIFY(order["direction"].toString() == "SELL");
    QVERIFY(order["offset"].toString() == "CLOSETODAY");
    QVERIFY(order["price_type"].toString() == "LIMIT");
    QVERIFY(qFuzzyCompare(order["limit_price"].toDouble(), 3010.5));

    const auto ack = parseOrderAck(objectFrom(R"({"success":true,"order_id":"r9","error":""})"));
    QVERIFY(ack.has_value());
    QVERIFY(ack->success);
    QVERIFY(ack->orderId == "r9");
}

void ProtocolTest::rejectsMalformedNumbers() {
    const auto quote = parseQuote(objectFrom(R"({"last_price":"not-a-number"})"));
    QVERIFY(quote.has_value());
    QVERIFY(!std::isfinite(quote->lastPrice));

    const auto frame = parseKlineFrame(QJsonValue(objectFrom(R"({"bars":"bad"})")));
    QVERIFY(frame.has_value());
    QVERIFY(frame->bars.empty());

    QVERIFY(parseCatalog(objectFrom(R"({"instruments":{}})")).empty());
    QVERIFY(parseKlineHistory(objectFrom(R"({"bars":{}})")).empty());
}

QTEST_APPLESS_MAIN(ProtocolTest)
#include "ProtocolTest.moc"
