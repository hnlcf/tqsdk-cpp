#include "widgets/QuoteSnapshotPanel.h"

#include "model/Formatting.h"

#include <QFormLayout>
#include <QLabel>

#include <cmath>
#include <limits>

namespace tqsdk::ui {
namespace {

void setMuted(QLabel* label, const QString& text) {
    if (label != nullptr) {
        label->setText(text);
        label->setStyleSheet(QStringLiteral("color: #8b93a3;"));
    }
}

} // namespace

QuoteSnapshotPanel::QuoteSnapshotPanel(QWidget* parent) : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(12, 12, 12, 12);

    symbol_       = new QLabel(QStringLiteral("—"), this);
    name_         = new QLabel(QStringLiteral("—"), this);
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
    symbol_->setText(symbol.isEmpty() ? QStringLiteral("—") : symbol);
}

void QuoteSnapshotPanel::setQuote(const Quote* quote, const QString& instrumentName) {
    name_->setText(instrumentName.isEmpty() ? QStringLiteral("—") : instrumentName);

    if (quote == nullptr) {
        setMuted(last_, QStringLiteral("—"));
        last_->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 700;"));
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
    last_->setText(priceText(quote->lastPrice));
    last_->setStyleSheet(
        QStringLiteral("font-size: 22px; font-weight: 700; color: %1;").arg(color));

    const double change = std::isfinite(quote->lastPrice) && std::isfinite(quote->preSettlement)
                              ? quote->lastPrice - quote->preSettlement
                              : std::numeric_limits<double>::quiet_NaN();
    change_->setText(
        std::isfinite(change)
            ? QStringLiteral("%1%2").arg(change >= 0.0 ? "+" : "").arg(change, 0, 'f', 2)
            : QStringLiteral("—"));
    change_->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;").arg(color));
    changePct_->setText(changeText(changePercent(*quote)));
    changePct_->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;").arg(color));

    bidAsk_->setText(
        QStringLiteral("%1 / %2").arg(priceText(quote->bidPrice1), priceText(quote->askPrice1)));
    volume_->setText(quote->volume > 0 ? QString::number(quote->volume) : QStringLiteral("—"));
    openInterest_->setText(std::isfinite(quote->openInterest)
                               ? QString::number(quote->openInterest, 'f', 0)
                               : QStringLiteral("—"));

    if (std::isfinite(quote->openInterest) && std::isfinite(quote->preOpenInterest)) {
        const double oiChange = quote->openInterest - quote->preOpenInterest;
        oiChange_->setText(
            QStringLiteral("%1%2").arg(oiChange >= 0.0 ? "+" : "").arg(oiChange, 0, 'f', 0));
        oiChange_->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;")
                                     .arg(oiChange >= 0.0 ? "#e43c4a" : "#2fbf71"));
    } else {
        setMuted(oiChange_, QStringLiteral("—"));
    }
    highLow_->setText(QStringLiteral("%1 / %2").arg(priceText(quote->high), priceText(quote->low)));
}

} // namespace tqsdk::ui
