#include "app/MainWindow.h"

#include "chart/KLineChartWidget.h"
#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "model/TradingModels.h"
#include "net/NodeClient.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSize>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStringListModel>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
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

QTableView* makeTable(QWidget* parent, QAbstractItemModel* model) {
    auto* table = new QTableView(parent);
    table->setModel(model);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
               "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: %2; "
               "border: 1px solid %4; border-radius: 4px; padding: 5px 8px; color: %3; }"
               "QPushButton { background: transparent; border: 1px solid transparent; "
               "border-radius: 4px; padding: 5px 8px; color: %5; }"
               "QPushButton:hover { background: %6; color: %3; }"
               "QPushButton:disabled { color: %4; }"
               "QTableView { background: %2; alternate-background-color: %7; "
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

int fuzzyScore(const QString& query, const QString& text) {
    if (query.isEmpty() || text.isEmpty()) {
        return 0;
    }
    if (text.compare(query, Qt::CaseInsensitive) == 0) {
        return 1000;
    }
    const int contains = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (contains >= 0) {
        return 500 - std::min(contains, 400);
    }

    // Lightweight subsequence fuzzy match; no pinyin dictionary yet.
    int queryPos = 0;
    int gaps     = 0;
    int last     = -1;
    for (int i = 0; i < text.size() && queryPos < query.size(); ++i) {
        if (text.at(i).toCaseFolded() == query.at(queryPos).toCaseFolded()) {
            if (last >= 0) {
                gaps += i - last - 1;
            }
            last = i;
            ++queryPos;
        }
    }
    if (queryPos != query.size()) {
        return -1;
    }
    return 100 - std::min(gaps, 90);
}

} // namespace

MainWindow::MainWindow(NodeClient* nodeClient, QWidget* parent)
    : QMainWindow(parent),
      nodeClient_(nodeClient) {
    setWindowTitle(tr("TQSDK Terminal (Qt)"));
    resize(1480, 940);

    watchlist_ = {QStringLiteral("SHFE.rb2701"),
                  QStringLiteral("DCE.m2701"),
                  QStringLiteral("CZCE.SA701"),
                  QStringLiteral("CFFEX.IF2612")};

    marketStore_    = new MarketStore(this);
    watchModel_     = new QuoteTableModel(this);
    futuresModel_   = new InstrumentTableModel(this);
    callsModel_     = new InstrumentTableModel(this);
    putsModel_      = new InstrumentTableModel(this);
    positionsModel_ = new PositionTableModel(this);
    ordersModel_    = new OrderTableModel(this);
    tradesModel_    = new TradeTableModel(this);
    futuresProxy_   = new QSortFilterProxyModel(this);
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
    statusBar()->showMessage(tr("Qt UI Phase 2 · 等待 node"));
}

