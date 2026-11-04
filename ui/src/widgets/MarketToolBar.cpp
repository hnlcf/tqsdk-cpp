#include "widgets/MarketToolBar.h"

#include <QCompleter>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringListModel>

#include <utility>

namespace tqsdk::ui {

MarketToolBar::MarketToolBar(QWidget* parent)
    : QToolBar(parent) {
    setObjectName(QStringLiteral("mainToolBar"));
    setMovable(false);
    setIconSize(QSize(18, 18));

    auto* brand = new QLabel(tr("TQSDK"), this);
    brand->setStyleSheet("font-size: 15px; font-weight: 700; padding: 0 10px;");
    addWidget(brand);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("marketSearch"));
    search_->setPlaceholderText(tr("搜索合约代码 / 名称，回车加入自选"));
    search_->setMinimumWidth(280);
    search_->setClearButtonEnabled(true);
    completions_ = new QStringListModel(this);
    completer_   = new QCompleter(completions_, this);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setFilterMode(Qt::MatchContains);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setMaxVisibleItems(16);
    search_->setCompleter(completer_);
    connect(search_, &QLineEdit::textChanged, this, &MarketToolBar::searchTextChanged);
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        emit symbolSubmitted(search_->text());
    });
    connect(completer_,
            QOverload<const QString&>::of(&QCompleter::activated),
            this,
            [this](const QString& text) { emit symbolSubmitted(text); });
    addWidget(search_);

    addButton_ = new QPushButton(tr("加入自选"), this);
    addButton_->setObjectName(QStringLiteral("addSymbolButton"));
    connect(
        addButton_, &QPushButton::clicked, this, [this] { emit addRequested(search_->text()); });
    addWidget(addButton_);

    components_ = new QPushButton(tr("组件"), this);
    components_->setObjectName(QStringLiteral("componentsButton"));
    addWidget(components_);

    addSeparator();
    auto* interval = new QLabel(tr("周期"), this);
    interval->setStyleSheet("padding: 0 6px;");
    addWidget(interval);

    const std::pair<qint64, const char*> intervals[] = {
        {60'000'000'000LL, "1m"},
        {300'000'000'000LL, "5m"},
        {900'000'000'000LL, "15m"},
        {1'800'000'000'000LL, "30m"},
        {3'600'000'000'000LL, "1h"},
        {7'200'000'000'000LL, "2h"},
        {14'400'000'000'000LL, "4h"},
        {86'400'000'000'000LL, "1d"},
        {259'200'000'000'000LL, "3d"},
        {604'800'000'000'000LL, "1w"},
    };
    for (const auto& [duration, label] : intervals) {
        auto* button = new QPushButton(QString::fromLatin1(label), this);
        button->setFlat(true);
        button->setToolTip(tr("切换 K 线周期"));
        connect(button,
                &QPushButton::clicked,
                this,
                [this, duration, text = QString::fromLatin1(label)] {
                    emit intervalRequested(duration, text);
                });
        addWidget(button);
    }

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(spacer);

    connection_ = new QLabel(tr("● 未连接"), this);
    connection_->setObjectName(QStringLiteral("connectionState"));
    connection_->setStyleSheet("color: #e7b35a; padding: 0 10px;");
    addWidget(connection_);

    themeButton_ = new QPushButton(tr("浅色"), this);
    themeButton_->setObjectName(QStringLiteral("themeButton"));
    themeButton_->setFlat(true);
    connect(themeButton_, &QPushButton::clicked, this, &MarketToolBar::themeToggleRequested);
    addWidget(themeButton_);
}

void MarketToolBar::setCompletions(const QStringList& completions) {
    completions_->setStringList(completions);
    if (!search_->text().trimmed().isEmpty() && !completions.isEmpty()) {
        completer_->setCompletionPrefix(search_->text());
        completer_->complete();
    }
}

void MarketToolBar::setComponentsMenu(QMenu* menu) {
    components_->setMenu(menu);
}

void MarketToolBar::setConnectionState(const QString& text, const QString& color) {
    connection_->setText(text);
    connection_->setStyleSheet(QStringLiteral("color: %1; padding: 0 10px;").arg(color));
}

void MarketToolBar::setThemeLabel(bool dark) {
    themeButton_->setText(dark ? tr("浅色") : tr("深色"));
}

void MarketToolBar::clearSearch() {
    search_->clear();
}

QString MarketToolBar::searchText() const {
    return search_->text();
}

} // namespace tqsdk::ui
