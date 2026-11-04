#include "app/MainWindow.h"

#include "app/NodeDataController.h"
#include "app/SymbolController.h"
#include "app/Theme.h"
#include "app/TradingController.h"
#include "app/WorkspaceLayout.h"

#include "app/IndicatorController.h"

#include "chart/KLineChartWidget.h"
#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "model/TradingModels.h"
#include "net/NodeClient.h"
#include "widgets/MarketToolBar.h"
#include "widgets/OrderBookPanel.h"
#include "widgets/OrderTicketPanel.h"
#include "widgets/QuoteSnapshotPanel.h"
#include "widgets/Toast.h"
#include "widgets/TradingPanels.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QJsonDocument>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <utility>

namespace tqsdk::ui {

MainWindow::MainWindow(NodeClient* nodeClient, QWidget* parent)
    : QMainWindow(parent),
      nodeClient_(nodeClient) {
    setWindowTitle(tr("TQSDK Terminal (Qt)"));
    resize(1480, 940);
    setDockNestingEnabled(true);
    dark_ = QSettings().value(QStringLiteral("ui/theme/dark"), true).toBool();

    marketStore_    = new MarketStore(this);
    watchModel_     = new QuoteTableModel(this);
    futuresModel_   = new InstrumentTableModel(this);
    callsModel_     = new InstrumentTableModel(this);
    putsModel_      = new InstrumentTableModel(this);
    positionsModel_ = new PositionTableModel(this);
    ordersModel_    = new OrderTableModel(this);
    tradesModel_    = new TradeTableModel(this);
    tapeModel_      = new TradeTapeModel(this);
    futuresProxy_   = new QSortFilterProxyModel(this);
    futuresProxy_->setSourceModel(futuresModel_);
    futuresProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    futuresProxy_->setFilterKeyColumn(-1);

    symbolController_ = new SymbolController(
        nodeClient_, watchModel_, futuresModel_, callsModel_, putsModel_, marketStore_, this);
    symbolController_->loadSettings();
    connect(symbolController_,
            &SymbolController::selectedSymbolChanged,
            this,
            [this](const QString& symbol) { applySelectedSymbol(symbol); });
    connect(symbolController_,
            &SymbolController::intervalChanged,
            this,
            [this](qint64 duration, const QString& label) { applyInterval(duration, label); });
    connect(symbolController_,
            &SymbolController::statusMessage,
            this,
            [this](const QString& message, int timeout) {
                statusBar()->showMessage(message, timeout);
            });
    connect(futuresModel_, &InstrumentTableModel::instrumentsLoaded, this, [this] {
        updateSearchCompletions(marketToolBar_ != nullptr ? marketToolBar_->searchText()
                                                          : QString{});
    });

    buildUi();
    applyTheme();
    connectNode();

    const QString initialSymbol = symbolController_->selectedSymbol();
    const int     selectedRow   = symbolController_->watchRow(initialSymbol);
    if (selectedRow >= 0) {
        workspace_->watchTable()->selectRow(selectedRow);
    }
    symbolController_->selectSymbol(initialSymbol);
}

void MainWindow::buildUi() {
    buildToolbar();
    workspace_ = new WorkspaceLayout(this,
                                     watchModel_,
                                     futuresModel_,
                                     futuresProxy_,
                                     callsModel_,
                                     putsModel_,
                                     positionsModel_,
                                     ordersModel_,
                                     tradesModel_,
                                     tapeModel_,
                                     this);

    toast_ = new Toast(this);

    chartRefreshTimer_ = new QTimer(this);
    chartRefreshTimer_->setSingleShot(true);
    chartRefreshTimer_->setInterval(500);
    connect(chartRefreshTimer_, &QTimer::timeout, this, &MainWindow::flushChartBars);

    selectionTimer_ = new QTimer(this);
    selectionTimer_->setSingleShot(true);
    selectionTimer_->setInterval(120);
    connect(selectionTimer_, &QTimer::timeout, this, [this] {
        const QString symbol = pendingSelection_;
        pendingSelection_.clear();
        if (!symbol.isEmpty() && symbol != symbolController_->selectedSymbol()) {
            symbolController_->selectSymbol(symbol);
        }
    });

    connect(workspace_,
            &WorkspaceLayout::watchSymbolActivated,
            this,
            [this](const QString& symbol) { scheduleSymbolSelection(symbol); });
    connect(workspace_,
            &WorkspaceLayout::watchRemoveRequested,
            this,
            [this](const QString& symbol) { symbolController_->removeFromWatchlist(symbol); });
    connect(workspace_,
            &WorkspaceLayout::watchMoveRequested,
            this,
            [this](const QString& symbol, int delta) {
                symbolController_->moveWatchlistSymbol(symbol, delta);
            });
    connect(symbolController_, &SymbolController::watchlistChanged, this, [this] {
        const int row = symbolController_->watchRow(symbolController_->selectedSymbol());
        if (row >= 0 && workspace_ != nullptr) {
            workspace_->watchTable()->selectRow(row);
        }
    });
    connect(watchModel_,
            &QuoteTableModel::symbolsReordered,
            symbolController_,
            &SymbolController::setWatchlistOrder);
    connect(workspace_,
            &WorkspaceLayout::futuresSymbolActivated,
            this,
            [this](const QString& symbol) { addSymbolToWatchlist(symbol); });
    connect(workspace_,
            &WorkspaceLayout::optionSymbolActivated,
            this,
            [this](const QString& symbol) { addSymbolToWatchlist(symbol); });
    connect(workspace_, &WorkspaceLayout::positionActivated, this, [this](const QString& symbol) {
        if (!symbol.isEmpty() && symbol != symbolController_->selectedSymbol()) {
            symbolController_->selectSymbol(symbol);
        }
        updatePositionDetail();
    });
    connect(workspace_, &WorkspaceLayout::orderSelectionChanged, this, [this] {
        updateOrderControls();
    });
    connect(workspace_, &WorkspaceLayout::orderSelected, this, [this](const QString& orderId) {
        if (workspace_ != nullptr && workspace_->tradesPanel() != nullptr) {
            workspace_->tradesPanel()->selectOrder(orderId);
        }
    });
    connect(workspace_, &WorkspaceLayout::cancelRequested, this, [this] {
        if (tradingController_ != nullptr) {
            tradingController_->cancelSelectedOrder();
        }
    });
    connect(workspace_, &WorkspaceLayout::futuresVisibilityChanged, this, [this](bool visible) {
        if (nodeDataController_ != nullptr) {
            nodeDataController_->setFuturesVisible(visible);
        }
    });
    connect(workspace_, &WorkspaceLayout::optionsVisibilityChanged, this, [this](bool visible) {
        if (nodeDataController_ != nullptr) {
            nodeDataController_->setOptionsVisible(visible);
        }
        if (visible && symbolController_ != nullptr) {
            symbolController_->requestOptionChain();
        }
    });

    indicatorController_ =
        new IndicatorController(workspace_->chart(), menuBar(), statusBar(), toast_, this);

    buildTradingController();
    buildViewMenu();
    buildTradingShortcuts();
    workspace_->restoreLayout();
    workspace_->chart()->setTitle(symbolController_->selectedSymbol(),
                                  symbolController_->selectedInterval());
    statusBar()->showMessage(tr("Qt UI · 等待 node"));
}

void MainWindow::buildToolbar() {
    marketToolBar_ = new MarketToolBar(this);
    addToolBar(marketToolBar_);
    connect(marketToolBar_, &MarketToolBar::searchTextChanged, this, [this](const QString& text) {
        futuresProxy_->setFilterFixedString(text);
        updateSearchCompletions(text);
    });
    connect(marketToolBar_, &MarketToolBar::symbolSubmitted, this, [this](const QString& input) {
        addSymbolFromInput(input);
    });
    connect(marketToolBar_, &MarketToolBar::addRequested, this, [this](const QString& input) {
        addSymbolFromInput(input);
    });
    connect(marketToolBar_,
            &MarketToolBar::intervalRequested,
            this,
            [this](qint64 duration, const QString& label) {
                symbolController_->setInterval(duration, label);
            });
    connect(marketToolBar_, &MarketToolBar::themeToggleRequested, this, [this] {
        dark_ = !dark_;
        applyTheme();
    });
}

void MainWindow::buildViewMenu() {
    auto* viewMenu = menuBar()->addMenu(tr("视图 / 组件"));
    workspace_->addDockActions(viewMenu);
    viewMenu->addSeparator();
    QAction* chartPopout = viewMenu->addAction(tr("图表弹出 / 收回"));
    connect(chartPopout, &QAction::triggered, this, [this] { workspace_->toggleChartPopout(); });
    QAction* reset = viewMenu->addAction(tr("恢复默认布局"));
    connect(reset, &QAction::triggered, this, [this] { workspace_->resetLayout(); });
    marketToolBar_->setComponentsMenu(viewMenu);
}

void MainWindow::buildTradingShortcuts() {
    auto*      tradeMenu = menuBar()->addMenu(tr("交易"));
    const auto addAction =
        [this, tradeMenu](const QString& text, const QKeySequence& key, auto&& callback) {
            auto* action = new QAction(text, this);
            action->setShortcut(key);
            action->setShortcutContext(Qt::WindowShortcut);
            connect(action, &QAction::triggered, this, std::forward<decltype(callback)>(callback));
            this->addAction(action);
            tradeMenu->addAction(action);
        };

    addAction(tr("买开"), QKeySequence(QStringLiteral("Alt+B")), [this] {
        if (tradingController_ != nullptr) {
            tradingController_->submit(Direction::Buy, Offset::Open);
        }
    });
    addAction(tr("卖开"), QKeySequence(QStringLiteral("Alt+S")), [this] {
        if (tradingController_ != nullptr) {
            tradingController_->submit(Direction::Sell, Offset::Open);
        }
    });
    addAction(tr("平仓"), QKeySequence(QStringLiteral("Alt+C")), [this] {
        if (tradingController_ != nullptr) {
            tradingController_->closePosition();
        }
    });
    addAction(tr("撤销选中委托"), QKeySequence(QStringLiteral("Alt+X")), [this] {
        if (tradingController_ != nullptr) {
            tradingController_->cancelSelectedOrder();
        }
    });
    tradeMenu->addSeparator();
    auto* hint =
        tradeMenu->addAction(tr("快捷键：Alt+B 买开 / Alt+S 卖开 / Alt+C 平仓 / Alt+X 撤单"));
    hint->setEnabled(false);
}

void MainWindow::buildTradingController() {
    OrderTicketPanel* ticket = workspace_->orderTicket();
    ticket->setSymbol(symbolController_->selectedSymbol());
    connect(ticket, &OrderTicketPanel::symbolSubmitted, this, [this](const QString& input) {
        const QString symbol = symbolController_->resolveSymbol(input);
        if (symbol.isEmpty()) {
            workspace_->orderTicket()->setStatus(tr("没有匹配的交易合约"), true);
            return;
        }
        addSymbolToWatchlist(symbol);
        workspace_->orderTicket()->setSymbol(symbolController_->selectedSymbol());
    });

    tradingController_ = new TradingController(
        nodeClient_, ticket, positionsModel_, ordersModel_, workspace_->ordersPanel(), this);
    connect(tradingController_, &TradingController::refreshRequested, this, [this] {
        if (nodeClient_ != nullptr) {
            nodeClient_->requestOrders();
            nodeClient_->requestPositions();
            nodeClient_->requestAccountSnapshot();
        }
    });
    connect(ticket, &OrderTicketPanel::buyOpenRequested, this, [this] {
        tradingController_->submit(Direction::Buy, Offset::Open);
    });
    connect(ticket, &OrderTicketPanel::sellOpenRequested, this, [this] {
        tradingController_->submit(Direction::Sell, Offset::Open);
    });
    connect(ticket, &OrderTicketPanel::closeRequested, this, [this] {
        tradingController_->closePosition();
    });
    tradingController_->setSelectedSymbol(symbolController_->selectedSymbol());
}

void MainWindow::connectNode() {
    if (nodeClient_ == nullptr) {
        return;
    }

    nodeDataController_ = new NodeDataController(nodeClient_,
                                                 marketStore_,
                                                 watchModel_,
                                                 futuresModel_,
                                                 callsModel_,
                                                 putsModel_,
                                                 positionsModel_,
                                                 ordersModel_,
                                                 tradesModel_,
                                                 tapeModel_,
                                                 this);
    nodeDataController_->setSelectedContract(symbolController_->selectedSymbol(),
                                             symbolController_->selectedDurationNs());
    nodeDataController_->setFuturesVisible(workspace_->futuresDock()->isVisible());
    nodeDataController_->setOptionsVisible(workspace_->optionsDock()->isVisible());

    connect(nodeDataController_,
            &NodeDataController::stateChanged,
            this,
            [this](NodeClient::State state, const QString& message) {
                const bool stateChanged = lastNodeState_ != static_cast<int>(state);
                lastNodeState_          = static_cast<int>(state);

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
                if (tradingController_ != nullptr) {
                    tradingController_->setNodeReady(state == NodeClient::State::Ready);
                }
                if (stateChanged) {
                    statusBar()->showMessage(state == NodeClient::State::Ready ? tr("行情就绪")
                                                                               : message);
                }
                marketToolBar_->setConnectionState(text, color);
                if (state == NodeClient::State::Connected || state == NodeClient::State::Ready) {
                    symbolController_->requestInitialData();
                    if (workspace_->optionsDock()->isVisible()) {
                        symbolController_->requestOptionChain();
                    }
                }
                updateOrderControls();
            });
    connect(nodeDataController_,
            &NodeDataController::errorOccurred,
            this,
            [this](const QString& error) {
                statusBar()->showMessage(error, 8000);
                workspace_->orderTicket()->setStatus(error, true);
            });
    connect(nodeDataController_, &NodeDataController::quotesApplied, this, [this] {
        updateQuotePanel();
    });
    connect(nodeDataController_,
            &NodeDataController::klineSeriesChanged,
            this,
            [this](const QString& symbol, qint64 durationNs, const QVector<KlineBar>& bars) {
                if (indicatorController_ != nullptr) {
                    indicatorController_->setBars(symbol, durationNs, bars);
                }
                queueChartBars(symbol, durationNs, bars);
            });
    connect(nodeDataController_,
            &NodeDataController::catalogLoaded,
            this,
            [this](const QVector<Instrument>& instruments) {
                QStringList suggestions;
                suggestions.reserve(instruments.size());
                for (const auto& instrument : instruments) {
                    suggestions.push_back(instrument.instrumentId + QStringLiteral("  ") +
                                          instrument.name);
                }
                workspace_->orderTicket()->setCompletions(suggestions);

                QVector<Quote>    fallback;
                const QStringList watchlist = symbolController_->watchlist();
                for (const auto& instrument : instruments) {
                    if (!watchlist.contains(instrument.instrumentId) ||
                        instrument.lastPrice <= 0.0) {
                        continue;
                    }
                    // Catalog last_price is a stale snapshot (often yesterday's
                    // close). It must never overwrite a live quote already
                    // delivered over WS/REST.
                    if (marketStore_->quote(instrument.instrumentId) != nullptr) {
                        continue;
                    }
                    Quote quote;
                    quote.instrumentId = instrument.instrumentId;
                    quote.lastPrice    = instrument.lastPrice;
                    fallback.push_back(quote);
                }
                if (!fallback.isEmpty()) {
                    marketStore_->applyQuotes(fallback);
                    watchModel_->applyQuotes(fallback);
                }
                updateQuotePanel();
                updateSearchCompletions(marketToolBar_->searchText());
                statusBar()->showMessage(tr("合约目录 %1 条").arg(instruments.size()), 5000);
            });
    connect(nodeDataController_,
            &NodeDataController::optionChainLoaded,
            this,
            [this](const OptionChain& chain) {
                statusBar()->showMessage(tr("期权链 %1: CALL %2 / PUT %3")
                                             .arg(chain.underlying)
                                             .arg(chain.calls.size())
                                             .arg(chain.puts.size()),
                                         5000);
            });
    connect(nodeDataController_,
            &NodeDataController::accountChanged,
            this,
            [this](const Account& account) { updateAccountPanel(account); });
    connect(nodeDataController_, &NodeDataController::positionsChanged, this, [this] {
        updateOrderControls();
        updatePositionDetail();
    });
    connect(nodeDataController_, &NodeDataController::ordersChanged, this, [this] {
        updateOrderControls();
    });
    connect(nodeDataController_,
            &NodeDataController::orderAckReceived,
            this,
            [this](const OrderAck& ack) {
                if (!ack.success && toast_ != nullptr) {
                    toast_->showMessage(tr("下单失败: %1").arg(ack.error), true);
                }
                if (tradingController_ != nullptr) {
                    tradingController_->handleOrderAck(ack);
                }
            });
    connect(nodeDataController_,
            &NodeDataController::orderFailed,
            this,
            [this](const QString& orderId, const QString& message) {
                const QString text = tr("委托 %1 失败: %2").arg(orderId, message);
                if (toast_ != nullptr) {
                    toast_->showMessage(text, true);
                }
                if (workspace_ != nullptr && workspace_->orderTicket() != nullptr) {
                    workspace_->orderTicket()->setStatus(text, true);
                }
            });
}

void MainWindow::addSymbolToWatchlist(const QString& symbol, bool request) {
    symbolController_->addSymbolToWatchlist(symbol, request);
    const int row = symbolController_->watchRow(symbol);
    if (row >= 0) {
        workspace_->watchTable()->selectRow(row);
        workspace_->watchTable()->scrollTo(watchModel_->index(row, 0));
    }
}

void MainWindow::addSymbolFromInput(const QString& input) {
    QString raw = input;
    if (raw.trimmed().isEmpty()) {
        const auto current = workspace_->futuresTable()->currentIndex();
        if (current.isValid()) {
            const auto source = futuresProxy_->mapToSource(current);
            raw               = futuresModel_->symbolAt(source.row());
        }
    }
    const QString symbol = symbolController_->addSymbolFromInput(raw);
    if (!symbol.isEmpty()) {
        const int row = symbolController_->watchRow(symbol);
        if (row >= 0) {
            workspace_->watchTable()->selectRow(row);
        }
        marketToolBar_->clearSearch();
    }
}

void MainWindow::updateSearchCompletions(const QString& input) {
    if (marketToolBar_ != nullptr && symbolController_ != nullptr) {
        marketToolBar_->setCompletions(symbolController_->searchCompletions(input));
    }
}

void MainWindow::scheduleSymbolSelection(const QString& symbol) {
    if (symbol.isEmpty() || symbol == symbolController_->selectedSymbol()) {
        return;
    }
    pendingSelection_ = symbol;
    if (selectionTimer_ != nullptr && !selectionTimer_->isActive()) {
        selectionTimer_->start();
    }
}

void MainWindow::applySelectedSymbol(const QString& symbol) {
    if (selectionTimer_ != nullptr) {
        selectionTimer_->stop();
    }
    pendingSelection_.clear();
    workspace_->orderTicket()->setSymbol(symbol);
    const int watchRow = symbolController_->watchRow(symbol);
    if (watchRow >= 0 && workspace_->watchTable()->currentIndex().row() != watchRow) {
        workspace_->watchTable()->selectRow(watchRow);
    }
    tapeModel_->clear();
    workspace_->quotePanel()->setSymbol(symbol);
    workspace_->orderBookPanel()->setSymbol(symbol);
    if (tradingController_ != nullptr) {
        tradingController_->setSelectedSymbol(symbol);
    }
    if (nodeDataController_ != nullptr) {
        nodeDataController_->setSelectedContract(symbol, symbolController_->selectedDurationNs());
    }
    workspace_->setChartSymbol(symbol);
    if (indicatorController_ != nullptr) {
        indicatorController_->setBars(
            symbol,
            symbolController_->selectedDurationNs(),
            marketStore_->bars(symbol, symbolController_->selectedDurationNs()));
    }
    workspace_->chart()->setTitle(symbol, symbolController_->selectedInterval());
    if (chartRefreshTimer_ != nullptr) {
        chartRefreshTimer_->stop();
    }
    pendingChartBars_.clear();
    workspace_->chart()->setBars(
        marketStore_->bars(symbol, symbolController_->selectedDurationNs()));
    updateQuotePanel();
}

void MainWindow::applyInterval(qint64 durationNs, const QString& label) {
    Q_UNUSED(durationNs);
    workspace_->chart()->setTitle(symbolController_->selectedSymbol(), label);
    if (nodeDataController_ != nullptr) {
        nodeDataController_->setSelectedContract(symbolController_->selectedSymbol(),
                                                 symbolController_->selectedDurationNs());
    }
    if (indicatorController_ != nullptr) {
        indicatorController_->setBars(symbolController_->selectedSymbol(),
                                      symbolController_->selectedDurationNs(),
                                      marketStore_->bars(symbolController_->selectedSymbol(),
                                                         symbolController_->selectedDurationNs()));
    }
    if (chartRefreshTimer_ != nullptr) {
        chartRefreshTimer_->stop();
    }
    pendingChartBars_.clear();
    workspace_->chart()->setBars(marketStore_->bars(symbolController_->selectedSymbol(),
                                                    symbolController_->selectedDurationNs()));
    updateQuotePanel();
}

void MainWindow::queueChartBars(const QString&           symbol,
                                qint64                   durationNs,
                                const QVector<KlineBar>& bars) {
    if (symbolController_ == nullptr || symbol != symbolController_->selectedSymbol() ||
        durationNs != symbolController_->selectedDurationNs()) {
        return;
    }
    pendingChartSymbol_   = symbol;
    pendingChartDuration_ = durationNs;
    pendingChartBars_     = bars;
    if (chartRefreshTimer_ != nullptr && !chartRefreshTimer_->isActive()) {
        chartRefreshTimer_->start();
    }
}

void MainWindow::flushChartBars() {
    if (symbolController_ == nullptr || pendingChartBars_.isEmpty()) {
        return;
    }
    if (pendingChartSymbol_ != symbolController_->selectedSymbol() ||
        pendingChartDuration_ != symbolController_->selectedDurationNs()) {
        pendingChartBars_.clear();
        return;
    }
    workspace_->chart()->setBars(pendingChartBars_);
    pendingChartBars_.clear();
}

void MainWindow::updateQuotePanel() {
    const QString     symbol     = symbolController_->selectedSymbol();
    const Quote*      quote      = marketStore_->quote(symbol);
    const Instrument* instrument = symbolController_->findInstrument(symbol);

    workspace_->quotePanel()->setSymbol(symbol);
    workspace_->quotePanel()->setQuote(quote, instrument != nullptr ? instrument->name : QString{});
    workspace_->orderBookPanel()->setSymbol(symbol);
    workspace_->orderBookPanel()->setQuote(quote);
    workspace_->chart()->setQuote(quote != nullptr ? *quote : Quote{});
    if (quote != nullptr) {
        tapeModel_->applyQuote(*quote);
        workspace_->orderTicket()->setPriceIfEmpty(quote->lastPrice);
    }
    updateOrderControls();
}

void MainWindow::updateAccountPanel(const Account& account) {
    workspace_->accountPanel()->setAccount(account);
    if (tradingController_ != nullptr) {
        tradingController_->setAccountLoaded(true);
    }
}

void MainWindow::updateOrderControls() {
    if (tradingController_ != nullptr) {
        tradingController_->updateControls();
    }
}

void MainWindow::updatePositionDetail() {
    const auto current = workspace_->positionsPanel()->view()->currentIndex();
    if (!current.isValid() || current.row() >= positionsModel_->positions().size()) {
        workspace_->positionDetailPanel()->clear();
        return;
    }
    workspace_->positionDetailPanel()->setPosition(&positionsModel_->positions().at(current.row()));
}

void MainWindow::applyTheme() {
    workspace_->chart()->setTheme(dark_);
    marketToolBar_->setThemeLabel(dark_);
    qApp->setStyleSheet(QString{});
    qApp->setPalette(applicationPalette(dark_));
    QSettings().setValue(QStringLiteral("ui/theme/dark"), dark_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (symbolController_ != nullptr) {
        symbolController_->saveSettings();
    }
    if (workspace_ != nullptr) {
        workspace_->saveLayout();
    }
    QMainWindow::closeEvent(event);
}

} // namespace tqsdk::ui
