#pragma once

#include "net/Protocol.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableView;

namespace tqsdk::ui {

class OrderTableModel;
class PositionTableModel;
class TradeTableModel;

/// 持仓 / 委托 / 成交 / 账户 四个页签。
///
/// 只负责交易表格和账户字段展示；交易动作通过信号交给 MainWindow。
class TradingTablesPanel final : public QWidget {
    Q_OBJECT

public:
    TradingTablesPanel(PositionTableModel* positions,
                       OrderTableModel*    orders,
                       TradeTableModel*    trades,
                       QWidget*            parent = nullptr);

    QTableView* positionTable() const { return positionTable_; }
    QTableView* orderTable() const { return orderTable_; }
    QTableView* tradeTable() const { return tradeTable_; }

    void    setAccount(const Account& account);
    void    setCancelEnabled(bool enabled);
    QString selectedOrderId() const;

signals:
    void positionActivated(const QString& symbol);
    void cancelRequested();
    void orderSelectionChanged();

private:
    QTableView*    positionTable_ = nullptr;
    QTableView*    orderTable_    = nullptr;
    QTableView*    tradeTable_    = nullptr;
    QPushButton*   cancelButton_  = nullptr;
    QLabel*        balance_       = nullptr;
    QLabel*        available_     = nullptr;
    QLabel*        margin_        = nullptr;
    QLabel*        risk_          = nullptr;
    OrderTableModel* ordersModel_ = nullptr;
};

} // namespace tqsdk::ui
