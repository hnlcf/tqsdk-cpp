#include "model/InstrumentTableModel.h"

#include <QFont>
#include <QHash>
#include <QString>

#include <cmath>

namespace tqsdk::ui {

InstrumentTableModel::InstrumentTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int InstrumentTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : instruments_.size();
}

int InstrumentTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

void InstrumentTableModel::setInstruments(const QVector<Instrument>& instruments) {
    beginResetModel();
    instruments_ = instruments;
    endResetModel();
}

void InstrumentTableModel::applyQuotes(const QVector<Quote>& quotes) {
    QHash<QString, const Quote*> bySymbol;
    for (const auto& quote : quotes) {
        bySymbol.insert(quote.instrumentId, &quote);
    }
    bool changed = false;
    for (auto& instrument : instruments_) {
        auto it = bySymbol.constFind(instrument.instrumentId);
        if (it != bySymbol.constEnd() && std::isfinite(it.value()->lastPrice) &&
            it.value()->lastPrice > 0.0) {
            instrument.lastPrice = it.value()->lastPrice;
            changed              = true;
        }
    }
    if (changed && !instruments_.isEmpty()) {
        emit dataChanged(index(0, 0), index(instruments_.size() - 1, ColumnCount - 1));
    }
}

QString InstrumentTableModel::symbolAt(int row) const {
    return row >= 0 && row < instruments_.size() ? instruments_.at(row).instrumentId : QString{};
}

QVariant InstrumentTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= instruments_.size() ||
        index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }
    const auto& instrument = instruments_.at(index.row());

    if (role == Qt::FontRole && index.column() == Symbol) {
        QFont font;
        font.setBold(true);
        return font;
    }
    if (role == Qt::TextAlignmentRole) {
        return index.column() >= LastPrice ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                                           : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
    case Symbol:
        return instrument.instrumentId;
    case Name:
        return instrument.name.isEmpty() ? instrument.instrumentId : instrument.name;
    case Exchange:
        return instrument.exchangeId;
    case Product:
        return instrument.product;
    case LastPrice:
        return instrument.lastPrice > 0.0 ? QString::number(instrument.lastPrice, 'f', 2)
                                          : QStringLiteral("—");
    case PriceTick:
        return instrument.priceTick > 0.0 ? QString::number(instrument.priceTick, 'g', 8)
                                          : QStringLiteral("—");
    case Multiplier:
        return instrument.volumeMultiple > 0.0 ? QString::number(instrument.volumeMultiple, 'g', 8)
                                               : QStringLiteral("—");
    default:
        return {};
    }
}

QVariant
InstrumentTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case Symbol:
            return QStringLiteral("合约");
        case Name:
            return QStringLiteral("名称");
        case Exchange:
            return QStringLiteral("交易所");
        case Product:
            return QStringLiteral("品种");
        case LastPrice:
            return QStringLiteral("最新价");
        case PriceTick:
            return QStringLiteral("最小变动");
        case Multiplier:
            return QStringLiteral("合约乘数");
        default:
            return {};
        }
    }
    return section + 1;
}

} // namespace tqsdk::ui
