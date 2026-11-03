#include "model/MarketStore.h"

#include <algorithm>

namespace tqsdk::ui {

MarketStore::MarketStore(QObject* parent)
    : QObject(parent) {}

const Quote* MarketStore::quote(const QString& symbol) const {
    auto it = quotes_.constFind(symbol);
    return it == quotes_.constEnd() ? nullptr : &it.value();
}

QVector<KlineBar> MarketStore::bars(const QString& symbol, qint64 durationNs) const {
    auto symbolIt = klines_.constFind(symbol);
    if (symbolIt == klines_.constEnd()) {
        return {};
    }
    return symbolIt.value().value(durationNs);
}

void MarketStore::applyQuotes(const QVector<Quote>& quotes) {
    if (quotes.isEmpty()) {
        return;
    }
    for (const auto& quote : quotes) {
        if (!quote.instrumentId.isEmpty()) {
            quotes_[quote.instrumentId] = quote;
        }
    }
    emit quotesChanged();
}

void MarketStore::applyKline(const QString& symbol, qint64 durationNs, const KlineFrame& frame) {
    if (symbol.isEmpty() || durationNs <= 0) {
        return;
    }

    auto& bars = klines_[symbol][durationNs];
    if (frame.reset) {
        bars = frame.bars;
    } else {
        for (const auto& incoming : frame.bars) {
            if (bars.isEmpty() || incoming.datetime > bars.back().datetime) {
                bars.push_back(incoming);
            } else if (incoming.datetime == bars.back().datetime) {
                bars.back() = incoming;
            } else {
                auto pos = std::lower_bound(
                    bars.begin(),
                    bars.end(),
                    incoming.datetime,
                    [](const KlineBar& item, qint64 datetime) { return item.datetime < datetime; });
                if (pos != bars.end() && pos->datetime == incoming.datetime) {
                    *pos = incoming;
                } else {
                    bars.insert(pos, incoming);
                }
            }
        }
    }

    while (bars.size() > maxBars_) {
        bars.removeFirst();
    }
    emit klineChanged(symbol, durationNs);
}

} // namespace tqsdk::ui
