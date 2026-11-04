#include "model/TradingModels.h"

#include <QColor>
#include <QFont>
#include <QString>

#include <algorithm>
#include <cmath>

namespace tqsdk::ui {
namespace {

QString number(double value, int decimals = 2) {
    return std::isfinite(value) ? QString::number(value, 'f', decimals) : QStringLiteral("—");
}

QVariant displayText(int column, const Position& position) {
    switch (column) {
    case PositionTableModel::Symbol:
        return position.instrumentId;
    case PositionTableModel::Long:
        return QString::number(position.volumeLong);
    case PositionTableModel::Short:
        return QString::number(position.volumeShort);
    case PositionTableModel::OpenLong:
        return number(position.openPriceLong);
    case PositionTableModel::OpenShort:
        return number(position.openPriceShort);
    case PositionTableModel::Last:
        return number(position.lastPrice);
    case PositionTableModel::FloatProfit:
        return number(position.floatProfit);
    case PositionTableModel::Margin:
        return number(position.margin);
    default:
        return {};
    }
}

QVariant displayText(int column, const Order& order) {
    switch (column) {
    case OrderTableModel::OrderId:
        return order.orderId;
    case OrderTableModel::Symbol:
        return order.instrumentId;
    case OrderTableModel::Direction:
        return order.direction;
    case OrderTableModel::Offset:
        return order.offset;
    case OrderTableModel::Price:
        return number(order.limitPrice);
    case OrderTableModel::VolumeLeft:
        return QStringLiteral("%1/%2").arg(order.volumeLeft).arg(order.volumeOrigin);
    case OrderTableModel::Status:
        return order.status.isEmpty()
                   ? (order.isDead ? QStringLiteral("DEAD") : QStringLiteral("ALIVE"))
                   : order.status;
    default:
        return {};
    }
}

QVariant displayText(int column, const Trade& trade) {
    switch (column) {
    case TradeTableModel::TradeId:
        return trade.tradeId;
    case TradeTableModel::Symbol:
        return trade.instrumentId;
    case TradeTableModel::Direction:
        return trade.direction;
    case TradeTableModel::Offset:
        return trade.offset;
    case TradeTableModel::Price:
        return number(trade.price);
    case TradeTableModel::Volume:
        return QString::number(trade.volume);
    case TradeTableModel::Time:
        return trade.tradeDateTime > 0 ? QString::number(trade.tradeDateTime) : QStringLiteral("—");
    default:
        return {};
    }
}

QVariant alignment(int column, int leftmostTextColumn = 1) {
    return column == leftmostTextColumn ? QVariant(Qt::AlignLeft | Qt::AlignVCenter)
                                        : QVariant(Qt::AlignRight | Qt::AlignVCenter);
}

} // namespace

PositionTableModel::PositionTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int PositionTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : positions_.size();
}

int PositionTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

void PositionTableModel::setPositions(const QVector<Position>& positions) {
    beginResetModel();
    positions_ = positions;
    endResetModel();
}

void PositionTableModel::applyPositions(const QVector<Position>& positions) {
    for (const auto& incoming : positions) {
        auto it = std::find_if(positions_.begin(), positions_.end(), [&](const Position& p) {
            return p.instrumentId == incoming.instrumentId;
        });
        if (it == positions_.end()) {
            positions_.push_back(incoming);
        } else {
            *it = incoming;
        }
    }
    if (!positions_.isEmpty()) {
        emit dataChanged(index(0, 0), index(positions_.size() - 1, ColumnCount - 1));
    }
}

QVariant PositionTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= positions_.size()) {
        return {};
    }
    if (role == Qt::TextAlignmentRole) {
        return alignment(index.column(), PositionTableModel::Symbol);
    }
    if (role == Qt::ForegroundRole && index.column() == FloatProfit) {
        const auto& position = positions_.at(index.row());
        if (!std::isfinite(position.floatProfit)) {
            return QColor("#8b93a3");
        }
        return position.floatProfit >= 0.0 ? QColor("#e43c4a") : QColor("#2fbf71");
    }
    if (role == Qt::DisplayRole) {
        return displayText(index.column(), positions_.at(index.row()));
    }
    return {};
}

