#include "model/QuoteTableModel.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QString>

#include <cmath>

namespace tqsdk::ui {
namespace {

bool valid(double value) {
    return std::isfinite(value);
}

QString numberText(double value, int decimals = 2) {
    if (!valid(value)) {
        return QStringLiteral("—");
    }
    return QString::number(value, 'f', decimals);
}

double changePercent(const Quote& quote) {
    if (!valid(quote.lastPrice) || !valid(quote.preSettlement) || quote.preSettlement == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (quote.lastPrice - quote.preSettlement) / quote.preSettlement * 100.0;
}

} // namespace

QuoteTableModel::QuoteTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int QuoteTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : symbols_.size();
}

int QuoteTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

void QuoteTableModel::setSymbols(const QStringList& symbols) {
    if (symbols_ == symbols) {
        return;
    }
    beginResetModel();
    symbols_ = symbols;
    endResetModel();
}

void QuoteTableModel::applyQuotes(const QVector<Quote>& quotes) {
    if (quotes.isEmpty() || symbols_.isEmpty()) {
        return;
    }
    for (const auto& quote : quotes) {
        if (!symbols_.contains(quote.instrumentId)) {
            continue;
        }
        auto it = quotes_.find(quote.instrumentId);
        if (it == quotes_.end()) {
            quotes_.insert(quote.instrumentId, quote);
        } else {
            *it = mergeQuote(*it, quote);
        }
    }
    emit dataChanged(index(0, 0), index(symbols_.size() - 1, ColumnCount - 1));
}

QString QuoteTableModel::symbolAt(int row) const {
    return row >= 0 && row < symbols_.size() ? symbols_.at(row) : QString{};
}

QVariant QuoteTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= symbols_.size() ||
        index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }

    const QString& symbol = symbols_.at(index.row());
    const auto     it     = quotes_.constFind(symbol);
    const Quote*   quote  = it == quotes_.constEnd() ? nullptr : &it.value();

    if (role == Qt::TextAlignmentRole) {
        return index.column() == Symbol ? QVariant(Qt::AlignLeft | Qt::AlignVCenter)
                                        : QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (role == Qt::ForegroundRole && (index.column() == ChangePercent ||
                                       index.column() == Change || index.column() == OiChange)) {
        if (!quote) {
            return QBrush(QColor("#8b93a3"));
        }
        if (index.column() == OiChange) {
            if (!valid(quote->openInterest) || !valid(quote->preOpenInterest)) {
                return QBrush(QColor("#8b93a3"));
            }
            return QBrush(quote->openInterest >= quote->preOpenInterest ? QColor("#e43c4a")
                                                                        : QColor("#2fbf71"));
        }
        if (!valid(changePercent(*quote))) {
            return QBrush(QColor("#8b93a3"));
        }
        return QBrush(changePercent(*quote) >= 0.0 ? QColor("#e43c4a") : QColor("#2fbf71"));
    }

    if (role == Qt::FontRole && index.column() == Symbol) {
        QFont font;
        font.setBold(true);
        return font;
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    if (index.column() == Symbol) {
        return symbol;
    }
    if (!quote) {
        return QStringLiteral("—");
    }

    switch (index.column()) {
    case Last:
        return numberText(quote->lastPrice);
    case ChangePercent: {
        const double pct = changePercent(*quote);
        return valid(pct) ? QStringLiteral("%1%2%").arg(pct >= 0.0 ? "+" : "").arg(pct, 0, 'f', 2)
                          : QStringLiteral("—");
    }
    case Bid1:
        return numberText(quote->bidPrice1);
    case Ask1:
        return numberText(quote->askPrice1);
    case Volume:
        return quote->volume > 0 ? QString::number(quote->volume) : QStringLiteral("—");
    case Change: {
        const double change = valid(quote->lastPrice) && valid(quote->preSettlement)
                                  ? quote->lastPrice - quote->preSettlement
                                  : std::numeric_limits<double>::quiet_NaN();
        return valid(change)
                   ? QStringLiteral("%1%2").arg(change >= 0.0 ? "+" : "").arg(change, 0, 'f', 2)
                   : QStringLiteral("—");
    }
    case PreSettlement:
        return numberText(quote->preSettlement);
    case OpenInterest:
        return valid(quote->openInterest) ? QString::number(quote->openInterest, 'f', 0)
                                          : QStringLiteral("—");
    case OiChange: {
        if (!valid(quote->openInterest) || !valid(quote->preOpenInterest)) {
            return QStringLiteral("—");
        }
        const double change = quote->openInterest - quote->preOpenInterest;
        return QStringLiteral("%1%2").arg(change >= 0.0 ? "+" : "").arg(change, 0, 'f', 0);
    }
    case UpperLimit:
        return numberText(quote->upperLimit);
    case LowerLimit:
        return numberText(quote->lowerLimit);
    case High:
        return numberText(quote->high);
    case Low:
        return numberText(quote->low);
    default:
        return {};
    }
}

QVariant QuoteTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case Symbol:
            return QStringLiteral("合约");
        case Last:
            return QStringLiteral("最新");
        case ChangePercent:
            return QStringLiteral("涨跌%");
        case Bid1:
            return QStringLiteral("买一");
        case Ask1:
            return QStringLiteral("卖一");
        case Volume:
            return QStringLiteral("成交量");
        case Change:
            return QStringLiteral("涨跌");
        case PreSettlement:
            return QStringLiteral("昨结算");
        case OpenInterest:
            return QStringLiteral("持仓量");
        case OiChange:
            return QStringLiteral("增仓");
        case UpperLimit:
            return QStringLiteral("涨停价");
        case LowerLimit:
            return QStringLiteral("跌停价");
        case High:
            return QStringLiteral("最高");
        case Low:
            return QStringLiteral("最低");
        default:
            return {};
        }
    }
    return section + 1;
}

} // namespace tqsdk::ui
