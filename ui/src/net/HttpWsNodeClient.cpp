#include "net/HttpWsNodeClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QAbstractSocket>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QWebSocketProtocol>

#include <algorithm>

namespace tqsdk::ui {
namespace {

QString replyError(QNetworkReply* reply, int status) {
    if (reply->error() != QNetworkReply::NoError) {
        return reply->errorString();
    }
    if (status < 200 || status >= 300) {
        return QStringLiteral("node HTTP %1").arg(status);
    }
    return {};
}

} // namespace

HttpWsNodeClient::HttpWsNodeClient(QObject* parent)
    : NodeClient(parent),
      network_(this),
      socket_(QString(), QWebSocketProtocol::VersionLatest, this),
      reconnectTimer_(this) {
    reconnectTimer_.setSingleShot(true);

    connect(&socket_, &QWebSocket::connected, this, [this] {
        reconnectTimer_.stop();
        reconnectDelayMs_ = 1000;
        setState(State::Connected, QStringLiteral("WebSocket 已连接"));
        sendSubscribe();
    });
    connect(&socket_, &QWebSocket::disconnected, this, [this] {
        if (manualDisconnect_) {
            setState(State::Disconnected, QStringLiteral("已断开"));
            return;
        }
        scheduleReconnect();
    });
    connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString& text) {
        QJsonParseError error;
        const auto      document = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            emit errorOccurred(QStringLiteral("invalid node WS JSON: %1").arg(error.errorString()));
            return;
        }
        handleEnvelope(document.object());
    });
    connect(&socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(QStringLiteral("WebSocket: %1").arg(socket_.errorString()));
        if (!manualDisconnect_ && socket_.state() != QAbstractSocket::ConnectedState) {
            scheduleReconnect();
        }
    });
    connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
        if (!manualDisconnect_ && socket_.state() == QAbstractSocket::UnconnectedState) {
            openSocket();
        }
    });
}

HttpWsNodeClient::~HttpWsNodeClient() {
    manualDisconnect_ = true;
    reconnectTimer_.stop();
    socket_.abort();
}

void HttpWsNodeClient::connectToNode(const QUrl& nodeUrl) {
    const QString scheme = nodeUrl.scheme().toLower();
    if (!nodeUrl.isValid() || nodeUrl.host().isEmpty() ||
        (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
        setState(State::Disconnected, QStringLiteral("无效 node URL"));
        emit errorOccurred(QStringLiteral("node URL 必须是 http(s)://host:port"));
        return;
    }

    nodeUrl_          = nodeUrl;
    manualDisconnect_ = false;
    reconnectDelayMs_ = 1000;
    reconnectTimer_.stop();
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.abort();
    }
    openSocket();
}

void HttpWsNodeClient::disconnectFromNode() {
    manualDisconnect_ = true;
    reconnectTimer_.stop();
    socket_.close();
    setState(State::Disconnected, QStringLiteral("手动断开"));
}

void HttpWsNodeClient::openSocket() {
    if (manualDisconnect_ || nodeUrl_.isEmpty()) {
        return;
    }
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    setState(state() == State::Reconnecting ? State::Reconnecting : State::Connecting,
             QStringLiteral("连接 node %1").arg(streamUrl().toString()));
    socket_.open(streamUrl());
}

