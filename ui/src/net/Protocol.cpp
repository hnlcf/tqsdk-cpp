#include "net/Protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <type_traits>

namespace tqsdk::ui {
namespace {

double numberValue(const QJsonValue& value,
                   double            fallback = std::numeric_limits<double>::quiet_NaN()) {
    if (value.isDouble()) {
        const double d = value.toDouble();
        return std::isfinite(d) ? d : fallback;
    }
    if (value.isString()) {
        bool         ok = false;
        const double d  = value.toString().toDouble(&ok);
        return ok && std::isfinite(d) ? d : fallback;
    }
    return fallback;
}

qint64 integerValue(const QJsonValue& value, qint64 fallback = 0) {
    if (value.isDouble()) {
        const double     d      = value.toDouble();
        constexpr double kTwo63 = 9'223'372'036'854'775'808.0;
        if (!std::isfinite(d) || d < -kTwo63 || d >= kTwo63) {
            return fallback;
        }
        return static_cast<qint64>(d);
    }
    if (value.isString()) {
        bool         ok = false;
        const qint64 v  = value.toString().toLongLong(&ok);
        return ok ? v : fallback;
    }
    return fallback;
}

QString stringValue(const QJsonValue& value) {
    return value.isString() ? value.toString() : QString{};
}

bool boolValue(const QJsonValue& value, bool fallback = false) {
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == "true" || text == "1" || text == "yes") {
            return true;
        }
        if (text == "false" || text == "0" || text == "no") {
            return false;
        }
    }
    if (value.isDouble()) {
        return value.toDouble() != 0.0;
    }
    return fallback;
}

} // namespace

std::optional<Quote> parseQuote(const QJsonObject& object) {
    if (object.isEmpty()) {
        return std::nullopt;
    }
    Quote q;
    q.instrumentId    = stringValue(object.value("instrument_id"));
    q.lastPrice       = numberValue(object.value("last_price"));
    q.bidPrice1       = numberValue(object.value("bid_price1"));
    q.askPrice1       = numberValue(object.value("ask_price1"));
    q.preSettlement   = numberValue(object.value("pre_settlement"));
    q.preOpenInterest = numberValue(object.value("pre_open_interest"));
    q.openInterest    = numberValue(object.value("open_interest"));
    q.preClose        = numberValue(object.value("pre_close"));
    q.open            = numberValue(object.value("open"));
    q.high            = numberValue(object.value("highest"));
    q.low             = numberValue(object.value("lowest"));
    q.upperLimit      = numberValue(object.value("upper_limit"));
    q.lowerLimit      = numberValue(object.value("lower_limit"));
    q.volume          = integerValue(object.value("volume"));
    q.datetime        = integerValue(object.value("datetime"));
    return q;
}

std::optional<KlineBar> parseKlineBar(const QJsonObject& object) {
    if (object.isEmpty()) {
        return std::nullopt;
    }
    KlineBar b;
    b.datetime = integerValue(object.value("datetime"));
    b.open     = numberValue(object.value("open"));
    b.high     = numberValue(object.value("high"));
    b.low      = numberValue(object.value("low"));
    b.close    = numberValue(object.value("close"));
    b.volume   = static_cast<quint64>(integerValue(object.value("volume")));
    b.openOi   = static_cast<quint64>(integerValue(object.value("open_oi")));
    b.closeOi  = static_cast<quint64>(integerValue(object.value("close_oi")));
    return b;
}

std::optional<KlineFrame> parseKlineFrame(const QJsonValue& value) {
    if (!value.isObject()) {
        return std::nullopt;
    }
    const auto object = value.toObject();
    KlineFrame frame;
    frame.reset     = boolValue(object.value("reset"), false);
    const auto bars = object.value("bars");
    if (bars.isArray()) {
        frame.bars.reserve(bars.toArray().size());
        for (const auto& item : bars.toArray()) {
            if (item.isObject()) {
                if (auto bar = parseKlineBar(item.toObject())) {
                    frame.bars.push_back(*bar);
                }
            }
        }
    }
    return frame;
}

std::optional<Instrument> parseInstrument(const QJsonObject& object) {
    const QString id = stringValue(object.value("instrument_id"));
    if (id.isEmpty()) {
        return std::nullopt;
    }
    Instrument i;
    i.instrumentId     = id;
    i.exchangeId       = stringValue(object.value("exchange_id"));
    i.name             = stringValue(object.value("name"));
    i.className        = stringValue(object.value("class"));
    i.product          = stringValue(object.value("product"));
    i.productName      = stringValue(object.value("product_name"));
    i.underlyingSymbol = stringValue(object.value("underlying_symbol"));
    i.optionClass      = stringValue(object.value("option_class"));
    i.lastPrice        = numberValue(object.value("last_price"), 0.0);
    i.strikePrice      = numberValue(object.value("strike_price"), 0.0);
    i.priceTick        = numberValue(object.value("price_tick"), 0.0);
    i.volumeMultiple   = numberValue(object.value("volume_multiple"), 0.0);
    i.expireDatetime   = numberValue(object.value("expire_datetime"), 0.0);
    i.priceDecs        = static_cast<int>(integerValue(object.value("price_decs")));
    i.expired          = boolValue(object.value("expired"), false);
    return i;
}

