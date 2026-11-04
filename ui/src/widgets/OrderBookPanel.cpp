#include "widgets/OrderBookPanel.h"

#include "model/Formatting.h"

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>

namespace tqsdk::ui {
namespace {

constexpr const char* kAskColor = "#e04a4a";
constexpr const char* kBidColor = "#23b39a";
constexpr const char* kMuted    = "#8f8f8f";

QLabel* makeLabel(QWidget* parent, Qt::Alignment alignment = Qt::AlignRight | Qt::AlignVCenter) {
    auto* label = new QLabel(QStringLiteral("—"), parent);
    label->setAlignment(alignment);
    return label;
}

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

} // namespace

OrderBookPanel::OrderBookPanel(QWidget* parent)
    : QWidget(parent) {
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(10, 8, 10, 8);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(4);

    auto* levelHeader  = new QLabel(tr("档位"), this);
    auto* priceHeader  = new QLabel(tr("价格"), this);
    auto* volumeHeader = new QLabel(tr("手数"), this);
    priceHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(levelHeader, 0, 0);
    grid->addWidget(priceHeader, 0, 1);
    grid->addWidget(volumeHeader, 0, 2);

    // 卖5 -> 卖1
    for (int display = 0; display < 5; ++display) {
        const int level      = 4 - display;
        auto*     levelLabel = new QLabel(QStringLiteral("卖%1").arg(level + 1), this);
        levelLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kAskColor));
        askPrice_[level]  = makeLabel(this);
        askVolume_[level] = makeLabel(this);
        askPrice_[level]->setObjectName(QStringLiteral("bookAskPrice%1").arg(level + 1));
        askVolume_[level]->setObjectName(QStringLiteral("bookAskVolume%1").arg(level + 1));
        askPrice_[level]->setStyleSheet(QStringLiteral("color: %1;").arg(kAskColor));
        askPrice_[level]->setCursor(Qt::PointingHandCursor);
        askPrice_[level]->installEventFilter(this);
        grid->addWidget(levelLabel, display + 1, 0);
        grid->addWidget(askPrice_[level], display + 1, 1);
        grid->addWidget(askVolume_[level], display + 1, 2);
    }

    auto* lastRow    = new QWidget(this);
    auto* lastLayout = new QHBoxLayout(lastRow);
    lastLayout->setContentsMargins(0, 6, 0, 6);
    symbol_ = new QLabel(QStringLiteral("—"), lastRow);
    symbol_->setObjectName(QStringLiteral("bookSymbol"));
    last_ = new QLabel(QStringLiteral("—"), lastRow);
    last_->setObjectName(QStringLiteral("bookLast"));
    last_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    last_->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700;"));
    last_->setCursor(Qt::PointingHandCursor);
    last_->installEventFilter(this);
    lastLayout->addWidget(symbol_);
    lastLayout->addStretch(1);
    lastLayout->addWidget(last_);
    grid->addWidget(lastRow, 6, 0, 1, 3);

    // 买1 -> 买5
    for (int level = 0; level < 5; ++level) {
        auto* levelLabel = new QLabel(QStringLiteral("买%1").arg(level + 1), this);
        levelLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kBidColor));
        bidPrice_[level]  = makeLabel(this);
        bidVolume_[level] = makeLabel(this);
        bidPrice_[level]->setObjectName(QStringLiteral("bookBidPrice%1").arg(level + 1));
        bidVolume_[level]->setObjectName(QStringLiteral("bookBidVolume%1").arg(level + 1));
        bidPrice_[level]->setStyleSheet(QStringLiteral("color: %1;").arg(kBidColor));
        bidPrice_[level]->setCursor(Qt::PointingHandCursor);
        bidPrice_[level]->installEventFilter(this);
        grid->addWidget(levelLabel, level + 7, 0);
        grid->addWidget(bidPrice_[level], level + 7, 1);
        grid->addWidget(bidVolume_[level], level + 7, 2);
    }
    grid->setRowStretch(12, 1);
}

bool OrderBookPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            bool isPrice = watched == last_;
            for (int level = 0; level < 5 && !isPrice; ++level) {
                isPrice = watched == askPrice_[level] || watched == bidPrice_[level];
            }
            if (isPrice) {
                if (auto* label = qobject_cast<QLabel*>(watched)) {
                    bool         ok    = false;
                    const double price = label->text().toDouble(&ok);
                    if (ok && price > 0.0) {
                        emit priceClicked(price);
                        return true;
                    }
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OrderBookPanel::setSymbol(const QString& symbol) {
    setTextIfChanged(symbol_, symbol.isEmpty() ? QStringLiteral("—") : symbol);
}

void OrderBookPanel::setQuote(const Quote* quote) {
    if (quote == nullptr) {
        clear();
        return;
    }

    const double asks[5] = {
        quote->askPrice1, quote->askPrice2, quote->askPrice3, quote->askPrice4, quote->askPrice5};
    const double bids[5] = {
        quote->bidPrice1, quote->bidPrice2, quote->bidPrice3, quote->bidPrice4, quote->bidPrice5};
    const quint64 askVolumes[5] = {quote->askVolume1,
                                   quote->askVolume2,
                                   quote->askVolume3,
                                   quote->askVolume4,
                                   quote->askVolume5};
    const quint64 bidVolumes[5] = {quote->bidVolume1,
                                   quote->bidVolume2,
                                   quote->bidVolume3,
                                   quote->bidVolume4,
                                   quote->bidVolume5};

    for (int level = 0; level < 5; ++level) {
        setTextIfChanged(askPrice_[level], priceText(asks[level]));
        setTextIfChanged(askVolume_[level],
                         askVolumes[level] > 0 ? QString::number(askVolumes[level])
                                               : QStringLiteral("—"));
        setTextIfChanged(bidPrice_[level], priceText(bids[level]));
        setTextIfChanged(bidVolume_[level],
                         bidVolumes[level] > 0 ? QString::number(bidVolumes[level])
                                               : QStringLiteral("—"));
    }
    setTextIfChanged(last_, priceText(quote->lastPrice));
    setStyleIfChanged(
        last_,
        QStringLiteral("font-size: 20px; font-weight: 700; color: %1;").arg(quoteColor(*quote)));
}

void OrderBookPanel::clear() {
    for (int level = 0; level < 5; ++level) {
        askPrice_[level]->setText(QStringLiteral("—"));
        askVolume_[level]->setText(QStringLiteral("—"));
        bidPrice_[level]->setText(QStringLiteral("—"));
        bidVolume_[level]->setText(QStringLiteral("—"));
    }
    last_->setText(QStringLiteral("—"));
    last_->setStyleSheet(
        QStringLiteral("font-size: 20px; font-weight: 700; color: %1;").arg(kMuted));
}

} // namespace tqsdk::ui
