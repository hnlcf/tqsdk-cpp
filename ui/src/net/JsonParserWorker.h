#pragma once

#include "net/Protocol.h"

#include <QByteArray>
#include <QObject>

class QTimer;

namespace tqsdk::ui {

/// 专用 JSON 解析线程对象。
///
/// WebSocket 和较大的 REST 响应不在这里之外的 UI 线程解析；市场增量在
/// worker 内按约 30Hz 合并后只向 UI 发送一次，避免消息洪峰阻塞绘制/输入。
class JsonParserWorker final : public QObject {
    Q_OBJECT

public:
    explicit JsonParserWorker(QObject* parent = nullptr);

public slots:
    void stop();
    void parseWsMessage(QByteArray payload);
    void parseCatalogPayload(QByteArray payload);
    void parseKlinePayload(QByteArray payload, QString symbol, qint64 durationNs);
    void parseOptionPayload(QByteArray payload);

signals:
    void wsSubscribed();
    void wsSnapshot();
    void wsError(const QString& error);
    void marketUpdateParsed(const tqsdk::ui::MarketUpdate& update);
    void catalogParsed(const QVector<tqsdk::ui::Instrument>& instruments);
    void klineHistoryParsed(const QString&                      symbol,
                            qint64                              durationNs,
                            const QVector<tqsdk::ui::KlineBar>& bars);
    void optionChainParsed(const tqsdk::ui::OptionChain& chain);
    void parseError(const QString& error);

private:
    void enqueueMarketUpdate(const MarketUpdate& update);
    void flushMarketUpdate();
    void mergeKlineFrame(KlineFrame& target, const KlineFrame& incoming);

    MarketUpdate pending_;
    QTimer*      flushTimer_ = nullptr;
};

} // namespace tqsdk::ui
