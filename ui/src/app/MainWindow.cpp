#include "app/MainWindow.h"

#include "chart/KLineChartWidget.h"
#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "net/NodeClient.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDockWidget>
#include <QFormLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSize>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include <cmath>
#include <limits>
#include <utility>

namespace tqsdk::ui {
namespace {

QLabel* placeholder(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setStyleSheet("color: #8b93a3; padding: 24px;");
    return label;
}

QWidget* panel(const QString& title, QWidget* content, QWidget* parent = nullptr) {
    auto* box    = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(8, 8, 8, 8);
    auto* heading = new QLabel(title, box);
    heading->setStyleSheet("font-weight: 600; padding: 4px 2px;");
    layout->addWidget(heading);
    layout->addWidget(content, 1);
    return box;
}

QTableWidget* emptyTable(const QStringList& headers, QWidget* parent = nullptr) {
    auto* table = new QTableWidget(0, headers.size(), parent);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    return table;
}

QString priceText(double value) {
    return std::isfinite(value) ? QString::number(value, 'f', 2) : QStringLiteral("—");
}

QString changeText(double percent) {
    if (!std::isfinite(percent)) {
        return QStringLiteral("—");
    }
    return QStringLiteral("%1%2%").arg(percent >= 0.0 ? "+" : "").arg(percent, 0, 'f', 2);
}

double changePercent(const Quote& quote) {
    if (!std::isfinite(quote.lastPrice) || !std::isfinite(quote.preSettlement) ||
        quote.preSettlement == 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (quote.lastPrice - quote.preSettlement) / quote.preSettlement * 100.0;
}

QString themeStyle(bool dark) {
    const QString bg       = dark ? "#0d1117" : "#f5f7fb";
    const QString panel    = dark ? "#111821" : "#ffffff";
    const QString text     = dark ? "#d9e2ec" : "#202a36";
    const QString muted    = dark ? "#8b93a3" : "#657080";
    const QString line     = dark ? "#263140" : "#dce3ec";
    const QString selected = dark ? "#1c2a3a" : "#e6f0ff";
    const QString accent   = dark ? "#2f81f7" : "#0b66c3";
    return QStringLiteral(
               "QMainWindow, QWidget { background: %1; color: %3; }"
               "QToolBar { background: %2; border: none; spacing: 4px; }"
               "QLineEdit { background: %2; border: 1px solid %4; border-radius: 4px; "
               "padding: 5px 8px; color: %3; }"
               "QPushButton { background: transparent; border: 1px solid transparent; "
               "border-radius: 4px; padding: 5px 8px; color: %5; }"
               "QPushButton:hover { background: %6; color: %3; }"
               "QTableView, QTableWidget { background: %2; alternate-background-color: %7; "
               "border: 1px solid %4; gridline-color: %4; selection-background-color: %6; "
               "selection-color: %3; }"
               "QHeaderView::section { background: %2; color: %5; border: none; "
               "border-bottom: 1px solid %4; padding: 6px; }"
               "QTabWidget::pane { border: 1px solid %4; }"
               "QTabBar::tab { padding: 7px 14px; color: %5; }"
               "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %8; }"
               "QDockWidget::title { background: %2; padding: 6px; }"
               "QStatusBar { color: %5; }")
        .arg(bg, panel, text, line, muted, selected, bg, accent);
}

} // namespace

MainWindow::MainWindow(NodeClient* nodeClient, QWidget* parent)
    : QMainWindow(parent),
      nodeClient_(nodeClient) {
    setWindowTitle(tr("TQSDK Terminal (Qt)"));
    resize(1440, 900);

    watchlist_ = {QStringLiteral("SHFE.rb2701"),
                  QStringLiteral("DCE.m2701"),
                  QStringLiteral("CZCE.SA701"),
                  QStringLiteral("CFFEX.IF2612")};

    marketStore_  = new MarketStore(this);
    watchModel_   = new QuoteTableModel(this);
    futuresModel_ = new InstrumentTableModel(this);
    futuresProxy_ = new QSortFilterProxyModel(this);
    futuresProxy_->setSourceModel(futuresModel_);
    futuresProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    futuresProxy_->setFilterKeyColumn(-1);
    watchModel_->setSymbols(watchlist_);

    buildUi();
    applyTheme();
    connectNode();

    if (watchModel_->rowCount() > 0) {
        watchTable_->selectRow(0);
    }
    selectSymbol(selectedSymbol_);
}

void MainWindow::buildUi() {
    buildToolbar();
    buildWorkspace();
    buildBottomDock();
    statusBar()->showMessage(tr("Qt UI Phase 1 · 等待 node"));
}

void MainWindow::buildToolbar() {
    auto* toolbar = addToolBar(tr("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));

    auto* brand = new QLabel(tr("TQSDK"), toolbar);
    brand->setStyleSheet("font-size: 15px; font-weight: 700; padding: 0 10px;");
    toolbar->addWidget(brand);

    search_ = new QLineEdit(toolbar);
    search_->setPlaceholderText(tr("搜索合约代码 / 名称"));
    search_->setMinimumWidth(260);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        futuresProxy_->setFilterFixedString(text);
    });
    toolbar->addWidget(search_);

    toolbar->addSeparator();
    auto* interval = new QLabel(tr("周期"), toolbar);
    interval->setStyleSheet("padding: 0 6px;");
    toolbar->addWidget(interval);

    const std::pair<qint64, const char*> intervals[] = {
        {60'000'000'000LL, "1m"},
        {300'000'000'000LL, "5m"},
        {900'000'000'000LL, "15m"},
        {3'600'000'000'000LL, "1h"},
        {86'400'000'000'000LL, "1d"},
    };
    for (const auto& [duration, label] : intervals) {
        auto* button = new QPushButton(QString::fromLatin1(label), toolbar);
        button->setFlat(true);
        connect(
            button,
            &QPushButton::clicked,
            this,
            [this, duration, text = QString::fromLatin1(label)] { setInterval(duration, text); });
        toolbar->addWidget(button);
    }

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    connectionLabel_ = new QLabel(tr("● 未连接"), toolbar);
    connectionLabel_->setStyleSheet("color: #e7b35a; padding: 0 10px;");
    toolbar->addWidget(connectionLabel_);

    themeButton_ = new QPushButton(tr("浅色"), toolbar);
    themeButton_->setFlat(true);
    connect(themeButton_, &QPushButton::clicked, this, [this] {
        dark_ = !dark_;
        applyTheme();
    });
    toolbar->addWidget(themeButton_);
}

void MainWindow::buildWorkspace() {
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    watchTable_ = new QTableView(splitter);
    watchTable_->setModel(watchModel_);
    watchTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    watchTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    watchTable_->verticalHeader()->setVisible(false);
    watchTable_->horizontalHeader()->setStretchLastSection(true);
    watchTable_->setColumnWidth(QuoteTableModel::Symbol, 118);
    watchTable_->setAlternatingRowColors(true);
    watchTable_->setMinimumWidth(280);
    connect(watchTable_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex& current) {
                if (!current.isValid()) {
                    return;
                }
                const QString symbol = watchModel_->symbolAt(current.row());
                if (!symbol.isEmpty() && symbol != selectedSymbol_) {
                    selectSymbol(symbol);
                }
            });
    splitter->addWidget(panel(tr("自选 / 最近"), watchTable_, splitter));

    centerTabs_ = new QTabWidget(splitter);
    chart_      = new KLineChartWidget(centerTabs_);
    chart_->setTitle(selectedSymbol_, selectedInterval_);
    centerTabs_->addTab(chart_, tr("交易终端"));

    auto* futuresContainer = new QWidget(centerTabs_);
    auto* futuresLayout    = new QVBoxLayout(futuresContainer);
    futuresLayout->setContentsMargins(4, 4, 4, 4);
    futuresTable_ = new QTableView(futuresContainer);
    futuresTable_->setModel(futuresProxy_);
    futuresTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    futuresTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    futuresTable_->verticalHeader()->setVisible(false);
    futuresTable_->horizontalHeader()->setStretchLastSection(true);
    futuresTable_->setAlternatingRowColors(true);
    connect(futuresTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto    source = futuresProxy_->mapToSource(index);
        const QString symbol = futuresModel_->symbolAt(source.row());
        if (!symbol.isEmpty()) {
            selectSymbol(symbol);
            centerTabs_->setCurrentIndex(0);
        }
    });
    futuresLayout->addWidget(futuresTable_);
    centerTabs_->addTab(futuresContainer, tr("期货列表"));

    centerTabs_->addTab(placeholder(tr("期权链 / T 型报价\nPhase 2")), tr("期权链"));
    splitter->addWidget(centerTabs_);

    auto* ticket       = new QWidget(splitter);
    auto* ticketLayout = new QFormLayout(ticket);
    ticketLayout->setContentsMargins(14, 14, 14, 14);
    symbolLabel_ = new QLabel(selectedSymbol_, ticket);
    lastLabel_   = new QLabel(tr("—"), ticket);
    changeLabel_ = new QLabel(tr("—"), ticket);
    bidAskLabel_ = new QLabel(tr("—"), ticket);
    lastLabel_->setStyleSheet("font-size: 22px; font-weight: 700;");
    ticketLayout->addRow(tr("合约"), symbolLabel_);
    ticketLayout->addRow(tr("最新"), lastLabel_);
    ticketLayout->addRow(tr("涨跌"), changeLabel_);
    ticketLayout->addRow(tr("买卖"), bidAskLabel_);
    auto* phase = new QLabel(tr("交易面板 Phase 2"), ticket);
    phase->setStyleSheet("color: #8b93a3; padding-top: 12px;");
    ticketLayout->addRow(phase);
    splitter->addWidget(panel(tr("行情快照"), ticket, splitter));

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 6);
    splitter->setStretchFactor(2, 1);
    setCentralWidget(splitter);
}

void MainWindow::buildBottomDock() {
    auto* dock = new QDockWidget(tr("交易与账户"), this);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    auto* tabs = new QTabWidget(dock);
    tabs->addTab(emptyTable({tr("合约"), tr("方向"), tr("手数"), tr("开仓价"), tr("浮动盈亏")}),
                 tr("持仓"));
    tabs->addTab(emptyTable({tr("订单号"), tr("合约"), tr("方向"), tr("价格"), tr("状态")}),
                 tr("委托"));
    tabs->addTab(emptyTable({tr("成交号"), tr("合约"), tr("方向"), tr("价格"), tr("数量")}),
                 tr("成交"));
    dock->setWidget(tabs);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->setMaximumHeight(230);
}

void MainWindow::connectNode() {
    if (nodeClient_ == nullptr) {
        return;
    }

    connect(nodeClient_,
            &NodeClient::stateChanged,
            this,
            [this](NodeClient::State state, const QString& message) {
                QString text;
                QString color;
                switch (state) {
                case NodeClient::State::Disconnected:
                    text  = tr("● 未连接");
                    color = "#e7b35a";
                    break;
                case NodeClient::State::Connecting:
                    text  = tr("● 连接中");
                    color = "#e7b35a";
                    break;
                case NodeClient::State::Connected:
                    text  = tr("● 已连接");
                    color = "#43d17c";
                    break;
                case NodeClient::State::Ready:
                    text  = tr("● 行情就绪");
                    color = "#43d17c";
                    break;
                case NodeClient::State::Reconnecting:
                    text  = tr("● 重连中");
                    color = "#e7b35a";
                    break;
                }
                if (!message.isEmpty()) {
                    statusBar()->showMessage(message);
                }
                if (connectionLabel_ != nullptr) {
                    connectionLabel_->setText(text);
                    connectionLabel_->setStyleSheet(
                        QStringLiteral("color: %1; padding: 0 10px;").arg(color));
                }
            });

    connect(nodeClient_, &NodeClient::errorOccurred, this, [this](const QString& error) {
        statusBar()->showMessage(error, 8000);
    });

    connect(nodeClient_, &NodeClient::quotesUpdated, this, [this](const QVector<Quote>& quotes) {
        marketStore_->applyQuotes(quotes);
        watchModel_->applyQuotes(quotes);
        updateQuotePanel();
    });

    connect(nodeClient_,
            &NodeClient::klineUpdated,
            this,
            [this](const QString& symbol, qint64 durationNs, const KlineFrame& frame) {
                marketStore_->applyKline(symbol, durationNs, frame);
                if (symbol == selectedSymbol_ && durationNs == selectedDurationNs_) {
                    chart_->setBars(marketStore_->bars(symbol, durationNs));
                }
            });

    connect(nodeClient_,
            &NodeClient::klineHistoryLoaded,
            this,
            [this](const QString& symbol, qint64 durationNs, const QVector<KlineBar>& bars) {
                KlineFrame frame;
                frame.reset = true;
                frame.bars  = bars;
                marketStore_->applyKline(symbol, durationNs, frame);
                if (symbol == selectedSymbol_ && durationNs == selectedDurationNs_) {
                    chart_->setBars(bars);
                }
            });

    connect(nodeClient_,
            &NodeClient::catalogLoaded,
            this,
            [this](const QVector<Instrument>& instruments) {
                futuresModel_->setInstruments(instruments);
                statusBar()->showMessage(tr("合约目录 %1 条").arg(instruments.size()), 5000);
            });

    CatalogQuery query;
    query.classes = QStringLiteral("FUTURE,FUTURE_CONT,FUTURE_INDEX");
    query.limit   = 3000;
    nodeClient_->requestCatalog(query);
}

void MainWindow::selectSymbol(const QString& symbol) {
    if (symbol.isEmpty()) {
        return;
    }
    selectedSymbol_ = symbol;
    if (!watchlist_.contains(symbol)) {
        watchlist_.push_back(symbol);
        watchModel_->setSymbols(watchlist_);
    }

    symbolLabel_->setText(symbol);
    chart_->setTitle(symbol, selectedInterval_);
    chart_->setBars(marketStore_->bars(symbol, selectedDurationNs_));
    updateQuotePanel();

    if (nodeClient_ == nullptr) {
        return;
    }

    Subscription subscription;
    subscription.symbols = watchlist_;
    subscription.klines.push_back({symbol, selectedDurationNs_, 200});
    subscription.topics = {QStringLiteral("account"),
                           QStringLiteral("orders"),
                           QStringLiteral("trades"),
                           QStringLiteral("positions")};
    nodeClient_->setSubscription(subscription);
    nodeClient_->requestKlineHistory(symbol, selectedDurationNs_, 200);
}

void MainWindow::setInterval(qint64 durationNs, const QString& label) {
    if (durationNs <= 0) {
        return;
    }
    selectedDurationNs_ = durationNs;
    selectedInterval_   = label;
    chart_->setTitle(selectedSymbol_, selectedInterval_);
    chart_->setBars(marketStore_->bars(selectedSymbol_, selectedDurationNs_));
    if (nodeClient_ != nullptr) {
        Subscription subscription;
        subscription.symbols = watchlist_;
        subscription.klines.push_back({selectedSymbol_, selectedDurationNs_, 200});
        subscription.topics = {QStringLiteral("account"),
                               QStringLiteral("orders"),
                               QStringLiteral("trades"),
                               QStringLiteral("positions")};
        nodeClient_->setSubscription(subscription);
        nodeClient_->requestKlineHistory(selectedSymbol_, selectedDurationNs_, 200);
    }
    updateQuotePanel();
}

void MainWindow::updateQuotePanel() {
    if (symbolLabel_ == nullptr) {
        return;
    }
    const Quote* quote = marketStore_->quote(selectedSymbol_);
    symbolLabel_->setText(selectedSymbol_);
    if (quote == nullptr) {
        lastLabel_->setText(tr("—"));
        changeLabel_->setText(tr("—"));
        bidAskLabel_->setText(tr("—"));
        return;
    }

    lastLabel_->setText(priceText(quote->lastPrice));
    const double percent = changePercent(*quote);
    changeLabel_->setText(changeText(percent));
    changeLabel_->setStyleSheet(
        QStringLiteral("font-weight: 600; color: %1;")
            .arg(!std::isfinite(percent) ? "#8b93a3" : (percent >= 0.0 ? "#e43c4a" : "#2fbf71")));
    bidAskLabel_->setText(
        QStringLiteral("%1 / %2").arg(priceText(quote->bidPrice1), priceText(quote->askPrice1)));
}

void MainWindow::applyTheme() {
    chart_->setTheme(dark_);
    themeButton_->setText(dark_ ? tr("浅色") : tr("深色"));
    qApp->setStyleSheet(themeStyle(dark_));
}

} // namespace tqsdk::ui
