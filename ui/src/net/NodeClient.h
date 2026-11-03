#pragma once

#include "net/Protocol.h"

#include <QObject>
#include <QUrl>

namespace tqsdk::ui {

/// 前端唯一的数据/交易入口抽象。
///
/// 页面、模型、图表只依赖 NodeClient，不依赖 HTTP/WS/QWebSocket。
/// 当前实现是 HttpWsNodeClient；后续可以增加 UDS/二进制或嵌入式实现，
/// 不需要改页面代码。
class NodeClient : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Ready,
        Reconnecting,
    };
    Q_ENUM(State)

    explicit NodeClient(QObject* parent = nullptr);
    ~NodeClient() override = default;

    virtual void connectToNode(const QUrl& nodeUrl) = 0;
    virtual void disconnectFromNode() = 0;

    /// 合并/替换当前订阅集。实现负责在重连后重新发送。
    virtual void setSubscription(const Subscription& subscription) = 0;

    /// 一次性快照查询；结果通过 *Loaded 信号返回。
    virtual void requestCatalog(const CatalogQuery& query) = 0;
    virtual void requestKlineHistory(const QString& symbol,
                                     qint64         durationNs,
                                     int            viewWidth) = 0;
    virtual void requestAccountSnapshot() = 0;
    virtual void requestOrders() = 0;
    virtual void requestPositions() = 0;
    virtual void requestTrades() = 0;

    virtual void placeOrder(const OrderRequest& request) = 0;
    virtual void cancelOrder(const QString& orderId) = 0;

    State state() const noexcept { return state_; }

signals:
    void stateChanged(tqsdk::ui::NodeClient::State state, const QString& message);

    void quotesUpdated(const QVector<tqsdk::ui::Quote>& quotes);
    void klineUpdated(const QString& symbol, qint64 durationNs, const tqsdk::ui::KlineFrame& frame);

    void accountUpdated(const tqsdk::ui::Account& account);
    void positionsUpdated(const QVector<tqsdk::ui::Position>& positions);
    void ordersUpdated(const QVector<tqsdk::ui::Order>& orders);
    void tradesUpdated(const QVector<tqsdk::ui::Trade>& trades);

    void catalogLoaded(const QVector<tqsdk::ui::Instrument>& instruments);
    void klineHistoryLoaded(const QString& symbol,
                            qint64         durationNs,
                            const QVector<tqsdk::ui::KlineBar>& bars);
    void orderAckReceived(const tqsdk::ui::OrderAck& ack);
    void errorOccurred(const QString& error);

protected:
    void setState(State state, const QString& message = {});

private:
    State state_ = State::Disconnected;
};

} // namespace tqsdk::ui

Q_DECLARE_METATYPE(tqsdk::ui::NodeClient::State)
Q_DECLARE_METATYPE(tqsdk::ui::Quote)
Q_DECLARE_METATYPE(tqsdk::ui::KlineFrame)
Q_DECLARE_METATYPE(tqsdk::ui::KlineBar)
Q_DECLARE_METATYPE(tqsdk::ui::Instrument)
Q_DECLARE_METATYPE(tqsdk::ui::Account)
Q_DECLARE_METATYPE(tqsdk::ui::Position)
Q_DECLARE_METATYPE(tqsdk::ui::Order)
Q_DECLARE_METATYPE(tqsdk::ui::Trade)
Q_DECLARE_METATYPE(tqsdk::ui::OrderAck)
