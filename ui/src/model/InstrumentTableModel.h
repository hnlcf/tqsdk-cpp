#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QTimer>
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

    void setInstruments(const QVector<Instrument>& instruments);
    /// 按 chunk 分批插入大目录/期权链，避免一次 reset+全量填充占满 GUI 线程。
    void    setInstrumentsIncremental(const QVector<Instrument>& instruments, int chunkSize = 256);
    void    applyQuotes(const QVector<Quote>& quotes);
    QString symbolAt(int row) const;
    const Instrument*          instrument(const QString& symbol) const;
    const QVector<Instrument>& instruments() const { return instruments_; }

signals:
    void instrumentsLoaded();

private:
    void                  appendNextInstrumentChunk();
    QVector<Instrument>   instruments_;
    QHash<QString, Quote> quotes_;
    QHash<QString, int>   rowBySymbol_;
    QVector<Instrument>   pendingInstruments_;
    int                   pendingOffset_ = 0;
    int                   chunkSize_     = 256;
    QTimer*               loadTimer_     = nullptr;
};

} // namespace tqsdk::ui