void MainWindow::buildToolbar() {
    auto* toolbar = addToolBar(tr("主工具栏"));
    toolbar->setMovable(false);
    toolbar->setIconSize(QSize(18, 18));

    auto* brand = new QLabel(tr("TQSDK"), toolbar);
    brand->setStyleSheet("font-size: 15px; font-weight: 700; padding: 0 10px;");
    toolbar->addWidget(brand);

    search_ = new QLineEdit(toolbar);
    search_->setPlaceholderText(tr("搜索合约代码 / 名称，回车加入自选"));
    search_->setMinimumWidth(280);
    search_->setClearButtonEnabled(true);
    searchCompletions_ = new QStringListModel(this);
    searchCompleter_   = new QCompleter(searchCompletions_, this);
    searchCompleter_->setCaseSensitivity(Qt::CaseInsensitive);
    searchCompleter_->setFilterMode(Qt::MatchContains);
    searchCompleter_->setCompletionMode(QCompleter::PopupCompletion);
    searchCompleter_->setMaxVisibleItems(16);
    search_->setCompleter(searchCompleter_);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        futuresProxy_->setFilterFixedString(text);
        updateSearchCompletions(text);
    });
    connect(
        search_, &QLineEdit::returnPressed, this, [this] { addSymbolFromInput(search_->text()); });
    connect(searchCompleter_,
            QOverload<const QString&>::of(&QCompleter::activated),
            this,
            [this](const QString& text) { addSymbolFromInput(text); });
    toolbar->addWidget(search_);

    addSymbolButton_ = new QPushButton(tr("加入自选"), toolbar);
    addSymbolButton_->setObjectName("addSymbolButton");
    connect(addSymbolButton_, &QPushButton::clicked, this, [this] {
        addSymbolFromInput(search_->text());
    });
    toolbar->addWidget(addSymbolButton_);

    toolbar->addSeparator();
    auto* interval = new QLabel(tr("周期"), toolbar);
    interval->setStyleSheet("padding: 0 6px;");
    toolbar->addWidget(interval);

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
        auto* button = new QPushButton(QString::fromLatin1(label), toolbar);
        button->setFlat(true);
        button->setToolTip(tr("切换 K 线周期"));
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

    watchTable_ = makeTable(splitter, watchModel_);
    installColumnMenu(watchTable_, QStringLiteral("watchlist"));
    watchTable_->setColumnWidth(QuoteTableModel::Symbol, 118);
    watchTable_->setMinimumWidth(290);
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
    futuresStatus_ = new QLabel(tr("正在加载合约目录…"), futuresContainer);
    futuresStatus_->setStyleSheet("color: #8b93a3; padding: 4px;");
    futuresLayout->addWidget(futuresStatus_);
    futuresTable_ = makeTable(futuresContainer, futuresProxy_);
    installColumnMenu(futuresTable_, QStringLiteral("futures"));
    connect(futuresTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto    source = futuresProxy_->mapToSource(index);
        const QString symbol = futuresModel_->symbolAt(source.row());
        if (!symbol.isEmpty()) {
            addSymbolToWatchlist(symbol);
            centerTabs_->setCurrentIndex(0);
        }
    });
    futuresLayout->addWidget(futuresTable_, 1);
    centerTabs_->addTab(futuresContainer, tr("期货列表"));

    auto* optionsContainer = new QWidget(centerTabs_);
    auto* optionsLayout    = new QHBoxLayout(optionsContainer);
    optionsLayout->setContentsMargins(4, 4, 4, 4);
    callsTable_ = makeTable(optionsContainer, callsModel_);
    installColumnMenu(callsTable_, QStringLiteral("option-calls"));
    putsTable_ = makeTable(optionsContainer, putsModel_);
    installColumnMenu(putsTable_, QStringLiteral("option-puts"));
    auto addOption =
        [this](QTableView* table, InstrumentTableModel* model, const QModelIndex& index) {
            const QString symbol = model->symbolAt(index.row());
            if (!symbol.isEmpty()) {
                addSymbolToWatchlist(symbol);
                centerTabs_->setCurrentIndex(0);
            }
        };
    connect(
        callsTable_, &QTableView::doubleClicked, this, [this, addOption](const QModelIndex& index) {
            addOption(callsTable_, callsModel_, index);
        });
    connect(
        putsTable_, &QTableView::doubleClicked, this, [this, addOption](const QModelIndex& index) {
            addOption(putsTable_, putsModel_, index);
        });
    optionsLayout->addWidget(panel(tr("CALL"), callsTable_, optionsContainer), 1);
    optionsLayout->addWidget(panel(tr("PUT"), putsTable_, optionsContainer), 1);
    centerTabs_->addTab(optionsContainer, tr("期权链"));
    connect(centerTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2) {
            requestOptionChain();
        }
    });
    splitter->addWidget(centerTabs_);

    auto* quoteContainer = new QWidget(splitter);
    auto* quoteForm      = new QFormLayout(quoteContainer);
    quoteForm->setContentsMargins(12, 12, 12, 12);
    symbolLabel_       = new QLabel(selectedSymbol_, quoteContainer);
    nameLabel_         = new QLabel(tr("—"), quoteContainer);
    lastLabel_         = new QLabel(tr("—"), quoteContainer);
    changeLabel_       = new QLabel(tr("—"), quoteContainer);
    changePctLabel_    = new QLabel(tr("—"), quoteContainer);
    bidAskLabel_       = new QLabel(tr("—"), quoteContainer);
    volumeLabel_       = new QLabel(tr("—"), quoteContainer);
    openInterestLabel_ = new QLabel(tr("—"), quoteContainer);
    oiChangeLabel_     = new QLabel(tr("—"), quoteContainer);
    highLowLabel_      = new QLabel(tr("—"), quoteContainer);
    lastLabel_->setStyleSheet("font-size: 22px; font-weight: 700;");
    quoteForm->addRow(tr("合约"), symbolLabel_);
    quoteForm->addRow(tr("名称"), nameLabel_);
    quoteForm->addRow(tr("最新"), lastLabel_);
    quoteForm->addRow(tr("涨跌"), changeLabel_);
    quoteForm->addRow(tr("涨幅"), changePctLabel_);
    quoteForm->addRow(tr("买卖"), bidAskLabel_);
    quoteForm->addRow(tr("成交"), volumeLabel_);
    quoteForm->addRow(tr("持仓"), openInterestLabel_);
    quoteForm->addRow(tr("增仓"), oiChangeLabel_);
    quoteForm->addRow(tr("高/低"), highLowLabel_);
    splitter->addWidget(panel(tr("行情快照"), quoteContainer, splitter));

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 6);
    splitter->setStretchFactor(2, 2);
    setCentralWidget(splitter);
}

