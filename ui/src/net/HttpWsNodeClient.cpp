#include "net/HttpWsNodeClient.h"

#include "net/JsonParserWorker.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonObject>
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

    qRegisterMetaType<MarketUpdate>("tqsdk::ui::MarketUpdate");
    qRegisterMetaType<OptionChain>("tqsdk::ui::OptionChain");
    qRegisterMetaType<QVector<Instrument>>("QVector<tqsdk::ui::Instrument>");
    qRegisterMetaType<QVector<KlineBar>>("QVector<tqsdk::ui::KlineBar>");
    qRegisterMetaType<Quote>("tqsdk::ui::Quote");
    qRegisterMetaType<Account>("tqsdk::ui::Account");
    qRegisterMetaType<OrderAck>("tqsdk::ui::OrderAck");

    parser_ = new JsonParserWorker;
    parser_->moveToThread(&parserThread_);
    connect(&parserThread_, &QThread::finished, parser_, &QObject::deleteLater);
    connect(
        parser_,
        &JsonParserWorker::marketUpdateParsed,
        this,
        [this](const MarketUpdate& update) { emitMarketUpdate(update); },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::wsSubscribed,
        this,
        [this] { setState(State::Ready, QStringLiteral("已订阅")); },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::wsSnapshot,
        this,
        [this] { setState(State::Ready, QStringLiteral("已同步快照")); },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::wsError,
        this,
        [this](const QString& error) { emit errorOccurred(error); },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::parseError,
        this,
        [this](const QString& error) { emit errorOccurred(error); },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::catalogParsed,
        this,
        [this](const QVector<Instrument>& instruments) { emit catalogLoaded(instruments); },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::klineHistoryParsed,
        this,
        [this](const QString& symbol, qint64 duration, const QVector<KlineBar>& bars) {
            emit klineHistoryLoaded(symbol, duration, bars);
        },
        Qt::QueuedConnection);
    connect(
        parser_,
        &JsonParserWorker::optionChainParsed,
        this,
        [this](const OptionChain& chain) { emit optionChainLoaded(chain); },
        Qt::QueuedConnection);
    parserThread_.start();

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
        enqueueWsPayload(text.toUtf8());
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
    if (parser_ != nullptr && parserThread_.isRunning()) {
        QMetaObject::invokeMethod(parser_, "stop", Qt::BlockingQueuedConnection);
    }
    parserThread_.quit();
    parserThread_.wait(2000);
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
    wsInputQueue_.clear();
    wsInFlight_       = false;
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
    wsInputQueue_.clear();
    wsInFlight_ = false;
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

void HttpWsNodeClient::enqueueWsPayload(const QByteArray& payload) {
    if (parser_ == nullptr) {
        return;
    }
    // 有界原始报文队列：worker 处理完一条才投递下一条，防止网络洪峰把
    // worker 的 Qt 事件队列无限撑大；极端情况下丢弃最旧报文。
    constexpr int kMaxQueuedWsPayloads = 8192;
    if (wsInputQueue_.size() >= kMaxQueuedWsPayloads) {
        wsInputQueue_.dequeue();
        ++wsDropped_;
    }
    wsInputQueue_.enqueue(payload);
    dispatchNextWsPayload();
}

void HttpWsNodeClient::dispatchNextWsPayload() {
    if (parser_ == nullptr || wsInFlight_ || wsInputQueue_.isEmpty()) {
        return;
    }
    const QByteArray payload = wsInputQueue_.dequeue();
    wsInFlight_              = true;
    const bool scheduled     = QMetaObject::invokeMethod(
        parser_, "parseWsMessage", Qt::QueuedConnection, Q_ARG(QByteArray, payload));
    if (!scheduled) {
        wsInFlight_ = false;
    }
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
    url.setQuery(QString{});
    return url;
}

QUrl HttpWsNodeClient::streamUrl() const {
    QUrl url = nodeUrl_;
    url.setScheme(url.scheme().toLower() == QStringLiteral("https") ? QStringLiteral("wss")
                                                                    : QStringLiteral("ws"));
    url.setPath(QStringLiteral("/stream"));
    url.setQuery(QString{});
    return url;
}

void HttpWsNodeClient::getRawJson(const QUrl& url, RawCallback callback) {
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
                callback({}, error);
                return;
            }
            callback(body, {});
        });
}

void HttpWsNodeClient::sendRawJson(QNetworkAccessManager::Operation method,
                                   const QUrl&                      url,
                                   const QJsonObject&               body,
                                   RawCallback                      callback) {
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
            callback({}, QStringLiteral("unsupported HTTP operation"));
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
                callback({}, error);
                return;
            }
            callback(body, {});
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

    getRawJson(url, [this](const QByteArray& body, const QString& error) {
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            return;
        }
        if (parser_ != nullptr) {
            QMetaObject::invokeMethod(
                parser_, "parseCatalogPayload", Qt::QueuedConnection, Q_ARG(QByteArray, body));
        }
    });
}