void HttpWsNodeClient::scheduleReconnect() {
    if (manualDisconnect_ || reconnectTimer_.isActive()) {
        return;
    }
    setState(State::Reconnecting, QStringLiteral("行情断开，%1ms 后重连").arg(reconnectDelayMs_));
    reconnectTimer_.start(reconnectDelayMs_);
    reconnectDelayMs_ = std::min(reconnectDelayMs_ * 2, 30'000);
}

void HttpWsNodeClient::setSubscription(const Subscription& subscription) {
    subscription_ = subscription;
    if (socket_.state() == QAbstractSocket::ConnectedState) {
        sendSubscribe();
    }
}

void HttpWsNodeClient::sendSubscribe() {
    if (socket_.state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const QJsonDocument document(makeSubscribe(subscription_));
    socket_.sendTextMessage(QString::fromUtf8(document.toJson(QJsonDocument::Compact)));
}

void HttpWsNodeClient::handleEnvelope(const QJsonObject& envelope) {
    const QString type = envelope.value("type").toString();
    if (type == QStringLiteral("error")) {
        emit errorOccurred(envelope.value("error").toString());
        return;
    }
    if (type == QStringLiteral("welcome")) {
        return;
    }
    if (type == QStringLiteral("subscribed")) {
        setState(State::Ready, QStringLiteral("已订阅"));
        return;
    }
    if (type == QStringLiteral("snapshot")) {
        setState(State::Ready, QStringLiteral("已同步快照"));
        if (envelope.value("data").isObject()) {
            handleMarketData(envelope.value("data").toObject());
        }
        return;
    }
    if (type == QStringLiteral("diff")) {
        if (envelope.value("data").isObject()) {
            handleMarketData(envelope.value("data").toObject());
        }
        return;
    }
    if (type == QStringLiteral("pong")) {
        return;
    }
    emit errorOccurred(QStringLiteral("unknown node WS message type: %1").arg(type));
}

void HttpWsNodeClient::handleMarketData(const QJsonObject& data) {
    emitMarketUpdate(parseMarketData(data));
}

void HttpWsNodeClient::emitMarketUpdate(const MarketUpdate& update) {
    if (!update.quotes.isEmpty()) {
        emit quotesUpdated(update.quotes);
    }
    for (auto sit = update.klines.constBegin(); sit != update.klines.constEnd(); ++sit) {
        for (auto dit = sit.value().constBegin(); dit != sit.value().constEnd(); ++dit) {
            emit klineUpdated(sit.key(), dit.key(), dit.value());
        }
    }
    if (update.hasAccount) {
        emit accountUpdated(update.account);
    }
    if (!update.positions.isEmpty()) {
        emit positionsUpdated(update.positions);
    }
    if (!update.orders.isEmpty()) {
        emit ordersUpdated(update.orders);
    }
    if (!update.trades.isEmpty()) {
        emit tradesUpdated(update.trades);
    }
}

QUrl HttpWsNodeClient::apiUrl(const QString& path) const {
    QUrl    url  = nodeUrl_;
    QString base = url.path();
    if (base.isEmpty()) {
        base = QStringLiteral("/");
    }
    if (!base.endsWith('/')) {
        base += '/';
    }
    QString relative = path;
    while (relative.startsWith('/')) {
        relative.remove(0, 1);
    }
    url.setPath(base + relative);
    url.setQuery({});
    return url;
}

QUrl HttpWsNodeClient::streamUrl() const {
    QUrl url = nodeUrl_;
    url.setScheme(url.scheme().toLower() == QStringLiteral("https") ? QStringLiteral("wss")
                                                                    : QStringLiteral("ws"));
    url.setPath(QStringLiteral("/stream"));
    url.setQuery({});
    return url;
}

void HttpWsNodeClient::getJson(const QUrl& url, JsonCallback callback) {
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    auto* reply = network_.get(request);

    connect(
        reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() mutable {
            const QByteArray body = reply->readAll();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString error = replyError(reply, status);
            reply->deleteLater();
            if (!error.isEmpty()) {
                callback(QJsonObject{}, error);
                return;
            }
            QJsonParseError parseError;
            const auto      document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                callback(QJsonObject{},
                         QStringLiteral("invalid node JSON: %1").arg(parseError.errorString()));
                return;
            }
            callback(document.object(), {});
        });
}

void HttpWsNodeClient::sendJson(QNetworkAccessManager::Operation method,
                                const QUrl&                      url,
                                const QJsonObject&               body,
                                JsonCallback                     callback) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = nullptr;
    if (method == QNetworkAccessManager::PostOperation) {
        reply = network_.post(request, payload);
    } else if (method == QNetworkAccessManager::DeleteOperation) {
        reply = network_.deleteResource(request);
    } else {
        if (callback) {
            callback(QJsonObject{}, QStringLiteral("unsupported HTTP operation"));
        }
        return;
    }

    connect(
        reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() mutable {
            const QByteArray body = reply->readAll();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString error = replyError(reply, status);
            reply->deleteLater();
            if (!error.isEmpty()) {
                callback(QJsonObject{}, error);
                return;
            }
            QJsonParseError parseError;
            const auto      document = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                callback(QJsonObject{},
                         QStringLiteral("invalid node JSON: %1").arg(parseError.errorString()));
                return;
            }
            callback(document.object(), {});
        });
}

