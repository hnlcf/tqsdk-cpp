#pragma once

#include "net/NodeClient.h"

#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>

class QNetworkReply;

namespace tqsdk::ui {

/// 方案 A 的首个 NodeClient 实现。
///
/// 数据面全部经过 HTTP/WS；UI 不链接 tqsdk_core。
class HttpWsNodeClient final : public NodeClient {
    Q_OBJECT

public:
    explicit HttpWsNodeClient(QObject* parent = nullptr);
    ~HttpWsNodeClient() override;

    void connectToNode(const QUrl& nodeUrl) override;
    void disconnectFromNode() override;

    void setSubscription(const Subscription& subscription) override;

    void requestCatalog(const CatalogQuery& query) override;
    void requestKlineHistory(const QString& symbol,
                             qint64         durationNs,
                             int            viewWidth) override;
    void requestAccountSnapshot() override;
    void requestOrders() override;
    void requestPositions() override;
    void requestTrades() override;

    void placeOrder(const OrderRequest& request) override;
    void cancelOrder(const QString& orderId) override;

private:
    void openSocket();
    void scheduleReconnect();
    void sendSubscribe();
    void handleEnvelope(const QJsonObject& envelope);
    void handleMarketData(const QJsonObject& data);
    void emitMarketUpdate(const MarketUpdate& update);

    QUrl apiUrl(const QString& path) const;
    QUrl streamUrl() const;

    using JsonCallback = std::function<void(const QJsonObject&, const QString&)>;
    void getJson(const QUrl& url, JsonCallback callback);
    void sendJson(QNetworkAccessManager::Operation method,
                  const QUrl&                        url,
                  const QJsonObject&                 body,
                  JsonCallback                       callback);

    QNetworkAccessManager network_;
    QWebSocket          socket_;
    QTimer              reconnectTimer_;
    QUrl                nodeUrl_;
    Subscription        subscription_;
    int                 reconnectDelayMs_ = 1000;
    bool                manualDisconnect_ = false;
};

} // namespace tqsdk::ui