void MainWindow::buildOrderPanel(QWidget* parent) {
    auto* form = new QFormLayout(parent);
    form->setContentsMargins(12, 4, 12, 12);

    priceBox_ = new QDoubleSpinBox(parent);
    priceBox_->setRange(0.0, 1.0e9);
    priceBox_->setDecimals(2);
    priceBox_->setSingleStep(1.0);
    priceBox_->setValue(0.0);
    volumeBox_ = new QSpinBox(parent);
    volumeBox_->setRange(1, 1000);
    volumeBox_->setValue(1);
    riskCheck_ = new QCheckBox(tr("我确认柜台与合约，允许下单"), parent);

    auto* buttons      = new QWidget(parent);
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(6);
    buyButton_ = new QPushButton(tr("买开"), buttons);
    buyButton_->setEnabled(false);
    sellButton_ = new QPushButton(tr("卖开"), buttons);
    sellButton_->setEnabled(false);
    closeButton_ = new QPushButton(tr("平仓"), buttons);
    closeButton_->setEnabled(false);
    buyButton_->setStyleSheet("background:#c62828;color:white;font-weight:600;padding:9px;");
    sellButton_->setStyleSheet("background:#1b8f5a;color:white;font-weight:600;padding:9px;");
    closeButton_->setStyleSheet("background:#9a6b00;color:white;font-weight:600;padding:9px;");
    buttonLayout->addWidget(buyButton_, 1);
    buttonLayout->addWidget(sellButton_, 1);
    buttonLayout->addWidget(closeButton_, 1);

    orderStatus_ = new QLabel(tr("未连接交易账户"), parent);
    orderStatus_->setWordWrap(true);
    orderStatus_->setStyleSheet("color: #8b93a3;");

    form->addRow(tr("三键"), buttons);
    form->addRow(tr("价格"), priceBox_);
    form->addRow(tr("数量"), volumeBox_);
    form->addRow(riskCheck_);
    form->addRow(orderStatus_);

    connect(riskCheck_, &QCheckBox::toggled, this, [this] { updateOrderControls(); });
    connect(buyButton_, &QPushButton::clicked, this, [this] {
        submitOrder(Direction::Buy, Offset::Open);
    });
    connect(sellButton_, &QPushButton::clicked, this, [this] {
        submitOrder(Direction::Sell, Offset::Open);
    });
    connect(closeButton_, &QPushButton::clicked, this, [this] {
        qint64 longVolume  = 0;
        qint64 shortVolume = 0;
        for (const auto& position : positionsModel_->positions()) {
            if (position.instrumentId != selectedSymbol_) {
                continue;
            }
            longVolume += position.volumeLong;
            shortVolume += position.volumeShort;
        }
        if (longVolume > 0) {
            submitOrder(Direction::Sell, Offset::Close);
        } else if (shortVolume > 0) {
            submitOrder(Direction::Buy, Offset::Close);
        } else {
            orderStatus_->setText(tr("当前合约无持仓"));
        }
    });
}

