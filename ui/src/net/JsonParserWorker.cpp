#include "net/JsonParserWorker.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>

#include <algorithm>

namespace tqsdk::ui {
namespace {

std::optional<QJsonObject> parseObject(const QByteArray& payload, QString& error) {
    QJsonParseError parseError;
    const auto      document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("invalid node JSON: %1").arg(parseError.errorString());
        return std::nullopt;
    }
    return document.object();
}

} // namespace

JsonParserWorker::JsonParserWorker(QObject* parent)
    : QObject(parent) {}

void JsonParserWorker::stop() {
    if (flushTimer_ != nullptr) {
        flushTimer_->stop();
    }
    pending_ = {};
}

void JsonParserWorker::parseWsMessage(QByteArray payload) {
    // The bounded WS ingress in HttpWsNodeClient dispatches exactly one
    // payload at a time. Every return path must signal completion, otherwise
    // the first message (usually welcome) permanently wedges the queue.
    struct FinishGuard {
        JsonParserWorker* worker;
        ~FinishGuard() { worker->wsMessageFinished(); }
    } finish{this};

    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit wsError(error);
        return;
    }

    const QString type = object->value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("error")) {
        emit wsError(object->value(QStringLiteral("error")).toString());
        return;
    }
    if (type == QStringLiteral("welcome") || type == QStringLiteral("pong")) {
        return;
    }
    if (type == QStringLiteral("subscribed")) {
        emit wsSubscribed();
        return;
    }
    if (type != QStringLiteral("snapshot") && type != QStringLiteral("diff")) {
        emit wsError(QStringLiteral("unknown node WS message type: %1").arg(type));
        return;
    }
    if (type == QStringLiteral("snapshot")) {
        emit wsSnapshot();
    }
    const auto data = object->value(QStringLiteral("data"));
    if (data.isObject()) {
        enqueueMarketUpdate(parseMarketData(data.toObject()));
    }
}

void JsonParserWorker::parseCatalogPayload(QByteArray payload) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    emit catalogParsed(parseCatalog(*object));
}

void JsonParserWorker::parseKlinePayload(QByteArray payload, QString symbol, qint64 durationNs) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    emit klineHistoryParsed(symbol, durationNs, parseKlineHistory(*object));
}

void JsonParserWorker::parseOptionPayload(QByteArray payload) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    if (auto chain = parseOptionChain(*object)) {
        emit optionChainParsed(*chain);
    }
}

void JsonParserWorker::parseQuotePayload(QByteArray payload, QString fallbackSymbol) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    if (auto quote = parseQuote(*object)) {
        emit quoteParsed(*quote, fallbackSymbol);
    }
}

void JsonParserWorker::parseAccountPayload(QByteArray payload) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    if (auto account = parseAccount(*object)) {
        emit accountParsed(*account);
    }
}

void JsonParserWorker::parseMarketPayload(QByteArray payload) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    enqueueMarketUpdate(parseMarketData(*object));
}

void JsonParserWorker::parseOrderAckPayload(QByteArray payload) {
    QString    error;
    const auto object = parseObject(payload, error);
    if (!object) {
        emit parseError(error);
        return;
    }
    if (auto ack = parseOrderAck(*object)) {
        emit orderAckParsed(*ack);
    }
}

void JsonParserWorker::mergeKlineFrame(KlineFrame& target, const KlineFrame& incoming) {
    if (incoming.reset) {
        target = incoming;
        return;
    }
    for (const auto& bar : incoming.bars) {
        if (target.bars.isEmpty() || bar.datetime > target.bars.back().datetime) {
            target.bars.push_back(bar);
            continue;
        }
        if (bar.datetime == target.bars.back().datetime) {
            target.bars.back() = bar;
            continue;
        }
        auto pos = std::lower_bound(
            target.bars.begin(),
            target.bars.end(),
            bar.datetime,
            [](const KlineBar& item, qint64 datetime) { return item.datetime < datetime; });
        if (pos != target.bars.end() && pos->datetime == bar.datetime) {
            *pos = bar;
        } else {
            target.bars.insert(pos, bar);
        }
    }
}

// 同一个 33ms 窗口内的多个 WS/REST 增量在这里合并：
// quote 按 symbol merge，kline 按 symbol/duration 合并，交易快照追加。
// UI 线程只会看到一次合并后的 MarketUpdate。
void JsonParserWorker::enqueueMarketUpdate(const MarketUpdate& update) {
    for (const auto& quote : update.quotes) {
        auto it =
            std::find_if(pending_.quotes.begin(), pending_.quotes.end(), [&](const Quote& item) {
                return item.instrumentId == quote.instrumentId;
            });
        if (it == pending_.quotes.end()) {
            pending_.quotes.push_back(quote);
        } else {
            *it = mergeQuote(*it, quote);
        }
    }

    for (auto sit = update.klines.constBegin(); sit != update.klines.constEnd(); ++sit) {
        for (auto dit = sit.value().constBegin(); dit != sit.value().constEnd(); ++dit) {
            auto& destination = pending_.klines[sit.key()][dit.key()];
            mergeKlineFrame(destination, dit.value());
        }
    }

    if (update.hasAccount) {
        pending_.hasAccount = true;
        pending_.account    = update.account;
    }
    pending_.positions += update.positions;
    pending_.orders += update.orders;
    pending_.trades += update.trades;

    if (flushTimer_ == nullptr) {
        flushTimer_ = new QTimer(this);
        flushTimer_->setSingleShot(true);
        connect(flushTimer_, &QTimer::timeout, this, &JsonParserWorker::flushMarketUpdate);
    }
    if (!flushTimer_->isActive()) {
        flushTimer_->start(33);
    }
}

void JsonParserWorker::flushMarketUpdate() {
    if (pending_.quotes.isEmpty() && pending_.klines.isEmpty() && !pending_.hasAccount &&
        pending_.positions.isEmpty() && pending_.orders.isEmpty() && pending_.trades.isEmpty()) {
        return;
    }
    emit marketUpdateParsed(pending_);
    pending_ = {};
}

} // namespace tqsdk::ui
