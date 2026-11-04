#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QVector>

namespace tqsdk::ui {

/// 合约/期货/期权列表模型。
///
/// quotes_ 保存按 symbol 合并后的最新 quote；setInstruments() 建立
/// symbol -> row 索引，applyQuotes() 只对变化的行发 dataChanged，
/// 避免 UI 线程每次行情都扫描全表。
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
        ChangePercent,
        Bid1,
        Ask1,
        Volume,
        OpenInterest,
        OiChange,
        UpperLimit,
        LowerLimit,
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
    QVector<Instrument>   instruments_;
    QHash<QString, Quote> quotes_;
    QHash<QString, int>   rowBySymbol_;
};

} // namespace tqsdk::ui