void MainWindow::buildBottomDock() {
    auto* dock = new QDockWidget(tr("交易与账户"), this);
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);

    auto* content       = new QWidget(dock);
    auto* contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    auto* tabs = new QTabWidget(content);

    positionTable_ = makeTable(tabs, positionsModel_);
    installColumnMenu(positionTable_, QStringLiteral("positions"));
    connect(positionTable_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex& current) {
                if (!current.isValid() || current.row() >= positionsModel_->positions().size()) {
                    return;
                }
                const QString symbol = positionsModel_->positions().at(current.row()).instrumentId;
                if (!symbol.isEmpty() && symbol != selectedSymbol_) {
                    selectSymbol(symbol);
                }
            });
    tabs->addTab(positionTable_, tr("持仓"));

    auto* orderPage   = new QWidget(tabs);
    auto* orderLayout = new QVBoxLayout(orderPage);
    orderLayout->setContentsMargins(4, 4, 4, 4);
    auto* orderBar = new QHBoxLayout();
    cancelButton_  = new QPushButton(tr("撤销选中委托"), orderPage);
    cancelButton_->setEnabled(false);
    orderBar->addWidget(cancelButton_);
    orderBar->addStretch(1);
    orderLayout->addLayout(orderBar);
    orderTable_ = makeTable(orderPage, ordersModel_);
    installColumnMenu(orderTable_, QStringLiteral("orders"));
    orderLayout->addWidget(orderTable_, 1);
    connect(cancelButton_, &QPushButton::clicked, this, [this] { cancelSelectedOrder(); });
    connect(orderTable_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] {
        updateOrderControls();
    });
    tabs->addTab(orderPage, tr("委托"));

    tradeTable_ = makeTable(tabs, tradesModel_);
    installColumnMenu(tradeTable_, QStringLiteral("trades"));
    tabs->addTab(tradeTable_, tr("成交"));

    auto* accountPage = new QWidget(tabs);
    auto* accountForm = new QFormLayout(accountPage);
    accountBalance_   = new QLabel(tr("—"), accountPage);
    accountAvailable_ = new QLabel(tr("—"), accountPage);
    accountMargin_    = new QLabel(tr("—"), accountPage);
    accountRisk_      = new QLabel(tr("—"), accountPage);
    accountForm->addRow(tr("权益"), accountBalance_);
    accountForm->addRow(tr("可用"), accountAvailable_);
    accountForm->addRow(tr("保证金"), accountMargin_);
    accountForm->addRow(tr("风险度"), accountRisk_);
    tabs->addTab(accountPage, tr("账户"));

    auto* orderBox = new QWidget(content);
    buildOrderPanel(orderBox);
    contentLayout->addWidget(tabs, 1);
    contentLayout->addWidget(panel(tr("三键下单"), orderBox, content), 0);

    dock->setWidget(content);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->setMinimumHeight(280);
    dock->setMaximumHeight(360);
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
                nodeReady_ = state == NodeClient::State::Ready;
                if (!message.isEmpty()) {
                    statusBar()->showMessage(message);
                }
                if (connectionLabel_ != nullptr) {
                    connectionLabel_->setText(text);
                    connectionLabel_->setStyleSheet(
                        QStringLiteral("color: %1; padding: 0 10px;").arg(color));
                }
                if (state == NodeClient::State::Connected || state == NodeClient::State::Ready) {
                    CatalogQuery query;
                    query.classes = QStringLiteral("FUTURE,FUTURE_CONT,FUTURE_INDEX");
                    query.limit   = 3000;
                    nodeClient_->requestCatalog(query);
                    nodeClient_->requestQuotes(watchlist_);
                    nodeClient_->requestKlineHistory(selectedSymbol_, selectedDurationNs_, 200);
                    if (centerTabs_ != nullptr && centerTabs_->currentIndex() == 2) {
                        requestOptionChain();
                    }
                    nodeClient_->requestAccountSnapshot();
                    nodeClient_->requestOrders();
                    nodeClient_->requestPositions();
                    nodeClient_->requestTrades();
                }
                updateOrderControls();
            });

    connect(nodeClient_, &NodeClient::errorOccurred, this, [this](const QString& error) {
        statusBar()->showMessage(error, 8000);
        if (orderStatus_ != nullptr) {
            orderStatus_->setText(error);
        }
    });

    connect(nodeClient_, &NodeClient::quotesUpdated, this, [this](const QVector<Quote>& quotes) {
        marketStore_->applyQuotes(quotes);
        watchModel_->applyQuotes(quotes);
        futuresModel_->applyQuotes(quotes);
        callsModel_->applyQuotes(quotes);
        putsModel_->applyQuotes(quotes);
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
                updateQuotePanel();
                if (futuresStatus_ != nullptr) {
                    futuresStatus_->setText(tr("合约目录 %1 条").arg(instruments.size()));
                }
                // Catalog carries a last-price snapshot; use it as a temporary fallback
                // for the watchlist so the quote pane is useful before the first WS tick.
                QVector<Quote> fallback;
                for (const auto& instrument : instruments) {
                    if (watchlist_.contains(instrument.instrumentId) &&
                        instrument.lastPrice > 0.0) {
                        Quote quote;
                        quote.instrumentId = instrument.instrumentId;
                        quote.lastPrice    = instrument.lastPrice;
                        fallback.push_back(quote);
                    }
                }
                if (!fallback.isEmpty()) {
                    marketStore_->applyQuotes(fallback);
                    watchModel_->applyQuotes(fallback);
                    updateQuotePanel();
                }
                updateSearchCompletions(search_ != nullptr ? search_->text() : QString{});
                statusBar()->showMessage(tr("合约目录 %1 条").arg(instruments.size()), 5000);
            });

    connect(nodeClient_, &NodeClient::optionChainLoaded, this, [this](const OptionChain& chain) {
        callsModel_->setInstruments(chain.calls);
        putsModel_->setInstruments(chain.puts);
        statusBar()->showMessage(tr("期权链 %1: CALL %2 / PUT %3")
                                     .arg(chain.underlying)
                                     .arg(chain.calls.size())
                                     .arg(chain.puts.size()),
                                 5000);
    });

    connect(nodeClient_, &NodeClient::accountUpdated, this, [this](const Account& account) {
        updateAccountPanel(account);
    });
    connect(nodeClient_,
            &NodeClient::positionsUpdated,
            this,
            [this](const QVector<Position>& positions) {
                positionsModel_->applyPositions(positions);
                updateOrderControls();
            });
    connect(nodeClient_, &NodeClient::ordersUpdated, this, [this](const QVector<Order>& orders) {
        ordersModel_->applyOrders(orders);
        updateOrderControls();
    });
    connect(nodeClient_, &NodeClient::tradesUpdated, this, [this](const QVector<Trade>& trades) {
        tradesModel_->applyTrades(trades);
    });
    connect(nodeClient_, &NodeClient::orderAckReceived, this, [this](const OrderAck& ack) {
        orderStatus_->setText(ack.success ? tr("命令已接受: %1").arg(ack.orderId)
                                          : tr("下单失败: %1").arg(ack.error));
        updateOrderControls();
        if (nodeClient_ != nullptr) {
            nodeClient_->requestOrders();
            nodeClient_->requestPositions();
            nodeClient_->requestAccountSnapshot();
        }
    });
}

