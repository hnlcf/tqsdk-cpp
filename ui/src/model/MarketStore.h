#pragma once

#include "net/Protocol.h"

#include <QHash>
#include <QObject>
#include <QString>

namespace tqsdk::ui {

/// UI 侧行情截面。
///
/// 只保存展示所需的最新 quote 和最近 K 线；不是 node DataManager 的复制品。
class MarketStore : public QObject {
    Q_OBJECT

public:
    explicit MarketStore(QObject* parent = nullptr);

    const Quote*      quote(const QString& symbol) const;
    QVector<KlineBar> bars(const QString& symbol, qint64 durationNs) const;

    void applyQuotes(const QVector<Quote>& quotes);
    void applyKline(const QString& symbol, qint64 durationNs, const KlineFrame& frame);

signals:
    void quotesChanged();
    void klineChanged(const QString& symbol, qint64 durationNs);

private:
    QHash<QString, Quote>                            quotes_;
    QHash<QString, QHash<qint64, QVector<KlineBar>>> klines_;
    int                                              maxBars_ = 2000;
};

} // namespace tqsdk::ui
