#include "model/InstrumentTableModel.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QString>

#include <algorithm>
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
    if (loadTimer_ != nullptr) {
        loadTimer_->stop();
    }
    pendingInstruments_.clear();
    pendingOffset_ = 0;

    beginResetModel();
    instruments_ = instruments;
    rowBySymbol_.clear();
    rowBySymbol_.reserve(instruments_.size());
    for (int row = 0; row < instruments_.size(); ++row) {
        rowBySymbol_.insert(instruments_.at(row).instrumentId, row);
    }
    endResetModel();
    emit instrumentsLoaded();
}

void InstrumentTableModel::setInstrumentsIncremental(const QVector<Instrument>& instruments,
                                                     int                        chunkSize) {
    if (loadTimer_ != nullptr) {
        loadTimer_->stop();
    }
    chunkSize_ = std::max(1, chunkSize);
    beginResetModel();
    instruments_.clear();
    rowBySymbol_.clear();
    pendingInstruments_ = instruments;
    pendingOffset_      = 0;
    endResetModel();

    if (pendingInstruments_.isEmpty()) {
        emit instrumentsLoaded();
        return;
    }
    if (loadTimer_ == nullptr) {
        loadTimer_ = new QTimer(this);
        loadTimer_->setSingleShot(true);
        connect(
            loadTimer_, &QTimer::timeout, this, &InstrumentTableModel::appendNextInstrumentChunk);
    }
    loadTimer_->start(0);
}

void InstrumentTableModel::appendNextInstrumentChunk() {
    if (pendingOffset_ >= pendingInstruments_.size()) {
        pendingInstruments_.clear();
        pendingOffset_ = 0;
        emit instrumentsLoaded();
        return;
    }

    const int first = instruments_.size();
    const int last =
        std::min(static_cast<int>(pendingInstruments_.size()), pendingOffset_ + chunkSize_);
    beginInsertRows({}, first, first + (last - pendingOffset_) - 1);
    for (int i = pendingOffset_; i < last; ++i) {
        const auto& instrument = pendingInstruments_.at(i);
        rowBySymbol_.insert(instrument.instrumentId, instruments_.size());
        instruments_.push_back(instrument);
    }
    endInsertRows();
    pendingOffset_ = last;

    if (pendingOffset_ < pendingInstruments_.size()) {
        loadTimer_->start(0);
    } else {
        pendingInstruments_.clear();
        pendingOffset_ = 0;
        emit instrumentsLoaded();
    }
}

void InstrumentTableModel::applyQuotes(const QVector<Quote>& quotes) {
    QSet<int> changedRows;
    for (const auto& quote : quotes) {
        if (quote.instrumentId.isEmpty()) {
            continue;
        }
        auto quoteIt = quotes_.find(quote.instrumentId);
        if (quoteIt == quotes_.end()) {
            quoteIt = quotes_.insert(quote.instrumentId, quote);
        } else {
            *quoteIt = mergeQuote(*quoteIt, quote);
        }
        const auto rowIt = rowBySymbol_.constFind(quote.instrumentId);
        if (rowIt == rowBySymbol_.constEnd()) {
            continue;
        }
        auto& instrument = instruments_[rowIt.value()];
        if (std::isfinite(quoteIt.value().lastPrice) && quoteIt.value().lastPrice > 0.0) {
            instrument.lastPrice = quoteIt.value().lastPrice;
        }
        changedRows.insert(rowIt.value());
    }
    for (const int row : changedRows) {
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
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
    if (role == Qt::ForegroundRole &&
        (index.column() == LastPrice || index.column() == ChangePercent)) {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        if (quote == quotes_.constEnd() || !std::isfinite(quote->lastPrice) ||
            !std::isfinite(quote->preSettlement) || quote->preSettlement == 0.0) {
            return QColor("#8b93a3");
        }
        const double pct = (quote->lastPrice - quote->preSettlement) / quote->preSettlement * 100.0;
        return pct >= 0.0 ? QColor("#e43c4a") : QColor("#2fbf71");
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
    case ChangePercent: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        if (quote == quotes_.constEnd() || !std::isfinite(quote->lastPrice) ||
            !std::isfinite(quote->preSettlement) || quote->preSettlement == 0.0) {
            return QStringLiteral("—");
        }
        const double pct = (quote->lastPrice - quote->preSettlement) / quote->preSettlement * 100.0;
        return QStringLiteral("%1%2%").arg(pct >= 0.0 ? "+" : "").arg(pct, 0, 'f', 2);
    }
    case Bid1: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        return quote != quotes_.constEnd() && std::isfinite(quote->bidPrice1)
                   ? QString::number(quote->bidPrice1, 'f', 2)
                   : QStringLiteral("—");
    }
    case Ask1: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        return quote != quotes_.constEnd() && std::isfinite(quote->askPrice1)
                   ? QString::number(quote->askPrice1, 'f', 2)
                   : QStringLiteral("—");
    }
    case Volume: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        return quote != quotes_.constEnd() && quote->volume > 0 ? QString::number(quote->volume)
                                                                : QStringLiteral("—");
    }
    case OpenInterest: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        return quote != quotes_.constEnd() && std::isfinite(quote->openInterest)
                   ? QString::number(quote->openInterest, 'f', 0)
                   : QStringLiteral("—");
    }
    case OiChange: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        if (quote == quotes_.constEnd() || !std::isfinite(quote->openInterest) ||
            !std::isfinite(quote->preOpenInterest)) {
            return QStringLiteral("—");
        }
        const double change = quote->openInterest - quote->preOpenInterest;
        return QStringLiteral("%1%2").arg(change >= 0.0 ? "+" : "").arg(change, 0, 'f', 0);
    }
    case UpperLimit: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        return quote != quotes_.constEnd() && std::isfinite(quote->upperLimit)
                   ? QString::number(quote->upperLimit, 'f', 2)
                   : QStringLiteral("—");
    }
    case LowerLimit: {
        const auto quote = quotes_.constFind(instrument.instrumentId);
        return quote != quotes_.constEnd() && std::isfinite(quote->lowerLimit)
                   ? QString::number(quote->lowerLimit, 'f', 2)
                   : QStringLiteral("—");
    }
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
        case ChangePercent:
            return QStringLiteral("涨跌幅");
        case Bid1:
            return QStringLiteral("买一");
        case Ask1:
            return QStringLiteral("卖一");
        case Volume:
            return QStringLiteral("成交量");
        case OpenInterest:
            return QStringLiteral("持仓量");
        case OiChange:
            return QStringLiteral("增仓");
        case UpperLimit:
            return QStringLiteral("涨停价");
        case LowerLimit:
            return QStringLiteral("跌停价");
        default:
            return {};
        }
    }
    return section + 1;
}

} // namespace tqsdk::ui
