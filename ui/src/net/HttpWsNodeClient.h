#pragma once

#include "net/NodeClient.h"

#include <QNetworkAccessManager>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>

class QNetworkReply;
namespace tqsdk::ui {
class JsonParserWorker;
} // namespace tqsdk::ui

namespace tqsdk::ui {

/// 方案 A 的首个 NodeClient 实现。
///
/// 数据面全部经过 HTTP/WS；UI 不链接 tqsdk_core。
///
/// 线程模型：QWebSocket/QNetworkAccessManager 在 GUI 线程收包；收到的
/// QByteArray 交给 JsonParserWorker（QThread）解析，worker 通过 queued signal
/// 把强类型快照/增量送回 GUI。GUI 线程不执行入站 JSON 解析。
class HttpWsNodeClient final : public NodeClient {
    Q_OBJECT

public:
    explicit HttpWsNodeClient(QObject* parent = nullptr);
    ~HttpWsNodeClient() override;

    void connectToNode(const QUrl& nodeUrl) override;
    void disconnectFromNode() override;

    void setSubscription(const Subscription& subscription) override;

    void requestCatalog(const CatalogQuery& query) override;
    void requestQuotes(const QStringList& symbols) override;
    void requestOptionChain(const QString& underlying) override;
    void requestKlineHistory(const QString& symbol, qint64 durationNs, int viewWidth) override;
    void requestAccountSnapshot() override;
    void requestOrders() override;
    void requestPositions() override;
    void requestTrades() override;
    void requestTradingSnapshot() override;

    void placeOrder(const OrderRequest& request) override;
    void cancelOrder(const QString& orderId) override;

private:
    void openSocket();
    void scheduleReconnect();
    void sendSubscribe();
    void emitMarketUpdate(const MarketUpdate& update);
    void enqueueWsPayload(const QByteArray& payload);
    void dispatchNextWsPayload();

    QUrl apiUrl(const QString& path) const;
    QUrl streamUrl() const;

    using RawCallback = std::function<void(const QByteArray&, const QString&)>;
    void getRawJson(const QUrl& url, RawCallback callback);
    void sendRawJson(QNetworkAccessManager::Operation method,
                     const QUrl&                      url,
                     const QJsonObject&               body,
                     RawCallback                      callback);

    QNetworkAccessManager network_;
    QThread               parserThread_;
    JsonParserWorker*     parser_ = nullptr;
    QQueue<QByteArray>    wsInputQueue_;
    bool                  wsInFlight_ = false;
    quint64               wsDropped_  = 0;
    QWebSocket            socket_;
    QTimer                reconnectTimer_;
    QUrl                  nodeUrl_;
    Subscription          subscription_;
    int                   reconnectDelayMs_ = 1000;
    bool                  manualDisconnect_ = false;
};

} // namespace tqsdk::ui
