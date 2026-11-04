#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>

#include <limits>
#include <optional>

namespace tqsdk::ui {

/// 传输无关的数据模型。
///
/// NodeClient 的实现可以是 HTTP/WS、未来的 UDS 二进制，或测试用 Mock；
/// 页面/图表只看到这些类型，不直接接触 QJsonObject/QWebSocket。
struct Quote {
    QString instrumentId;
    double  lastPrice       = std::numeric_limits<double>::quiet_NaN();
    double  bidPrice1       = std::numeric_limits<double>::quiet_NaN();
    double  askPrice1       = std::numeric_limits<double>::quiet_NaN();
    double  preSettlement   = std::numeric_limits<double>::quiet_NaN();
    double  preOpenInterest = std::numeric_limits<double>::quiet_NaN();
    double  openInterest    = std::numeric_limits<double>::quiet_NaN();
    double  upperLimit      = std::numeric_limits<double>::quiet_NaN();
    double  lowerLimit      = std::numeric_limits<double>::quiet_NaN();
    double  preClose        = std::numeric_limits<double>::quiet_NaN();
    double  open            = std::numeric_limits<double>::quiet_NaN();
    double  high            = std::numeric_limits<double>::quiet_NaN();
    double  low             = std::numeric_limits<double>::quiet_NaN();
    qint64  volume          = 0;
    qint64  datetime        = 0;
};

struct KlineBar {
    qint64  datetime = 0;
    double  open     = std::numeric_limits<double>::quiet_NaN();
    double  high     = std::numeric_limits<double>::quiet_NaN();
    double  low      = std::numeric_limits<double>::quiet_NaN();
    double  close    = std::numeric_limits<double>::quiet_NaN();
    quint64 volume   = 0;
    quint64 openOi   = 0;
    quint64 closeOi  = 0;
};

struct KlineFrame {
    bool              reset = false;
    QVector<KlineBar> bars;
};

struct Instrument {
    QString instrumentId;
    QString exchangeId;
    QString name;
    QString className;
    QString product;
    QString productName;
    QString underlyingSymbol;
    QString optionClass;
    double  lastPrice      = 0.0;
    double  strikePrice    = 0.0;
    double  priceTick      = 0.0;
    double  volumeMultiple = 0.0;
    double  expireDatetime = 0.0;
    int     priceDecs      = 0;
    bool    expired        = false;
};

struct OptionChain {
    QString             underlying;
    QVector<Instrument> calls;
    QVector<Instrument> puts;
};

struct Account {
    QString currency;
    double  balance     = std::numeric_limits<double>::quiet_NaN();
    double  available   = std::numeric_limits<double>::quiet_NaN();
    double  margin      = std::numeric_limits<double>::quiet_NaN();
    double  floatProfit = std::numeric_limits<double>::quiet_NaN();
    double  riskRatio   = std::numeric_limits<double>::quiet_NaN();
};

struct Position {
    QString instrumentId;
    qint64  volumeLong     = 0;
    qint64  volumeShort    = 0;
    double  openPriceLong  = std::numeric_limits<double>::quiet_NaN();
    double  openPriceShort = std::numeric_limits<double>::quiet_NaN();
    double  lastPrice      = std::numeric_limits<double>::quiet_NaN();
    double  floatProfit    = std::numeric_limits<double>::quiet_NaN();
    double  margin         = std::numeric_limits<double>::quiet_NaN();
};

struct Order {
    QString orderId;
    QString instrumentId;
    QString direction;
    QString offset;
    QString status;
    QString message;
    double  limitPrice   = std::numeric_limits<double>::quiet_NaN();
    qint64  volumeOrigin = 0;
    qint64  volumeLeft   = 0;
    bool    isDead       = false;
};

struct Trade {
    QString tradeId;
    QString instrumentId;
    QString direction;
    QString offset;
    double  price         = std::numeric_limits<double>::quiet_NaN();
    qint64  volume        = 0;
    qint64  tradeDateTime = 0;
};

/// 一次 snapshot/diff 中所有实体。
struct MarketUpdate {
    QVector<Quote>                            quotes;
    QHash<QString, QHash<qint64, KlineFrame>> klines; // symbol -> duration -> frame
    bool                                      hasAccount = false;
    Account                                   account;
    QVector<Position>                         positions;
    QVector<Order>                            orders;
    QVector<Trade>                            trades;
};

struct KlineSubscription {
    QString symbol;
    qint64  durationNs = 0;
    int     viewWidth  = 200;
};

struct Subscription {
    QStringList                symbols;
    QVector<KlineSubscription> klines;
    QStringList                topics;
};

enum class Direction {
    Buy,
    Sell
};
enum class Offset {
    Open,
    Close,
    CloseToday
};
enum class PriceType {
    Limit
};

struct OrderRequest {
    QString   instrumentId;
    QString   exchangeId;
    Direction direction  = Direction::Buy;
    Offset    offset     = Offset::Open;
    PriceType priceType  = PriceType::Limit;
    double    limitPrice = 0.0;
    qint64    volume     = 0;
    qint64    minVolume  = 0;
};

struct OrderAck {
    bool    success = false;
    QString orderId;
    QString error;
};

struct CatalogQuery {
    QString classes;
    QString exchange;
    QString product;
    QString query;
    QString underlying;
    bool    includeExpired = false;
    int     limit          = 5000;
    int     offset         = 0;
};

// ---------------------------------------------------------------------------
// JSON <-> typed models
// ---------------------------------------------------------------------------

std::optional<Quote>      parseQuote(const QJsonObject& object);
Quote                     mergeQuote(const Quote& previous, const Quote& patch);
std::optional<KlineBar>   parseKlineBar(const QJsonObject& object);
std::optional<KlineFrame> parseKlineFrame(const QJsonValue& value);
std::optional<Instrument> parseInstrument(const QJsonObject& object);
std::optional<Account>    parseAccount(const QJsonObject& object);
std::optional<Position>   parsePosition(const QJsonObject& object);
std::optional<Order>      parseOrder(const QJsonObject& object);
std::optional<Trade>      parseTrade(const QJsonObject& object);

MarketUpdate parseMarketData(const QJsonObject& data);

QVector<Instrument>        parseCatalog(const QJsonObject& response);
std::optional<OptionChain> parseOptionChain(const QJsonObject& response);
QVector<KlineBar>          parseKlineHistory(const QJsonObject& response);
std::optional<OrderAck>    parseOrderAck(const QJsonObject& response);

QJsonObject makeSubscribe(const Subscription& subscription);
QJsonObject makeOrder(const OrderRequest& request);

QString directionText(Direction direction);
QString offsetText(Offset offset);
QString priceTypeText(PriceType priceType);

} // namespace tqsdk::ui