std::optional<Account> parseAccount(const QJsonObject& object) {
    if (object.isEmpty()) {
        return std::nullopt;
    }
    Account a;
    a.currency    = stringValue(object.value("currency"));
    a.balance     = numberValue(object.value("balance"));
    a.available   = numberValue(object.value("available"));
    a.margin      = numberValue(object.value("margin"));
    a.floatProfit = numberValue(object.value("float_profit"));
    a.riskRatio   = numberValue(object.value("risk_ratio"));
    return a;
}

std::optional<Position> parsePosition(const QJsonObject& object) {
    if (object.isEmpty()) {
        return std::nullopt;
    }
    Position p;
    p.instrumentId   = stringValue(object.value("instrument_id"));
    p.volumeLong     = integerValue(object.value("volume_long"));
    p.volumeShort    = integerValue(object.value("volume_short"));
    p.openPriceLong  = numberValue(object.value("open_price_long"));
    p.openPriceShort = numberValue(object.value("open_price_short"));
    p.lastPrice      = numberValue(object.value("last_price"));
    p.floatProfit    = numberValue(object.value("float_profit"));
    p.margin         = numberValue(object.value("margin"));
    return p;
}

std::optional<Order> parseOrder(const QJsonObject& object) {
    if (object.isEmpty()) {
        return std::nullopt;
    }
    Order o;
    o.orderId      = stringValue(object.value("order_id"));
    o.instrumentId = stringValue(object.value("instrument_id"));
    o.direction    = stringValue(object.value("direction"));
    o.offset       = stringValue(object.value("offset"));
    o.status       = stringValue(object.value("status"));
    o.message      = stringValue(object.value("last_msg"));
    o.limitPrice   = numberValue(object.value("limit_price"));
    o.volumeOrigin = integerValue(object.value("volume_orign"));
    o.volumeLeft   = integerValue(object.value("volume_left"));
    o.isDead       = boolValue(object.value("is_dead"), false);
    return o;
}

std::optional<Trade> parseTrade(const QJsonObject& object) {
    if (object.isEmpty()) {
        return std::nullopt;
    }
    Trade t;
    t.tradeId       = stringValue(object.value("trade_id"));
    t.instrumentId  = stringValue(object.value("instrument_id"));
    t.direction     = stringValue(object.value("direction"));
    t.offset        = stringValue(object.value("offset"));
    t.price         = numberValue(object.value("price"));
    t.volume        = integerValue(object.value("volume"));
    t.tradeDateTime = integerValue(object.value("trade_date_time"));
    return t;
}

MarketUpdate parseMarketData(const QJsonObject& data) {
    MarketUpdate update;

    if (const auto quotes = data.value("quotes"); quotes.isObject()) {
        const auto quoteObject = quotes.toObject();
        for (auto it = quoteObject.begin(); it != quoteObject.end(); ++it) {
            if (auto quote = parseQuote(it.value().toObject())) {
                quote->instrumentId = it.key();
                update.quotes.push_back(*quote);
            }
        }
    }

    if (const auto klines = data.value("klines"); klines.isObject()) {
        const auto klineObject = klines.toObject();
        for (auto sit = klineObject.begin(); sit != klineObject.end(); ++sit) {
            if (!sit.value().isObject()) {
                continue;
            }
            const auto byDuration = sit.value().toObject();
            for (auto dit = byDuration.begin(); dit != byDuration.end(); ++dit) {
                bool         ok       = false;
                const qint64 duration = dit.key().toLongLong(&ok);
                if (!ok) {
                    continue;
                }
                if (auto frame = parseKlineFrame(dit.value())) {
                    update.klines[sit.key()][duration] = *frame;
                }
            }
        }
    }

    if (const auto account = data.value("account"); account.isObject()) {
        if (auto parsed = parseAccount(account.toObject())) {
            update.hasAccount = true;
            update.account    = *parsed;
        }
    }

    const auto parseMap = [](const QJsonValue& value, auto&& parser, auto& output) {
        if (!value.isObject()) {
            return;
        }
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!it.value().isObject()) {
                continue;
            }
            if (auto parsed = parser(it.value().toObject())) {
                if constexpr (std::is_same_v<std::decay_t<decltype(*parsed)>, Order>) {
                    if (parsed->orderId.isEmpty()) {
                        parsed->orderId = it.key();
                    }
                }
                if constexpr (std::is_same_v<std::decay_t<decltype(*parsed)>, Trade>) {
                    if (parsed->tradeId.isEmpty()) {
                        parsed->tradeId = it.key();
                    }
                }
                if constexpr (std::is_same_v<std::decay_t<decltype(*parsed)>, Position>) {
                    if (parsed->instrumentId.isEmpty()) {
                        parsed->instrumentId = it.key();
                    }
                }
                output.push_back(*parsed);
            }
        }
    };
    parseMap(data.value("orders"), parseOrder, update.orders);
    parseMap(data.value("trades"), parseTrade, update.trades);
    parseMap(data.value("positions"), parsePosition, update.positions);

    return update;
}

