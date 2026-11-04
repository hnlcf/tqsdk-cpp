#pragma once

#include "net/Protocol.h"

#include <QAbstractTableModel>
#include <QFont>
#include <QHash>
#include <QMimeData>
#include <QStringList>

namespace tqsdk::ui {

/// 自选列表模型。
///
/// 以用户 watchlist 的行顺序为唯一顺序，quote 数据按 symbol 合并；
/// 涨跌、涨幅和增仓都在 data() 中由最新快照计算，不依赖服务端额外字段。
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

    void        setSymbols(const QStringList& symbols);
    void        applyQuotes(const QVector<Quote>& quotes);
    QString     symbolAt(int row) const;
    QStringList symbols() const { return symbols_; }

    Qt::ItemFlags   flags(const QModelIndex& index) const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList     mimeTypes() const override;
    QMimeData*      mimeData(const QModelIndexList& indexes) const override;
    bool            canDropMimeData(const QMimeData*   data,
                                    Qt::DropAction     action,
                                    int                row,
                                    int                column,
                                    const QModelIndex& parent) const override;
    bool            dropMimeData(const QMimeData*   data,
                                 Qt::DropAction     action,
                                 int                row,
                                 int                column,
                                 const QModelIndex& parent) override;

signals:
    void symbolsReordered(const QStringList& symbols);

private:
    QStringList           symbols_;
    QHash<QString, Quote> quotes_;
    QFont                 boldFont_;
};

} // namespace tqsdk::ui