void HttpWsNodeClient::requestQuotes(const QStringList& symbols) {
    for (const QString& symbol : symbols) {
        if (symbol.isEmpty()) {
            continue;
        }
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(symbol));
        getRawJson(apiUrl(QStringLiteral("/quote/%1").arg(encoded)),
                   [this, symbol](const QByteArray& body, const QString& error) {
                       if (!error.isEmpty()) {
                           emit errorOccurred(error);
                           return;
                       }
                       if (parser_ != nullptr) {
                           QMetaObject::invokeMethod(parser_,
                                                     "parseQuotePayload",
                                                     Qt::QueuedConnection,
                                                     Q_ARG(QByteArray, body),
                                                     Q_ARG(QString, symbol));
                       }
                   });
    }
}

void HttpWsNodeClient::requestOptionChain(const QString& underlying) {
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(underlying));
    getRawJson(apiUrl(QStringLiteral("/catalog/option-chain/%1").arg(encoded)),
               [this](const QByteArray& body, const QString& error) {
                   if (!error.isEmpty()) {
                       emit errorOccurred(error);
                       return;
                   }
                   if (parser_ != nullptr) {
                       QMetaObject::invokeMethod(parser_,
                                                 "parseOptionPayload",
                                                 Qt::QueuedConnection,
                                                 Q_ARG(QByteArray, body));
                   }
               });
}

void HttpWsNodeClient::requestKlineHistory(const QString& symbol,
                                           qint64         durationNs,
                                           int            viewWidth) {
    (void)viewWidth; // node 当前 REST 接口使用默认 200；扩展 query 后再接入。
    const QString encodedSymbol = QString::fromLatin1(QUrl::toPercentEncoding(symbol));
    const QUrl    url = apiUrl(QStringLiteral("/kline/%1/%2").arg(encodedSymbol).arg(durationNs));
    getRawJson(url, [this, symbol, durationNs](const QByteArray& body, const QString& error) {
        if (!error.isEmpty()) {
            emit errorOccurred(error);
            return;
        }
        if (parser_ != nullptr) {
            QMetaObject::invokeMethod(parser_,
                                      "parseKlinePayload",
                                      Qt::QueuedConnection,
                                      Q_ARG(QByteArray, body),
                                      Q_ARG(QString, symbol),
                                      Q_ARG(qint64, durationNs));
        }
    });
}

void HttpWsNodeClient::requestAccountSnapshot() {
    getRawJson(
        apiUrl(QStringLiteral("/account")), [this](const QByteArray& body, const QString& error) {
            if (!error.isEmpty()) {
                emit errorOccurred(error);
                return;
            }
            if (parser_ != nullptr) {
                QMetaObject::invokeMethod(
                    parser_, "parseAccountPayload", Qt::QueuedConnection, Q_ARG(QByteArray, body));
            }
        });
}

void HttpWsNodeClient::requestOrders() {
    getRawJson(
        apiUrl(QStringLiteral("/orders")), [this](const QByteArray& body, const QString& error) {
            if (!error.isEmpty()) {
                emit errorOccurred(error);
                return;
            }
            if (parser_ != nullptr) {
                QMetaObject::invokeMethod(
                    parser_, "parseMarketPayload", Qt::QueuedConnection, Q_ARG(QByteArray, body));
            }
        });
}

void HttpWsNodeClient::requestPositions() {
    getRawJson(
        apiUrl(QStringLiteral("/positions")), [this](const QByteArray& body, const QString& error) {
            if (!error.isEmpty()) {
                emit errorOccurred(error);
                return;
            }
            if (parser_ != nullptr) {
                QMetaObject::invokeMethod(
                    parser_, "parseMarketPayload", Qt::QueuedConnection, Q_ARG(QByteArray, body));
            }
        });
}

void HttpWsNodeClient::requestTrades() {
    getRawJson(
        apiUrl(QStringLiteral("/trades")), [this](const QByteArray& body, const QString& error) {
            if (!error.isEmpty()) {
                emit errorOccurred(error);
                return;
            }
            if (parser_ != nullptr) {
                QMetaObject::invokeMethod(
                    parser_, "parseMarketPayload", Qt::QueuedConnection, Q_ARG(QByteArray, body));
            }
        });
}

void HttpWsNodeClient::placeOrder(const OrderRequest& request) {
    sendRawJson(QNetworkAccessManager::PostOperation,
                apiUrl(QStringLiteral("/order")),
                makeOrder(request),
                [this](const QByteArray& body, const QString& error) {
                    if (!error.isEmpty()) {
                        emit errorOccurred(error);
                        return;
                    }
                    if (parser_ != nullptr) {
                        QMetaObject::invokeMethod(parser_,
                                                  "parseOrderAckPayload",
                                                  Qt::QueuedConnection,
                                                  Q_ARG(QByteArray, body));
                    }
                });
}

void HttpWsNodeClient::cancelOrder(const QString& orderId) {
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(orderId));
    sendRawJson(QNetworkAccessManager::DeleteOperation,
                apiUrl(QStringLiteral("/order/%1").arg(encoded)),
                {},
                [this](const QByteArray& body, const QString& error) {
                    if (!error.isEmpty()) {
                        emit errorOccurred(error);
                        return;
                    }
                    if (parser_ != nullptr) {
                        QMetaObject::invokeMethod(parser_,
                                                  "parseOrderAckPayload",
                                                  Qt::QueuedConnection,
                                                  Q_ARG(QByteArray, body));
                    }
                });
}

} // namespace tqsdk::ui