void MainWindow::addSymbolToWatchlist(const QString& symbol, bool request) {
    const QString trimmed = symbol.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    int row = watchlist_.indexOf(trimmed);
    if (row < 0) {
        watchlist_.push_back(trimmed);
        watchModel_->setSymbols(watchlist_);
        row = watchlist_.indexOf(trimmed);
    }
    if (row >= 0 && watchTable_ != nullptr) {
        watchTable_->selectRow(row);
        watchTable_->scrollTo(watchModel_->index(row, 0));
    }
    if (request) {
        selectSymbol(trimmed);
        if (nodeClient_ != nullptr && nodeClient_->state() != NodeClient::State::Disconnected) {
            nodeClient_->requestQuotes({trimmed});
        }
    }
}

namespace {
Quote catalogFallbackQuote(const InstrumentTableModel* model, const QString& symbol) {
    if (model == nullptr) {
        return {};
    }
    for (const auto& instrument : model->instruments()) {
        if (instrument.instrumentId == symbol && instrument.lastPrice > 0.0) {
            Quote quote;
            quote.instrumentId = symbol;
            quote.lastPrice    = instrument.lastPrice;
            return quote;
        }
    }
    return {};
}
} // namespace

QString MainWindow::resolveSymbol(const QString& input) const {
    QString   query     = input.trimmed();
    const int separator = query.indexOf(QStringLiteral("  "));
    if (separator >= 0) {
        query = query.left(separator).trimmed();
    }
    if (query.isEmpty() || futuresModel_ == nullptr) {
        return {};
    }

    QString best;
    int     bestScore = -1;
    for (const auto& instrument : futuresModel_->instruments()) {
        const int score = std::max({fuzzyScore(query, instrument.instrumentId),
                                    fuzzyScore(query, instrument.name),
                                    fuzzyScore(query, instrument.productName)});
        if (score > bestScore) {
            bestScore = score;
            best      = instrument.instrumentId;
        }
    }
    if (bestScore >= 0) {
        return best;
    }

    static const QRegularExpression rawSymbol(QStringLiteral("^[A-Za-z]+\\.[A-Za-z0-9]+$"));
    return rawSymbol.match(query).hasMatch() ? query : QString{};
}

