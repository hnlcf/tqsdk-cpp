#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QVector>

namespace tqsdk::ui {

class InstrumentTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        Symbol = 0,
        Name,
        Exchange,
        Product,
        LastPrice,
        PriceTick,
        Multiplier,
        ColumnCount,
    };

    explicit InstrumentTableModel(QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = {}) const override;
    int      columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void                       setInstruments(const QVector<Instrument>& instruments);
    void                       applyQuotes(const QVector<Quote>& quotes);
    QString                    symbolAt(int row) const;
    const QVector<Instrument>& instruments() const { return instruments_; }

private:
    QVector<Instrument> instruments_;
};

} // namespace tqsdk::ui
