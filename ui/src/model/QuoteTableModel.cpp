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
        quotes_[quote.instrumentId] = quote;
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

    if (role == Qt::ForegroundRole && index.column() == ChangePercent) {
        if (!quote || !valid(changePercent(*quote))) {
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
        default:
            return {};
        }
    }
    return section + 1;
}

} // namespace tqsdk::ui