QVector<Instrument> parseCatalog(const QJsonObject& response) {
    QVector<Instrument> out;
    const auto          array = response.value("instruments");
    if (!array.isArray()) {
        return out;
    }
    out.reserve(array.toArray().size());
    for (const auto& item : array.toArray()) {
        if (item.isObject()) {
            if (auto instrument = parseInstrument(item.toObject())) {
                out.push_back(*instrument);
            }
        }
    }
    return out;
}

std::optional<OptionChain> parseOptionChain(const QJsonObject& response) {
    if (response.isEmpty()) {
        return std::nullopt;
    }
    OptionChain chain;
    chain.underlying     = stringValue(response.value("underlying"));
    const auto parseSide = [](const QJsonValue& value, QVector<Instrument>& output) {
        if (!value.isArray()) {
            return;
        }
        for (const auto& item : value.toArray()) {
            if (item.isObject()) {
                const auto object = item.toObject();
                if (auto instrument = parseInstrument(object)) {
                    if (const auto quoteValue = object.value("quote"); quoteValue.isObject()) {
                        if (auto quote = parseQuote(quoteValue.toObject())) {
                            if (std::isfinite(quote->lastPrice)) {
                                instrument->lastPrice = quote->lastPrice;
                            }
                        }
                    }
                    output.push_back(*instrument);
                }
            }
        }
    };
    parseSide(response.value("calls"), chain.calls);
    parseSide(response.value("puts"), chain.puts);
    return chain;
}

QVector<KlineBar> parseKlineHistory(const QJsonObject& response) {
    QVector<KlineBar> out;
    const auto        array = response.value("bars");
    if (!array.isArray()) {
        return out;
    }
    out.reserve(array.toArray().size());
    for (const auto& item : array.toArray()) {
        if (item.isObject()) {
            if (auto bar = parseKlineBar(item.toObject())) {
                out.push_back(*bar);
            }
        }
    }
    return out;
}

std::optional<OrderAck> parseOrderAck(const QJsonObject& response) {
    OrderAck ack;
    ack.success = boolValue(response.value("success"), false);
    ack.orderId = stringValue(response.value("order_id"));
    ack.error   = stringValue(response.value("error"));
    return ack;
}

QJsonObject makeSubscribe(const Subscription& subscription) {
    QJsonArray symbols;
    for (const auto& symbol : subscription.symbols) {
        symbols.append(symbol);
    }

    QJsonArray klines;
    for (const auto& kline : subscription.klines) {
        klines.append(QJsonObject{{"symbol", kline.symbol},
                                  {"duration_ns", static_cast<double>(kline.durationNs)},
                                  {"view_width", kline.viewWidth}});
    }

    QJsonArray topics;
    for (const auto& topic : subscription.topics) {
        topics.append(topic);
    }

    return QJsonObject{
        {"op", "subscribe"}, {"symbols", symbols}, {"klines", klines}, {"topics", topics}};
}

QJsonObject makeOrder(const OrderRequest& request) {
    return QJsonObject{
        {"instrument_id", request.instrumentId},
        {"exchange_id", request.exchangeId},
        {"direction", directionText(request.direction)},
        {"offset", offsetText(request.offset)},
        {"price_type", priceTypeText(request.priceType)},
        {"limit_price", request.limitPrice},
        {"time_cond", "GFD"},
        {"volume_cond", "ANY"},
        {"volume", static_cast<double>(request.volume)},
        {"min_volume", static_cast<double>(request.minVolume)},
    };
}

QString directionText(Direction direction) {
    return direction == Direction::Buy ? "BUY" : "SELL";
}

QString offsetText(Offset offset) {
    switch (offset) {
    case Offset::Open:
        return "OPEN";
    case Offset::Close:
        return "CLOSE";
    case Offset::CloseToday:
        return "CLOSETODAY";
    }
    return "OPEN";
}

QString priceTypeText(PriceType priceType) {
    (void)priceType;
    return "LIMIT";
}

} // namespace tqsdk::ui
