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

/// 持仓列表。
class PositionsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PositionsPanel(PositionTableModel* model, QWidget* parent = nullptr);
    QTableView* view() const { return view_; }

signals:
    void positionActivated(const QString& symbol);

private:
    QTableView* view_ = nullptr;
};

/// 委托列表，含撤单按钮和选中委托信号。
class OrdersPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OrdersPanel(OrderTableModel* model, QWidget* parent = nullptr);
    QTableView* view() const { return view_; }
    QString     selectedOrderId() const;
    void        setCancelEnabled(bool enabled);

signals:
    void cancelRequested();
    void selectionChanged(const QString& orderId);

private:
    OrderTableModel* model_  = nullptr;
    QTableView*      view_   = nullptr;
    QPushButton*     cancel_ = nullptr;
};

/// 成交列表。
class TradesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit TradesPanel(TradeTableModel* model, QWidget* parent = nullptr);
    QTableView* view() const { return view_; }

private:
    QTableView* view_ = nullptr;
};

/// 账户权益/可用/保证金/风险度。
class AccountPanel final : public QWidget {
    Q_OBJECT

public:
    explicit AccountPanel(QWidget* parent = nullptr);
    void setAccount(const Account& account);

private:
    QLabel* balance_   = nullptr;
    QLabel* available_ = nullptr;
    QLabel* margin_    = nullptr;
    QLabel* risk_      = nullptr;
};

/// 选中持仓的明细字段面板。
class PositionDetailPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PositionDetailPanel(QWidget* parent = nullptr);
    void setPosition(const Position* position);
    void clear();

private:
    QLabel* symbol_      = nullptr;
    QLabel* longVolume_  = nullptr;
    QLabel* shortVolume_ = nullptr;
    QLabel* openLong_    = nullptr;
    QLabel* openShort_   = nullptr;
    QLabel* last_        = nullptr;
    QLabel* floatProfit_ = nullptr;
    QLabel* margin_      = nullptr;
};

} // namespace tqsdk::ui
