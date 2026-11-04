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

PositionDetailPanel::PositionDetailPanel(QWidget* parent)
    : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(12, 12, 12, 12);
    symbol_      = new QLabel(QStringLiteral("—"), this);
    longVolume_  = new QLabel(QStringLiteral("—"), this);
    shortVolume_ = new QLabel(QStringLiteral("—"), this);
    openLong_    = new QLabel(QStringLiteral("—"), this);
    openShort_   = new QLabel(QStringLiteral("—"), this);
    last_        = new QLabel(QStringLiteral("—"), this);
    floatProfit_ = new QLabel(QStringLiteral("—"), this);
    margin_      = new QLabel(QStringLiteral("—"), this);
    symbol_->setObjectName(QStringLiteral("positionDetailSymbol"));
    form->addRow(tr("合约"), symbol_);
    form->addRow(tr("多头/空头"), new QLabel(QStringLiteral("持仓"), this));
    form->addRow(tr("多"), longVolume_);
    form->addRow(tr("空"), shortVolume_);
    form->addRow(tr("开多价"), openLong_);
    form->addRow(tr("开空价"), openShort_);
    form->addRow(tr("最新价"), last_);
    form->addRow(tr("浮动盈亏"), floatProfit_);
    form->addRow(tr("保证金"), margin_);
}

void PositionDetailPanel::setPosition(const Position* position) {
    if (position == nullptr) {
        clear();
        return;
    }
    symbol_->setText(position->instrumentId.isEmpty() ? QStringLiteral("—")
                                                      : position->instrumentId);
    longVolume_->setText(QString::number(position->volumeLong));
    shortVolume_->setText(QString::number(position->volumeShort));
    openLong_->setText(priceText(position->openPriceLong));
    openShort_->setText(priceText(position->openPriceShort));
    last_->setText(priceText(position->lastPrice));
    floatProfit_->setText(priceText(position->floatProfit));
    margin_->setText(priceText(position->margin));
}

void PositionDetailPanel::clear() {
    symbol_->setText(QStringLiteral("—"));
    longVolume_->setText(QStringLiteral("—"));
    shortVolume_->setText(QStringLiteral("—"));
    openLong_->setText(QStringLiteral("—"));
    openShort_->setText(QStringLiteral("—"));
    last_->setText(QStringLiteral("—"));
    floatProfit_->setText(QStringLiteral("—"));
    margin_->setText(QStringLiteral("—"));
}

} // namespace tqsdk::ui