void MainWindow::addSymbolFromInput(const QString& input) {
    QString raw = input;
    if (raw.trimmed().isEmpty() && futuresTable_ != nullptr) {
        const auto current = futuresTable_->currentIndex();
        if (current.isValid()) {
            raw = futuresModel_->symbolAt(futuresProxy_->mapToSource(current).row());
        }
    }
    const QString symbol = resolveSymbol(raw);
    if (symbol.isEmpty()) {
        statusBar()->showMessage(tr("没有匹配的合约: %1").arg(raw.trimmed()), 5000);
        return;
    }
    addSymbolToWatchlist(symbol);
    if (search_ != nullptr) {
        search_->clear();
    }
    statusBar()->showMessage(tr("已加入自选: %1").arg(symbol), 3000);
}

void MainWindow::updateSearchCompletions(const QString& input) {
    if (searchCompletions_ == nullptr || futuresModel_ == nullptr) {
        return;
    }
    const QString query = input.trimmed();
    if (query.isEmpty()) {
        searchCompletions_->setStringList({});
        return;
    }

    struct Candidate {
        int     score;
        QString text;
    };
    QVector<Candidate> candidates;
    for (const auto& instrument : futuresModel_->instruments()) {
        const int score = std::max({fuzzyScore(query, instrument.instrumentId),
                                    fuzzyScore(query, instrument.name),
                                    fuzzyScore(query, instrument.productName)});
        if (score >= 0) {
            candidates.push_back(
                {score, instrument.instrumentId + QStringLiteral("  ") + instrument.name});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score > b.score;
    });
    QStringList suggestions;
    for (int i = 0; i < candidates.size() && i < 20; ++i) {
        suggestions.push_back(candidates.at(i).text);
    }
    searchCompletions_->setStringList(suggestions);
    if (searchCompleter_ != nullptr && !suggestions.isEmpty()) {
        searchCompleter_->setCompletionPrefix(query);
        searchCompleter_->complete();
    }
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

    if (marketStore_->quote(symbol) == nullptr) {
        const Quote fallback = catalogFallbackQuote(futuresModel_, symbol);
        if (!fallback.instrumentId.isEmpty()) {
            marketStore_->applyQuotes({fallback});
            watchModel_->applyQuotes({fallback});
        }
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
    if (nodeClient_->state() == NodeClient::State::Disconnected) {
        return;
    }
    nodeClient_->requestQuotes({symbol});
    nodeClient_->requestKlineHistory(symbol, selectedDurationNs_, 200);
    if (centerTabs_ != nullptr && centerTabs_->currentIndex() == 2) {
        requestOptionChain();
    }
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
        if (nodeClient_->state() != NodeClient::State::Disconnected) {
            nodeClient_->requestKlineHistory(selectedSymbol_, selectedDurationNs_, 200);
        }
    }
    updateQuotePanel();
}

