#pragma once

#include "net/Protocol.h"

#include <QString>

#include <cmath>
#include <limits>

namespace tqsdk::ui {

/// Display formatting shared by tables, chart and side panels.
inline QString priceText(double value) {
    return std::isfinite(value) ? QString::number(value, 'f', 2) : QStringLiteral("—");
}

inline QString changeText(double percent) {
    if (!std::isfinite(percent)) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1%2%").arg(percent >= 0.0 ? "+" : "").arg(percent, 0, 'f', 2);
}

inline double changePercent(const Quote& quote) {
    if (!std::isfinite(quote.lastPrice) || !std::isfinite(quote.preSettlement) ||
        quote.preSettlement == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (quote.lastPrice - quote.preSettlement) / quote.preSettlement * 100.0;
}

inline QString quoteColor(const Quote& quote) {
    const double change = changePercent(quote);
    return !std::isfinite(change)
               ? QStringLiteral("#8f8f8f")
               : (change >= 0.0 ? QStringLiteral("#e04a4a") : QStringLiteral("#23b39a"));
}

} // namespace tqsdk::ui
