#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QFont>
#include <QHash>
#include <QVector>

namespace tqsdk::ui {

/// 持仓/委托/成交为交易快照模型；TradeTapeModel 为最新优先的有界分笔队列。
class PositionTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Symbol = 0,
        Long,
        Short,
        OpenLong,
        OpenShort,
        Last,
        FloatProfit,
        Margin,
        ColumnCount
    };

    explicit PositionTableModel(QObject* parent = nullptr);
    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void                     setPositions(const QVector<Position>& positions);
    void                     applyPositions(const QVector<Position>& positions);
    const QVector<Position>& positions() const { return positions_; }

private:
    QVector<Position> positions_;
};

class OrderTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        OrderId = 0,
        Symbol,
        Direction,
        Offset,
        Price,
        VolumeLeft,
        Status,
        Message,
        ColumnCount,
    };

    explicit OrderTableModel(QObject* parent = nullptr);
    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void                  setOrders(const QVector<Order>& orders);
    void                  applyOrders(const QVector<Order>& orders);
    QString               orderIdAt(int row) const;
    bool                  isOrderAlive(int row) const;
    const QVector<Order>& orders() const { return orders_; }

private:
    QVector<Order>      orders_;
    QHash<QString, int> indexById_;
    QFont               boldFont_;
    void                rebuildIndex();
};

struct TickTrade {
    qint64 datetime = 0;
    double price    = std::numeric_limits<double>::quiet_NaN();
    qint64 volume   = 0;
    bool   buy      = false;
};

class TradeTapeModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Time = 0,
        Price,
        Volume,
        Direction,
        ColumnCount
    };

    explicit TradeTapeModel(QObject* parent = nullptr);
    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void clear();
    void setMaxRows(int rows);
    void applyQuote(const Quote& quote);

private:
    QVector<TickTrade>     ticks_;
    QHash<QString, qint64> lastVolume_;
    QHash<QString, double> lastPrice_;
    int                    maxRows_ = 500;
};

class TradeTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        TradeId = 0,
        Symbol,
        Direction,
        Offset,
        Price,
        Volume,
        Time,
        ColumnCount
    };

    explicit TradeTableModel(QObject* parent = nullptr);
    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void    setTrades(const QVector<Trade>& trades);
    void    applyTrades(const QVector<Trade>& trades);
    QString orderIdAt(int row) const;

private:
    QVector<Trade> trades_;
};

} // namespace tqsdk::ui