void MainWindow::updateQuotePanel() {
    if (symbolLabel_ == nullptr) {
        return;
    }
    const Quote* quote = marketStore_->quote(selectedSymbol_);
    symbolLabel_->setText(selectedSymbol_);
    const Instrument* instrument = findInstrument(selectedSymbol_);
    nameLabel_->setText(instrument != nullptr && !instrument->name.isEmpty() ? instrument->name
                                                                             : QStringLiteral("—"));
    if (quote == nullptr) {
        chart_->setQuote(Quote{});
        lastLabel_->setText(tr("—"));
        changeLabel_->setText(tr("—"));
        changePctLabel_->setText(tr("—"));
        bidAskLabel_->setText(tr("—"));
        volumeLabel_->setText(tr("—"));
        openInterestLabel_->setText(tr("—"));
        oiChangeLabel_->setText(tr("—"));
        highLowLabel_->setText(tr("—"));
        updateOrderControls();
        return;
    }

    chart_->setQuote(*quote);
    lastLabel_->setText(priceText(quote->lastPrice));
    const double  change  = std::isfinite(quote->lastPrice) && std::isfinite(quote->preSettlement)
                                ? quote->lastPrice - quote->preSettlement
                                : std::numeric_limits<double>::quiet_NaN();
    const double  percent = changePercent(*quote);
    const QString color =
        !std::isfinite(change) ? "#8b93a3" : (change >= 0.0 ? "#e43c4a" : "#2fbf71");
    changeLabel_->setText(
        std::isfinite(change)
            ? QStringLiteral("%1%2").arg(change >= 0.0 ? "+" : "").arg(change, 0, 'f', 2)
            : QStringLiteral("—"));
    changeLabel_->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;").arg(color));
    changePctLabel_->setText(changeText(percent));
    changePctLabel_->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;").arg(color));
    bidAskLabel_->setText(
        QStringLiteral("%1 / %2").arg(priceText(quote->bidPrice1), priceText(quote->askPrice1)));
    volumeLabel_->setText(quote->volume > 0 ? QString::number(quote->volume) : QStringLiteral("—"));
    openInterestLabel_->setText(std::isfinite(quote->openInterest)
                                    ? QString::number(quote->openInterest, 'f', 0)
                                    : QStringLiteral("—"));
    if (std::isfinite(quote->openInterest) && std::isfinite(quote->preOpenInterest)) {
        const double oiChange = quote->openInterest - quote->preOpenInterest;
        oiChangeLabel_->setText(
            QStringLiteral("%1%2").arg(oiChange >= 0.0 ? "+" : "").arg(oiChange, 0, 'f', 0));
        oiChangeLabel_->setStyleSheet(QStringLiteral("font-weight: 600; color: %1;")
                                          .arg(oiChange >= 0.0 ? "#e43c4a" : "#2fbf71"));
    } else {
        oiChangeLabel_->setText(QStringLiteral("—"));
        oiChangeLabel_->setStyleSheet("color: #8b93a3;");
    }
    highLowLabel_->setText(
        QStringLiteral("%1 / %2").arg(priceText(quote->high), priceText(quote->low)));
    if (priceBox_ != nullptr && priceBox_->value() <= 0.0 && std::isfinite(quote->lastPrice)) {
        priceBox_->setValue(quote->lastPrice);
    }
    updateOrderControls();
}

void MainWindow::updateAccountPanel(const Account& account) {
    accountLoaded_ = true;
    if (accountBalance_ == nullptr) {
        return;
    }
    accountBalance_->setText(priceText(account.balance));
    accountAvailable_->setText(priceText(account.available));
    accountMargin_->setText(priceText(account.margin));
    accountRisk_->setText(std::isfinite(account.riskRatio)
                              ? QString::number(account.riskRatio * 100.0, 'f', 2) + "%"
                              : QStringLiteral("—"));
    updateOrderControls();
}

void MainWindow::updateOrderControls() {
    if (buyButton_ == nullptr) {
        return;
    }
    const bool canSubmit = nodeReady_ && accountLoaded_ && riskCheck_->isChecked();
    buyButton_->setEnabled(canSubmit);
    sellButton_->setEnabled(canSubmit);
    qint64 longVolume  = 0;
    qint64 shortVolume = 0;
    for (const auto& position : positionsModel_->positions()) {
        if (position.instrumentId == selectedSymbol_) {
            longVolume += position.volumeLong;
            shortVolume += position.volumeShort;
        }
    }
    closeButton_->setEnabled(canSubmit && (longVolume > 0 || shortVolume > 0));
    if (cancelButton_ != nullptr && orderTable_ != nullptr) {
        const auto current = orderTable_->currentIndex();
        cancelButton_->setEnabled(current.isValid() && ordersModel_->isOrderAlive(current.row()));
    }
}

