#include "widgets/OrderTicketPanel.h"

#include <QCompleter>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringListModel>

#include <cmath>

namespace tqsdk::ui {

OrderTicketPanel::OrderTicketPanel(QWidget* parent)
    : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(12, 4, 12, 12);

    symbolEdit_ = new QLineEdit(this);
    symbolEdit_->setPlaceholderText(tr("输入或搜索合约，回车切换图表"));
    symbolEdit_->setObjectName(QStringLiteral("orderSymbol"));

    completions_ = new QStringListModel(this);
    completer_   = new QCompleter(completions_, this);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setFilterMode(Qt::MatchContains);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setMaxVisibleItems(16);
    symbolEdit_->setCompleter(completer_);

    connect(symbolEdit_, &QLineEdit::returnPressed, this, [this] {
        emit symbolSubmitted(symbolEdit_->text());
    });
    connect(completer_,
            QOverload<const QString&>::of(&QCompleter::activated),
            this,
            [this](const QString& text) { emit symbolSubmitted(text); });

    priceBox_ = new QDoubleSpinBox(this);
    priceBox_->setObjectName(QStringLiteral("orderPrice"));
    priceBox_->setRange(0.0, 1.0e9);
    priceBox_->setDecimals(2);
    priceBox_->setSingleStep(1.0);
    priceBox_->setValue(0.0);
    volumeBox_ = new QSpinBox(this);
    volumeBox_->setObjectName(QStringLiteral("orderVolume"));
    volumeBox_->setRange(1, 1000);
    volumeBox_->setValue(1);

    auto* buttons      = new QWidget(this);
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(6);
    buyButton_   = new QPushButton(tr("买开"), buttons);
    sellButton_  = new QPushButton(tr("卖开"), buttons);
    closeButton_ = new QPushButton(tr("平仓"), buttons);
    buyButton_->setObjectName(QStringLiteral("orderBuy"));
    sellButton_->setObjectName(QStringLiteral("orderSell"));
    closeButton_->setObjectName(QStringLiteral("orderClose"));
    buyButton_->setEnabled(false);
    sellButton_->setEnabled(false);
    closeButton_->setEnabled(false);
    const auto styleActionButton = [](QPushButton*   button,
                                      const QString& base,
                                      const QString& hover,
                                      const QString& pressed,
                                      const QString& border,
                                      const QString& disabled) {
        button->setStyleSheet(
            QStringLiteral("QPushButton { background:%1; color:#ffffff; font-weight:600; "
                           "padding:9px 14px; border:1px solid %4; border-radius:3px; }"
                           "QPushButton:hover { background:%2; }"
                           "QPushButton:pressed { background:%3; }"
                           "QPushButton:disabled { background:%5; color:#8f8f8f; "
                           "border-color:#3a3a3a; }")
                .arg(base, hover, pressed, border, disabled));
    };
    styleActionButton(buyButton_,
                      QStringLiteral("#c94b4b"),
                      QStringLiteral("#d85b5b"),
                      QStringLiteral("#a83b3b"),
                      QStringLiteral("#e06a6a"),
                      QStringLiteral("#3a2424"));
    styleActionButton(sellButton_,
                      QStringLiteral("#249c86"),
                      QStringLiteral("#2bb39b"),
                      QStringLiteral("#187b6b"),
                      QStringLiteral("#36c4aa"),
                      QStringLiteral("#1d3431"));
    styleActionButton(closeButton_,
                      QStringLiteral("#a8792f"),
                      QStringLiteral("#bd8a38"),
                      QStringLiteral("#865d20"),
                      QStringLiteral("#d09a48"),
                      QStringLiteral("#352c1d"));
    buttonLayout->addWidget(buyButton_, 1);
    buttonLayout->addWidget(sellButton_, 1);
    buttonLayout->addWidget(closeButton_, 1);

    status_ = new QLabel(tr("未连接交易账户"), this);
    status_->setObjectName(QStringLiteral("orderStatus"));
    status_->setWordWrap(true);
    status_->setStyleSheet("color: #8f8f8f;");

    form->addRow(tr("合约"), symbolEdit_);
    form->addRow(tr("价格"), priceBox_);
    form->addRow(tr("数量"), volumeBox_);
    form->addRow(tr("三键"), buttons);
    form->addRow(status_);

    connect(buyButton_, &QPushButton::clicked, this, &OrderTicketPanel::buyOpenRequested);
    connect(sellButton_, &QPushButton::clicked, this, &OrderTicketPanel::sellOpenRequested);
    connect(closeButton_, &QPushButton::clicked, this, &OrderTicketPanel::closeRequested);
}

void OrderTicketPanel::setSymbol(const QString& symbol) {
    symbolEdit_->setText(symbol);
}

void OrderTicketPanel::setCompletions(const QStringList& completions) {
    completions_->setStringList(completions);
}

void OrderTicketPanel::setTradingEnabled(bool enabled) {
    buyButton_->setEnabled(enabled);
    sellButton_->setEnabled(enabled);
    if (!enabled) {
        closeButton_->setEnabled(false);
    }
}

void OrderTicketPanel::setCloseEnabled(bool enabled) {
    closeButton_->setEnabled(enabled);
}

void OrderTicketPanel::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? QStringLiteral("color: #e04a4a;")
                                 : QStringLiteral("color: #8f8f8f;"));
}

void OrderTicketPanel::setPriceIfEmpty(double price) {
    if (priceBox_->value() <= 0.0 && std::isfinite(price)) {
        priceBox_->setValue(price);
    }
}

QString OrderTicketPanel::symbolText() const {
    return symbolEdit_->text();
}

double OrderTicketPanel::price() const {
    return priceBox_->value();
}

int OrderTicketPanel::volume() const {
    return volumeBox_->value();
}

} // namespace tqsdk::ui
