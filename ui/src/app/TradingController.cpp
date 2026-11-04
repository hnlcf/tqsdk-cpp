#include "app/TradingController.h"

#include "model/Formatting.h"
#include "model/TradingModels.h"
#include "net/NodeClient.h"
#include "widgets/OrderTicketPanel.h"
#include "widgets/TradingPanels.h"

#include <QTableView>

namespace tqsdk::ui {

TradingController::TradingController(NodeClient*         nodeClient,
                                     OrderTicketPanel*   ticket,
                                     PositionTableModel* positions,
                                     OrderTableModel*    orders,
                                     OrdersPanel*        ordersPanel,
                                     QObject*            parent)
    : QObject(parent),
      nodeClient_(nodeClient),
      ticket_(ticket),
      positions_(positions),
      orders_(orders),
      ordersPanel_(ordersPanel) {
    updateControls();
}

void TradingController::setSelectedSymbol(const QString& symbol) {
    selectedSymbol_ = symbol;
    updateControls();
}

void TradingController::setNodeReady(bool ready) {
    nodeReady_ = ready;
    updateControls();
}

void TradingController::setAccountLoaded(bool loaded) {
    accountLoaded_ = loaded;
    updateControls();
}

void TradingController::updateControls() {
    if (ticket_ == nullptr) {
        return;
    }
    const bool canSubmit = nodeReady_ && accountLoaded_;
    ticket_->setTradingEnabled(canSubmit);

    qint64 longVolume  = 0;
    qint64 shortVolume = 0;
    if (positions_ != nullptr) {
        for (const auto& position : positions_->positions()) {
            if (position.instrumentId != selectedSymbol_) {
                continue;
            }
            longVolume += position.volumeLong;
            shortVolume += position.volumeShort;
        }
    }
    ticket_->setCloseEnabled(canSubmit && (longVolume > 0 || shortVolume > 0));

    if (ordersPanel_ != nullptr && orders_ != nullptr) {
        const auto current = ordersPanel_->view()->currentIndex();
        ordersPanel_->setCancelEnabled(current.isValid() && orders_->isOrderAlive(current.row()));
    }
}

void TradingController::submit(Direction direction, Offset offset) {
    if (ticket_ == nullptr) {
        return;
    }
    if (nodeClient_ == nullptr || !nodeReady_ || !accountLoaded_) {
        ticket_->setStatus(tr("交易未就绪"), true);
        return;
    }
    if (ticket_->volume() <= 0) {
        ticket_->setStatus(tr("数量无效"), true);
        return;
    }

    PriceType priceType = PriceType::Limit;
    if (ticket_->marketOrder()) {
        priceType = PriceType::Market;
    } else if (ticket_->counterpartyOrder()) {
        priceType = PriceType::Best;
    } else if (ticket_->price() <= 0.0) {
        ticket_->setStatus(tr("限价单价格无效"), true);
        return;
    }

    OrderRequest request;
    request.instrumentId = selectedSymbol_;
    request.direction    = direction;
    request.offset       = offset;
    request.priceType    = priceType;
    request.limitPrice   = priceType == PriceType::Limit ? ticket_->price() : 0.0;
    request.volume       = ticket_->volume();
    nodeClient_->placeOrder(request);
    ticket_->setStatus(
        tr("已提交 %1 %2 手 %3")
            .arg(selectedSymbol_)
            .arg(request.volume)
            .arg(
                priceType == PriceType::Market
                    ? tr("市价")
                    : (priceType == PriceType::Best ? tr("对价") : priceText(request.limitPrice))));
    updateControls();
}

void TradingController::closePosition() {
    qint64 longVolume  = 0;
    qint64 shortVolume = 0;
    if (positions_ != nullptr) {
        for (const auto& position : positions_->positions()) {
            if (position.instrumentId != selectedSymbol_) {
                continue;
            }
            longVolume += position.volumeLong;
            shortVolume += position.volumeShort;
        }
    }
    if (longVolume > 0) {
        submit(Direction::Sell, Offset::Close);
    } else if (shortVolume > 0) {
        submit(Direction::Buy, Offset::Close);
    } else if (ticket_ != nullptr) {
        ticket_->setStatus(tr("当前合约无持仓"), true);
    }
}

void TradingController::cancelSelectedOrder() {
    if (nodeClient_ == nullptr || ordersPanel_ == nullptr || orders_ == nullptr) {
        return;
    }
    const auto current = ordersPanel_->view()->currentIndex();
    if (!current.isValid() || !orders_->isOrderAlive(current.row())) {
        return;
    }
    const QString orderId = orders_->orderIdAt(current.row());
    if (orderId.isEmpty()) {
        return;
    }
    nodeClient_->cancelOrder(orderId);
    if (ticket_ != nullptr) {
        ticket_->setStatus(tr("撤单请求: %1").arg(orderId));
    }
}

void TradingController::handleOrderAck(const OrderAck& ack) {
    if (ticket_ != nullptr) {
        ticket_->setStatus(ack.success ? tr("命令已接受: %1").arg(ack.orderId)
                                       : tr("下单失败: %1").arg(ack.error),
                           !ack.success);
    }
    updateControls();
    emit refreshRequested();
}

} // namespace tqsdk::ui
