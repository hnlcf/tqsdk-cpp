#include "widgets/TradingPanels.h"

#include "model/Formatting.h"
#include "model/TradingModels.h"
#include "widgets/TablePanel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <cmath>

namespace tqsdk::ui {

PositionsPanel::PositionsPanel(PositionTableModel* model, QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* panel = new TablePanel(model, this);
    panel->installColumnMenu(QStringLiteral("positions"));
    view_ = panel->view();
    layout->addWidget(panel);

    connect(view_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this, model](const QModelIndex& current) {
                if (!current.isValid() || current.row() >= model->positions().size()) {
                    return;
                }
                const QString symbol = model->positions().at(current.row()).instrumentId;
                if (!symbol.isEmpty()) {
                    emit positionActivated(symbol);
                }
            });
}

OrdersPanel::OrdersPanel(OrderTableModel* model, QWidget* parent)
    : QWidget(parent),
      model_(model) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout();
    cancel_   = new QPushButton(tr("撤销选中委托"), this);
    cancel_->setObjectName(QStringLiteral("cancelSelectedOrder"));
    bar->addWidget(cancel_);
    bar->addStretch(1);
    layout->addLayout(bar);

    auto* panel = new TablePanel(model, this);
    panel->installColumnMenu(QStringLiteral("orders"));
    view_ = panel->view();
    layout->addWidget(panel, 1);

    connect(cancel_, &QPushButton::clicked, this, &OrdersPanel::cancelRequested);
    connect(view_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex&) { emit selectionChanged(selectedOrderId()); });
}

QString OrdersPanel::selectedOrderId() const {
    if (view_ == nullptr || !view_->currentIndex().isValid() || model_ == nullptr) {
        return {};
    }
    return model_->orderIdAt(view_->currentIndex().row());
}

void OrdersPanel::setCancelEnabled(bool enabled) {
    cancel_->setEnabled(enabled);
}

TradesPanel::TradesPanel(TradeTableModel* model, QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* panel = new TablePanel(model, this);
    panel->installColumnMenu(QStringLiteral("trades"));
    view_ = panel->view();
    layout->addWidget(panel);
}

AccountPanel::AccountPanel(QWidget* parent)
    : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(12, 12, 12, 12);
    balance_   = new QLabel(QStringLiteral("—"), this);
    available_ = new QLabel(QStringLiteral("—"), this);
    margin_    = new QLabel(QStringLiteral("—"), this);
    risk_      = new QLabel(QStringLiteral("—"), this);
    balance_->setObjectName(QStringLiteral("accountBalance"));
    available_->setObjectName(QStringLiteral("accountAvailable"));
    margin_->setObjectName(QStringLiteral("accountMargin"));
    risk_->setObjectName(QStringLiteral("accountRisk"));
    form->addRow(tr("权益"), balance_);
    form->addRow(tr("可用"), available_);
    form->addRow(tr("保证金"), margin_);
    form->addRow(tr("风险度"), risk_);
}

void AccountPanel::setAccount(const Account& account) {
    balance_->setText(priceText(account.balance));
    available_->setText(priceText(account.available));
    margin_->setText(priceText(account.margin));
    risk_->setText(std::isfinite(account.riskRatio)
                       ? QString::number(account.riskRatio * 100.0, 'f', 2) + QStringLiteral("%")
                       : QStringLiteral("—"));
}

OrderDetailPanel::OrderDetailPanel(QWidget* parent)
    : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(12, 12, 12, 12);
    orderId_ = new QLabel(QStringLiteral("—"), this);
    orderId_->setObjectName(QStringLiteral("orderDetailId"));
    symbol_ = new QLabel(QStringLiteral("—"), this);
    side_   = new QLabel(QStringLiteral("—"), this);
    price_  = new QLabel(QStringLiteral("—"), this);
    volume_ = new QLabel(QStringLiteral("—"), this);
    status_ = new QLabel(QStringLiteral("—"), this);
    form->addRow(tr("委托号"), orderId_);
    form->addRow(tr("合约"), symbol_);
    form->addRow(tr("方向/开平"), side_);
    form->addRow(tr("价格"), price_);
    form->addRow(tr("数量"), volume_);
    form->addRow(tr("状态"), status_);
}

void OrderDetailPanel::setOrder(const Order* order) {
    if (order == nullptr) {
        clear();
        return;
    }
    orderId_->setText(order->orderId.isEmpty() ? QStringLiteral("—") : order->orderId);
    symbol_->setText(order->instrumentId.isEmpty() ? QStringLiteral("—") : order->instrumentId);
    side_->setText(QStringLiteral("%1 / %2").arg(order->direction, order->offset));
    price_->setText(priceText(order->limitPrice));
    volume_->setText(QStringLiteral("%1 / %2").arg(order->volumeLeft).arg(order->volumeOrigin));
    status_->setText(order->status);
}

void OrderDetailPanel::clear() {
    orderId_->setText(QStringLiteral("—"));
    symbol_->setText(QStringLiteral("—"));
    side_->setText(QStringLiteral("—"));
    price_->setText(QStringLiteral("—"));
    volume_->setText(QStringLiteral("—"));
    status_->setText(QStringLiteral("—"));
}

} // namespace tqsdk::ui