void MainWindow::submitOrder(Direction direction, Offset offset) {
    if (nodeClient_ == nullptr || !nodeReady_ || !accountLoaded_ || !riskCheck_->isChecked()) {
        orderStatus_->setText(tr("交易未就绪或未确认风险"));
        return;
    }
    if (priceBox_->value() <= 0.0 || volumeBox_->value() <= 0) {
        orderStatus_->setText(tr("价格/数量无效"));
        return;
    }

    OrderRequest request;
    request.instrumentId = selectedSymbol_;
    request.direction    = direction;
    request.offset       = offset;
    request.priceType    = PriceType::Limit;
    request.limitPrice   = priceBox_->value();
    request.volume       = volumeBox_->value();
    nodeClient_->placeOrder(request);
    orderStatus_->setText(tr("已提交 %1 %2 手 @ %3")
                              .arg(selectedSymbol_)
                              .arg(request.volume)
                              .arg(priceText(request.limitPrice)));
    updateOrderControls();
}

void MainWindow::cancelSelectedOrder() {
    if (nodeClient_ == nullptr || orderTable_ == nullptr) {
        return;
    }
    const auto index = orderTable_->currentIndex();
    if (!index.isValid() || !ordersModel_->isOrderAlive(index.row())) {
        return;
    }
    const QString orderId = ordersModel_->orderIdAt(index.row());
    if (orderId.isEmpty()) {
        return;
    }
    nodeClient_->cancelOrder(orderId);
    orderStatus_->setText(tr("撤单请求: %1").arg(orderId));
}

const Instrument* MainWindow::findInstrument(const QString& symbol) const {
    if (futuresModel_ == nullptr) {
        return nullptr;
    }
    for (const auto& instrument : futuresModel_->instruments()) {
        if (instrument.instrumentId == symbol) {
            return &instrument;
        }
    }
    for (const auto* model : {callsModel_, putsModel_}) {
        if (model == nullptr) {
            continue;
        }
        for (const auto& instrument : model->instruments()) {
            if (instrument.instrumentId == symbol) {
                return &instrument;
            }
        }
    }
    return nullptr;
}

void MainWindow::installColumnMenu(QTableView* view, const QString& key) {
    if (view == nullptr || view->model() == nullptr || view->horizontalHeader() == nullptr) {
        return;
    }
    auto* header = view->horizontalHeader();
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    const auto loadSettings = [view, key] {
        QSettings settings;
        for (int i = 0; i < view->model()->columnCount(); ++i) {
            const QString name = QStringLiteral("ui/columns/%1/%2").arg(key).arg(i);
            if (settings.contains(name)) {
                view->setColumnHidden(i, !settings.value(name, true).toBool());
            }
        }
    };
    loadSettings();

    connect(header,
            &QWidget::customContextMenuRequested,
            this,
            [this, view, key, header](const QPoint& pos) {
                QMenu    menu(view);
                QAction* reset = menu.addAction(tr("显示全部列"));
                menu.addSeparator();
                for (int i = 0; i < view->model()->columnCount(); ++i) {
                    const QString title = view->model()->headerData(i, Qt::Horizontal).toString();
                    QAction* action = menu.addAction(title.isEmpty() ? QString::number(i) : title);
                    action->setCheckable(true);
                    action->setChecked(!view->isColumnHidden(i));
                    connect(action, &QAction::toggled, this, [view, i, key](bool checked) {
                        view->setColumnHidden(i, !checked);
                        QSettings settings;
                        settings.setValue(QStringLiteral("ui/columns/%1/%2").arg(key).arg(i),
                                          checked);
                    });
                }
                if (menu.exec(header->viewport()->mapToGlobal(pos)) == reset) {
                    QSettings settings;
                    for (int i = 0; i < view->model()->columnCount(); ++i) {
                        view->setColumnHidden(i, false);
                        settings.setValue(QStringLiteral("ui/columns/%1/%2").arg(key).arg(i), true);
                    }
                }
            });
}

void MainWindow::requestOptionChain() {
    if (nodeClient_ == nullptr || selectedSymbol_.isEmpty()) {
        return;
    }
    nodeClient_->requestOptionChain(selectedSymbol_);
}

void MainWindow::applyTheme() {
    chart_->setTheme(dark_);
    themeButton_->setText(dark_ ? tr("浅色") : tr("深色"));
    qApp->setStyleSheet(themeStyle(dark_));
}

} // namespace tqsdk::ui