void HttpWsNodeClient::requestCatalog(const CatalogQuery& query) {
    QUrl      url = apiUrl(QStringLiteral("/catalog/instruments"));
    QUrlQuery q;
    if (!query.classes.isEmpty()) {
        q.addQueryItem("classes", query.classes);
    }
    if (!query.exchange.isEmpty()) {
        q.addQueryItem("exchange", query.exchange);
    }
    if (!query.product.isEmpty()) {
        q.addQueryItem("product", query.product);
    }
    if (!query.query.isEmpty()) {
        q.addQueryItem("q", query.query);
    }
    if (!query.underlying.isEmpty()) {
        q.addQueryItem("underlying", query.underlying);
    }
    if (query.includeExpired) {
        q.addQueryItem("include_expired", "1");
    }
    q.addQueryItem("limit", QString::number(query.limit));
    q.addQueryItem("offset", QString::number(query.offset));
    url.setQuery(q);

    getJson(url, [this](const QJsonObject& response, const QString& error) {
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            return;
        }
        emit catalogLoaded(parseCatalog(response));
    });
}

void HttpWsNodeClient::requestKlineHistory(const QString& symbol,
                                           qint64         durationNs,
                                           int            viewWidth) {
    (void)viewWidth; // node 当前 REST 接口使用默认 200；扩展 query 后再接入。
    const QString encodedSymbol = QString::fromLatin1(QUrl::toPercentEncoding(symbol));
    const QUrl    url = apiUrl(QStringLiteral("/kline/%1/%2").arg(encodedSymbol).arg(durationNs));
    getJson(url, [this, symbol, durationNs](const QJsonObject& response, const QString& error) {
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            return;
        }
        emit klineHistoryLoaded(symbol, durationNs, parseKlineHistory(response));
    });
}

void HttpWsNodeClient::requestAccountSnapshot() {
    getJson(apiUrl(QStringLiteral("/account")),
            [this](const QJsonObject& response, const QString& error) {
                if (!error.isEmpty()) {
                    emit errorOccurred(error);
                    return;
                }
                if (auto account = parseAccount(response)) {
                    emit accountUpdated(*account);
                }
            });
}

void HttpWsNodeClient::requestOrders() {
    getJson(apiUrl(QStringLiteral("/orders")),
            [this](const QJsonObject& response, const QString& error) {
                if (!error.isEmpty()) {
                    emit errorOccurred(error);
                    return;
                }
                auto update = parseMarketData(response);
                if (!update.orders.isEmpty()) {
                    emit ordersUpdated(update.orders);
                }
            });
}

void HttpWsNodeClient::requestPositions() {
    getJson(apiUrl(QStringLiteral("/positions")),
            [this](const QJsonObject& response, const QString& error) {
                if (!error.isEmpty()) {
                    emit errorOccurred(error);
                    return;
                }
                auto update = parseMarketData(response);
                if (!update.positions.isEmpty()) {
                    emit positionsUpdated(update.positions);
                }
            });
}

void HttpWsNodeClient::requestTrades() {
    getJson(apiUrl(QStringLiteral("/trades")),
            [this](const QJsonObject& response, const QString& error) {
                if (!error.isEmpty()) {
                    emit errorOccurred(error);
                    return;
                }
                auto update = parseMarketData(response);
                if (!update.trades.isEmpty()) {
                    emit tradesUpdated(update.trades);
                }
            });
}

void HttpWsNodeClient::placeOrder(const OrderRequest& request) {
    sendJson(QNetworkAccessManager::PostOperation,
             apiUrl(QStringLiteral("/order")),
             makeOrder(request),
             [this](const QJsonObject& response, const QString& error) {
                 if (!error.isEmpty()) {
                     emit errorOccurred(error);
                     return;
                 }
                 if (auto ack = parseOrderAck(response)) {
                     if (!ack->success && !ack->error.isEmpty()) {
                         emit errorOccurred(ack->error);
                     }
                     emit orderAckReceived(*ack);
                 }
             });
}

void HttpWsNodeClient::cancelOrder(const QString& orderId) {
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(orderId));
    sendJson(QNetworkAccessManager::DeleteOperation,
             apiUrl(QStringLiteral("/order/%1").arg(encoded)),
             {},
             [this](const QJsonObject& response, const QString& error) {
                 if (!error.isEmpty()) {
                     emit errorOccurred(error);
                     return;
                 }
                 if (auto ack = parseOrderAck(response)) {
                     if (!ack->success && !ack->error.isEmpty()) {
                         emit errorOccurred(ack->error);
                     }
                     emit orderAckReceived(*ack);
                 }
             });
}

} // namespace tqsdk::ui
