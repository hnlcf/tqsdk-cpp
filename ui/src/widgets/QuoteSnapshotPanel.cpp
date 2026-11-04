#include "widgets/QuoteSnapshotPanel.h"

#include "model/Formatting.h"

#include <QFormLayout>
#include <QLabel>

#include <cmath>
#include <limits>

namespace tqsdk::ui {
namespace {

void setTextIfChanged(QLabel* label, const QString& text) {
    if (label != nullptr && label->text() != text) {
        label->setText(text);
    }
}

void setStyleIfChanged(QLabel* label, const QString& style) {
    if (label != nullptr && label->styleSheet() != style) {
        label->setStyleSheet(style);
    }
}

void setMuted(QLabel* label, const QString& text) {
    setTextIfChanged(label, text);
    setStyleIfChanged(label, QStringLiteral("color: #8f8f8f;"));
}

} // namespace

QuoteSnapshotPanel::QuoteSnapshotPanel(QWidget* parent)
    : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(12, 12, 12, 12);

    symbol_ = new QLabel(QStringLiteral("—"), this);
    name_   = new QLabel(QStringLiteral("—"), this);
    name_->setObjectName(QStringLiteral("quoteName"));
    last_         = new QLabel(QStringLiteral("—"), this);
    change_       = new QLabel(QStringLiteral("—"), this);
    changePct_    = new QLabel(QStringLiteral("—"), this);
    bidAsk_       = new QLabel(QStringLiteral("—"), this);
    volume_       = new QLabel(QStringLiteral("—"), this);
    openInterest_ = new QLabel(QStringLiteral("—"), this);
    oiChange_     = new QLabel(QStringLiteral("—"), this);
    highLow_      = new QLabel(QStringLiteral("—"), this);

    symbol_->setObjectName(QStringLiteral("quoteSymbol"));
    last_->setObjectName(QStringLiteral("quoteLast"));
    change_->setObjectName(QStringLiteral("quoteChange"));
    changePct_->setObjectName(QStringLiteral("quoteChangePct"));
    oiChange_->setObjectName(QStringLiteral("quoteOiChange"));

    last_->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 700;"));

    form->addRow(tr("合约"), symbol_);
    form->addRow(tr("名称"), name_);
    form->addRow(tr("最新"), last_);
    form->addRow(tr("涨跌"), change_);
    form->addRow(tr("涨幅"), changePct_);
    form->addRow(tr("买卖"), bidAsk_);
    form->addRow(tr("成交"), volume_);
    form->addRow(tr("持仓"), openInterest_);
    form->addRow(tr("增仓"), oiChange_);
    form->addRow(tr("高/低"), highLow_);
}

void QuoteSnapshotPanel::setSymbol(const QString& symbol) {
    const QString display = symbol.isEmpty() ? QStringLiteral("—") : symbol;
    if (symbol_->text() != display) {
        symbol_->setText(display);
        // Do not carry the previous contract's name across a symbol switch.
        setTextIfChanged(name_, QStringLiteral("—"));
    }
}

void QuoteSnapshotPanel::setQuote(const Quote* quote, const QString& instrumentName) {
    // Catalog loading can temporarily make findInstrument() return null. Keep
    // the last known name instead of flashing "—" on every quote update.
    if (!instrumentName.isEmpty()) {
        setTextIfChanged(name_, instrumentName);
    }

    if (quote == nullptr) {
        setMuted(last_, QStringLiteral("—"));
        setStyleIfChanged(last_, QStringLiteral("font-size: 22px; font-weight: 700;"));
        setMuted(change_, QStringLiteral("—"));
        setMuted(changePct_, QStringLiteral("—"));
        setMuted(bidAsk_, QStringLiteral("—"));
        setMuted(volume_, QStringLiteral("—"));
        setMuted(openInterest_, QStringLiteral("—"));
        setMuted(oiChange_, QStringLiteral("—"));
        setMuted(highLow_, QStringLiteral("—"));
        return;
    }

    const QString color = quoteColor(*quote);
    setTextIfChanged(last_, priceText(quote->lastPrice));
    setStyleIfChanged(last_,
                      QStringLiteral("font-size: 22px; font-weight: 700; color: %1;").arg(color));

    const double change = std::isfinite(quote->lastPrice) && std::isfinite(quote->preSettlement)
                              ? quote->lastPrice - quote->preSettlement
                              : std::numeric_limits<double>::quiet_NaN();
    setTextIfChanged(
        change_,
        std::isfinite(change)
            ? QStringLiteral("%1%2").arg(change >= 0.0 ? "+" : "").arg(change, 0, 'f', 2)
            : QStringLiteral("—"));
    setStyleIfChanged(change_, QStringLiteral("font-weight: 600; color: %1;").arg(color));
    setTextIfChanged(changePct_, changeText(changePercent(*quote)));
    setStyleIfChanged(changePct_, QStringLiteral("font-weight: 600; color: %1;").arg(color));

    setTextIfChanged(
        bidAsk_,
        QStringLiteral("%1 / %2").arg(priceText(quote->bidPrice1), priceText(quote->askPrice1)));
    setTextIfChanged(volume_,
                     quote->volume > 0 ? QString::number(quote->volume) : QStringLiteral("—"));
    setTextIfChanged(openInterest_,
                     std::isfinite(quote->openInterest)
                         ? QString::number(quote->openInterest, 'f', 0)
                         : QStringLiteral("—"));

    if (std::isfinite(quote->openInterest) && std::isfinite(quote->preOpenInterest)) {
        const double oiChange = quote->openInterest - quote->preOpenInterest;
        setTextIfChanged(
            oiChange_,
            QStringLiteral("%1%2").arg(oiChange >= 0.0 ? "+" : "").arg(oiChange, 0, 'f', 0));
        setStyleIfChanged(oiChange_,
                          QStringLiteral("font-weight: 600; color: %1;")
                              .arg(oiChange >= 0.0 ? "#e04a4a" : "#23b39a"));
    } else {
        setMuted(oiChange_, QStringLiteral("—"));
    }
    setTextIfChanged(highLow_,
                     QStringLiteral("%1 / %2").arg(priceText(quote->high), priceText(quote->low)));
}

} // namespace tqsdk::ui
