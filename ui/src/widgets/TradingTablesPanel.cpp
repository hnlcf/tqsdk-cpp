#include "widgets/TradingTablesPanel.h"

#include "model/Formatting.h"
#include "model/TradingModels.h"
#include "widgets/TablePanel.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <cmath>

namespace tqsdk::ui {

TradingTablesPanel::TradingTablesPanel(PositionTableModel* positions,
                                       OrderTableModel*    orders,
                                       TradeTableModel*    trades,
                                       QWidget*            parent)
    : QWidget(parent),
      ordersModel_(orders) {
    auto* tabs   = new QTabWidget(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tabs);

    auto* positionPanel = new TablePanel(positions, tabs);
    positionPanel->installColumnMenu(QStringLiteral("positions"));
    positionTable_ = positionPanel->view();
    connect(positionTable_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this, positions](const QModelIndex& current) {
                if (!current.isValid() || current.row() >= positions->positions().size()) {
                    return;
                }
                const QString symbol = positions->positions().at(current.row()).instrumentId;
                if (!symbol.isEmpty()) {
                    emit positionActivated(symbol);
                }
            });
    tabs->addTab(positionPanel, tr("持仓"));

    auto* orderPage   = new QWidget(tabs);
    auto* orderLayout = new QVBoxLayout(orderPage);
    orderLayout->setContentsMargins(4, 4, 4, 4);
    auto* orderBar = new QHBoxLayout();
    cancelButton_  = new QPushButton(tr("撤销选中委托"), orderPage);
    cancelButton_->setEnabled(false);
    orderBar->addWidget(cancelButton_);
    orderBar->addStretch(1);
    orderLayout->addLayout(orderBar);

    auto* orderPanel = new TablePanel(orders, orderPage);
    orderPanel->installColumnMenu(QStringLiteral("orders"));
    orderTable_ = orderPanel->view();
    orderLayout->addWidget(orderPanel, 1);
    connect(cancelButton_, &QPushButton::clicked, this, &TradingTablesPanel::cancelRequested);
    connect(orderTable_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex&) { emit orderSelectionChanged(); });
    tabs->addTab(orderPage, tr("委托"));

    auto* tradePanel = new TablePanel(trades, tabs);
    tradePanel->installColumnMenu(QStringLiteral("trades"));
    tradeTable_ = tradePanel->view();
    tabs->addTab(tradePanel, tr("成交"));

    auto* accountPage = new QWidget(tabs);
    auto* accountForm = new QFormLayout(accountPage);
    accountForm->setContentsMargins(12, 12, 12, 12);
    balance_   = new QLabel(QStringLiteral("—"), accountPage);
    available_ = new QLabel(QStringLiteral("—"), accountPage);
    margin_    = new QLabel(QStringLiteral("—"), accountPage);
    risk_      = new QLabel(QStringLiteral("—"), accountPage);
    balance_->setObjectName(QStringLiteral("accountBalance"));
    available_->setObjectName(QStringLiteral("accountAvailable"));
    margin_->setObjectName(QStringLiteral("accountMargin"));
    risk_->setObjectName(QStringLiteral("accountRisk"));
    accountForm->addRow(tr("权益"), balance_);
    accountForm->addRow(tr("可用"), available_);
    accountForm->addRow(tr("保证金"), margin_);
    accountForm->addRow(tr("风险度"), risk_);
    tabs->addTab(accountPage, tr("账户"));
}

void TradingTablesPanel::setAccount(const Account& account) {
    balance_->setText(priceText(account.balance));
    available_->setText(priceText(account.available));
    margin_->setText(priceText(account.margin));
    risk_->setText(std::isfinite(account.riskRatio)
                       ? QString::number(account.riskRatio * 100.0, 'f', 2) + QStringLiteral("%")
                       : QStringLiteral("—"));
}

void TradingTablesPanel::setCancelEnabled(bool enabled) {
    cancelButton_->setEnabled(enabled);
}

QString TradingTablesPanel::selectedOrderId() const {
    if (orderTable_ == nullptr || !orderTable_->currentIndex().isValid()) {
        return {};
    }
    const int row = orderTable_->currentIndex().row();
    return ordersModel_ != nullptr ? ordersModel_->orderIdAt(row) : QString{};
}

} // namespace tqsdk::ui
