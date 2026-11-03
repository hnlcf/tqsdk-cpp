#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>

namespace tqsdk::ui {

class PositionTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Symbol = 0,
        Long,
        Short,
        OpenLong,
        Last,
        FloatProfit,
        ColumnCount
    };

    explicit PositionTableModel(QObject* parent = nullptr);
    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setPositions(const QVector<Position>& positions);
    void applyPositions(const QVector<Position>& positions);

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
        ColumnCount,
    };

    explicit OrderTableModel(QObject* parent = nullptr);
    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void    setOrders(const QVector<Order>& orders);
    void    applyOrders(const QVector<Order>& orders);
    QString orderIdAt(int row) const;
    bool    isOrderAlive(int row) const;

private:
    QVector<Order>      orders_;
    QHash<QString, int> indexById_;
    void                rebuildIndex();
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

    void setTrades(const QVector<Trade>& trades);
    void applyTrades(const QVector<Trade>& trades);

private:
    QVector<Trade> trades_;
};

} // namespace tqsdk::ui
