#pragma once

#include "net/Protocol.h"

#include <QObject>
#include <QString>

namespace tqsdk::ui {

class NodeClient;
class OrderTableModel;
class OrderTicketPanel;
class OrdersPanel;
class PositionTableModel;

/// 交易动作协调器：把三键按钮、持仓/委托模型和 NodeClient 连接起来。
///
/// MainWindow 不负责构造 OrderRequest、判断平仓方向或维护按钮可用状态。
class TradingController final : public QObject {
    Q_OBJECT

public:
    TradingController(NodeClient*         nodeClient,
                      OrderTicketPanel*   ticket,
                      PositionTableModel* positions,
                      OrderTableModel*    orders,
                      OrdersPanel*        ordersPanel,
                      QObject*            parent = nullptr);

    void setSelectedSymbol(const QString& symbol);
    void setNodeReady(bool ready);
    void setAccountLoaded(bool loaded);
    void updateControls();
    void submit(Direction direction, Offset offset);
    void closePosition();
    void cancelSelectedOrder();
    void handleOrderAck(const OrderAck& ack);

signals:
    /// ack 后请求刷新委托/持仓/账户。
    void refreshRequested();
    /// 撤单命令被拒绝时上报给 MainWindow 做 Toast/状态提示。
    void cancelFailed(const QString& message);

private:
    NodeClient*         nodeClient_  = nullptr;
    OrderTicketPanel*   ticket_      = nullptr;
    PositionTableModel* positions_   = nullptr;
    OrderTableModel*    orders_      = nullptr;
    OrdersPanel*        ordersPanel_ = nullptr;
    QString             selectedSymbol_;
    bool                nodeReady_     = false;
    bool                accountLoaded_ = false;
    QString             pendingCancelId_;
};

} // namespace tqsdk::ui
