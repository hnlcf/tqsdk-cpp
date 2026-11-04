#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QStringList>

namespace tqsdk::ui {

class QuoteTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Symbol = 0,
        Last,
        ChangePercent,
        Bid1,
        Ask1,
        Volume,
        Change,
        PreSettlement,
        OpenInterest,
        OiChange,
        UpperLimit,
        LowerLimit,
        High,
        Low,
        ColumnCount,
    };

    explicit QuoteTableModel(QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void    setSymbols(const QStringList& symbols);
    void    applyQuotes(const QVector<Quote>& quotes);
    QString symbolAt(int row) const;

private:
    QStringList           symbols_;
    QHash<QString, Quote> quotes_;
};

} // namespace tqsdk::ui