QVariant PositionTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case Symbol:
            return QStringLiteral("合约");
        case Long:
            return QStringLiteral("多");
        case Short:
            return QStringLiteral("空");
        case OpenLong:
            return QStringLiteral("多头均价");
        case OpenShort:
            return QStringLiteral("空头均价");
        case Last:
            return QStringLiteral("最新");
        case FloatProfit:
            return QStringLiteral("浮动盈亏");
        case Margin:
            return QStringLiteral("保证金");
        default:
            return {};
        }
    }
    return section + 1;
}

OrderTableModel::OrderTableModel(QObject* parent)
    : QAbstractTableModel(parent) {
    boldFont_.setBold(true);
}

int OrderTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : orders_.size();
}

int OrderTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

void OrderTableModel::rebuildIndex() {
    indexById_.clear();
    for (int i = 0; i < orders_.size(); ++i) {
        indexById_.insert(orders_.at(i).orderId, i);
    }
}

void OrderTableModel::setOrders(const QVector<Order>& orders) {
    beginResetModel();
    orders_ = orders;
    rebuildIndex();
    endResetModel();
}

void OrderTableModel::applyOrders(const QVector<Order>& orders) {
    for (const auto& incoming : orders) {
        auto it = indexById_.constFind(incoming.orderId);
        if (it == indexById_.constEnd()) {
            const int row = orders_.size();
            beginInsertRows({}, row, row);
            orders_.push_back(incoming);
            indexById_.insert(incoming.orderId, row);
            endInsertRows();
        } else {
            orders_[it.value()] = incoming;
        }
    }
    if (!orders_.isEmpty()) {
        emit dataChanged(index(0, 0), index(orders_.size() - 1, ColumnCount - 1));
    }
}

QString OrderTableModel::orderIdAt(int row) const {
    return row >= 0 && row < orders_.size() ? orders_.at(row).orderId : QString{};
}

bool OrderTableModel::isOrderAlive(int row) const {
    return row >= 0 && row < orders_.size() && !orders_.at(row).isDead;
}

QVariant OrderTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= orders_.size()) {
        return {};
    }
    if (role == Qt::TextAlignmentRole) {
        return alignment(index.column());
    }
    if (role == Qt::FontRole && index.column() == OrderId) {
        return boldFont_;
    }
    if (role == Qt::ForegroundRole && index.column() == Status) {
        const auto& order = orders_.at(index.row());
        return order.isDead ? QColor("#8b93a3") : QColor("#43d17c");
    }
    if (role == Qt::DisplayRole) {
        return displayText(index.column(), orders_.at(index.row()));
    }
    return {};
}

QVariant OrderTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case OrderId:
            return QStringLiteral("订单号");
        case Symbol:
            return QStringLiteral("合约");
        case Direction:
            return QStringLiteral("方向");
        case Offset:
            return QStringLiteral("开平");
        case Price:
            return QStringLiteral("价格");
        case VolumeLeft:
            return QStringLiteral("剩余/总量");
        case Status:
            return QStringLiteral("状态");
        default:
            return {};
        }
    }
    return section + 1;
}

TradeTapeModel::TradeTapeModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int TradeTapeModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ticks_.size();
}

int TradeTapeModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

void TradeTapeModel::clear() {
    beginResetModel();
    ticks_.clear();
    lastVolume_.clear();
    lastPrice_.clear();
    endResetModel();
}

void TradeTapeModel::setMaxRows(int rows) {
    const int newMax = std::max(1, rows);
    if (newMax == maxRows_) {
        return;
    }
    maxRows_ = newMax;
    if (ticks_.size() > maxRows_) {
        beginRemoveRows({}, maxRows_, ticks_.size() - 1);
        ticks_.resize(maxRows_);
        endRemoveRows();
    }
}

void TradeTapeModel::applyQuote(const Quote& quote) {
    if (quote.instrumentId.isEmpty()) {
        return;
    }
    const auto   previousVolume = lastVolume_.constFind(quote.instrumentId);
    const auto   previousPrice  = lastPrice_.constFind(quote.instrumentId);
    const qint64 volume         = quote.volume;
    if (previousVolume == lastVolume_.constEnd()) {
        lastVolume_[quote.instrumentId] = volume;
        lastPrice_[quote.instrumentId]  = quote.lastPrice;
        return;
    }
    const qint64 delta              = volume - previousVolume.value();
    lastVolume_[quote.instrumentId] = volume;
    if (delta <= 0 || !std::isfinite(quote.lastPrice)) {
        lastPrice_[quote.instrumentId] = quote.lastPrice;
        return;
    }

    TickTrade tick;
    tick.datetime =
        quote.datetime > 0 ? quote.datetime : QDateTime::currentMSecsSinceEpoch() * 1'000'000LL;
    tick.price  = quote.lastPrice;
    tick.volume = delta;
    tick.buy    = previousPrice != lastPrice_.constEnd() && std::isfinite(previousPrice.value()) &&
                  quote.lastPrice >= previousPrice.value();
    lastPrice_[quote.instrumentId] = quote.lastPrice;

    if (ticks_.size() >= maxRows_) {
        beginRemoveRows({}, ticks_.size() - 1, ticks_.size() - 1);
        ticks_.removeLast();
        endRemoveRows();
    }
    beginInsertRows({}, 0, 0);
    ticks_.prepend(tick);
    endInsertRows();
}

QVariant TradeTapeModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= ticks_.size()) {
        return {};
    }
    const auto& tick = ticks_.at(index.row());
    if (role == Qt::TextAlignmentRole) {
        return index.column() == Time ? QVariant(Qt::AlignLeft | Qt::AlignVCenter)
                                      : QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role == Qt::ForegroundRole && (index.column() == Price || index.column() == Direction)) {
        return tick.buy ? QColor("#e43c4a") : QColor("#2fbf71");
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (index.column()) {
    case Time:
        return tick.datetime > 0 ? QDateTime::fromMSecsSinceEpoch(tick.datetime / 1'000'000)
                                       .toString(QStringLiteral("HH:mm:ss"))
                                 : QStringLiteral("—");
    case Price:
        return number(tick.price);
    case Volume:
        return QString::number(tick.volume);
    case Direction:
        return tick.buy ? QStringLiteral("买") : QStringLiteral("卖");
    default:
        return {};
    }
}

QVariant TradeTapeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case Time:
            return QStringLiteral("时间");
        case Price:
            return QStringLiteral("价格");
        case Volume:
            return QStringLiteral("手数");
        case Direction:
            return QStringLiteral("方向");
        default:
            return {};
        }
    }
    return section + 1;
}

TradeTableModel::TradeTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int TradeTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : trades_.size();
}

int TradeTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

void TradeTableModel::setTrades(const QVector<Trade>& trades) {
    beginResetModel();
    trades_ = trades;
    endResetModel();
}

void TradeTableModel::applyTrades(const QVector<Trade>& trades) {
    for (const auto& incoming : trades) {
        auto it = std::find_if(trades_.begin(), trades_.end(), [&](const Trade& trade) {
            return trade.tradeId == incoming.tradeId;
        });
        if (it == trades_.end()) {
            trades_.push_back(incoming);
        } else {
            *it = incoming;
        }
    }
    if (!trades_.isEmpty()) {
        emit dataChanged(index(0, 0), index(trades_.size() - 1, ColumnCount - 1));
    }
}

QVariant TradeTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= trades_.size()) {
        return {};
    }
    if (role == Qt::TextAlignmentRole) {
        return alignment(index.column());
    }
    if (role == Qt::DisplayRole) {
        return displayText(index.column(), trades_.at(index.row()));
    }
    return {};
}

QVariant TradeTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case TradeId:
            return QStringLiteral("成交号");
        case Symbol:
            return QStringLiteral("合约");
        case Direction:
            return QStringLiteral("方向");
        case Offset:
            return QStringLiteral("开平");
        case Price:
            return QStringLiteral("价格");
        case Volume:
            return QStringLiteral("数量");
        case Time:
            return QStringLiteral("时间");
        default:
            return {};
        }
    }
    return section + 1;
}

} // namespace tqsdk::ui
