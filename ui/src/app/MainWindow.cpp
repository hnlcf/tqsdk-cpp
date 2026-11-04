#include "app/MainWindow.h"

#include "chart/KLineChartWidget.h"
#include "model/Formatting.h"
#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "model/TradingModels.h"
#include "net/NodeClient.h"
#include "widgets/OrderBookPanel.h"
#include "widgets/OrderTicketPanel.h"
#include "widgets/QuoteSnapshotPanel.h"
#include "widgets/TablePanel.h"
#include "widgets/TradeTapePanel.h"
#include "widgets/TradingPanels.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCompleter>
#include <QDialog>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSize>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QStringListModel>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tqsdk::ui {
namespace {

// 工作区布局版本：dock 组件集合变化时 bump，避免旧 saveState() 把新 dock
// 恢复到不合理的拆分位置。表格列/图表状态有独立 key，不受此版本影响。
constexpr int kWorkspaceVersion = 3;

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
    setDockNestingEnabled(true);

    workspaceSaveTimer_ = new QTimer(this);
    installEventFilter(this);
    workspaceSaveTimer_->setSingleShot(true);
    workspaceSaveTimer_->setInterval(250);
    connect(workspaceSaveTimer_, &QTimer::timeout, this, [this] { saveWorkspace(); });

    watchlist_ = {QStringLiteral("DCE.i2701"),
                  QStringLiteral("DCE.jm2701"),
                  QStringLiteral("CZCE.SA701"),
                  QStringLiteral("SHFE.rb2701"),
                  QStringLiteral("SHFE.au2701"),
                  QStringLiteral("SHFE.ag2701"),
                  QStringLiteral("SHFE.cu2701"),
                  QStringLiteral("DCE.m2701"),
                  QStringLiteral("DCE.p2701"),
                  QStringLiteral("DCE.y2701"),
                  QStringLiteral("CZCE.MA701"),
                  QStringLiteral("CZCE.TA701"),
                  QStringLiteral("CFFEX.IF2612"),
                  QStringLiteral("CFFEX.IC2612"),
                  QStringLiteral("SHFE.al2701"),
                  QStringLiteral("SHFE.ni2701")};
    {
        QSettings         settings;
        const QStringList saved = settings.value(QStringLiteral("ui/watchlist")).toStringList();
        if (!saved.isEmpty()) {
            watchlist_ = saved;
        }
        selectedSymbol_ =
            settings.value(QStringLiteral("ui/chart/symbol"), watchlist_.value(0, selectedSymbol_))
                .toString();
        selectedInterval_ =
            settings.value(QStringLiteral("ui/chart/interval"), selectedInterval_).toString();
        selectedDurationNs_ =
            settings.value(QStringLiteral("ui/chart/durationNs"), selectedDurationNs_).toLongLong();
        if (selectedSymbol_.isEmpty()) {
            selectedSymbol_ = watchlist_.value(0, QStringLiteral("SHFE.rb2701"));
        }
        if (selectedDurationNs_ <= 0) {
            selectedDurationNs_ = 86'400'000'000'000LL;
            selectedInterval_   = QStringLiteral("1d");
        }
    }

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
    watchModel_->setSymbols(watchlist_);

    buildUi();
    applyTheme();
    connectNode();

    const int selectedRow = watchlist_.indexOf(selectedSymbol_);
    if (selectedRow >= 0) {
        watchTable_->selectRow(selectedRow);
    }
    selectSymbol(selectedSymbol_);
}

// 构建顺序很重要：先建 dock，再建 view menu，最后恢复 QMainWindow state。
// QDockWidget 必须在 restoreState() 之前存在，否则保存的尺寸/浮动状态无法匹配。
void MainWindow::buildUi() {
    buildToolbar();
    buildWorkspace();
    buildBottomDock();
    buildViewMenu();
    restoreWorkspace();
    statusBar()->showMessage(tr("Qt UI Phase 2 · 等待 node"));
}

void MainWindow::buildToolbar() {
    auto* toolbar = addToolBar(tr("主工具栏"));
    toolbar->setObjectName(QStringLiteral("mainToolBar"));
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

    componentsButton_ = new QPushButton(tr("组件"), toolbar);
    componentsButton_->setObjectName("componentsButton");
    toolbar->addWidget(componentsButton_);

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

QDockWidget* MainWindow::addModuleDock(const QString&     title,
                                       QWidget*           content,
                                       Qt::DockWidgetArea area,
                                       const QString&     objectName) {
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setWidget(content);
    dock->installEventFilter(this);
    addDockWidget(area, dock);
    return dock;
}

void MainWindow::buildViewMenu() {
    auto*      viewMenu  = menuBar()->addMenu(tr("视图 / 组件"));
    const auto addToggle = [viewMenu](QDockWidget* dock) {
        if (dock == nullptr) {
            return;
        }
        QAction* action = dock->toggleViewAction();
        action->setText(dock->windowTitle());
        viewMenu->addAction(action);
    };
    addToggle(watchDock_);
    addToggle(futuresDock_);
    addToggle(quoteDock_);
    addToggle(orderBookDock_);
    addToggle(tapeDock_);
    addToggle(positionsDock_);
    addToggle(ordersDock_);
    addToggle(tradesDock_);
    addToggle(accountDock_);
    addToggle(detailDock_);
    addToggle(optionsDock_);
    addToggle(orderDock_);
    viewMenu->addSeparator();
    QAction* chartPopout = viewMenu->addAction(tr("图表弹出 / 收回"));
    connect(chartPopout, &QAction::triggered, this, [this] { toggleChartPopout(); });
    QAction* reset = viewMenu->addAction(tr("恢复默认布局"));
    connect(reset, &QAction::triggered, this, [this] { resetWorkspace(); });
    componentsButton_->setMenu(viewMenu);
    for (QDockWidget* dock : {watchDock_,
                              futuresDock_,
                              quoteDock_,
                              orderBookDock_,
                              tapeDock_,
                              positionsDock_,
                              ordersDock_,
                              tradesDock_,
                              accountDock_,
                              detailDock_,
                              optionsDock_,
                              orderDock_}) {
        if (dock == nullptr) {
            continue;
        }
        connect(dock, &QDockWidget::dockLocationChanged, this, [this] { scheduleWorkspaceSave(); });
        connect(dock, &QDockWidget::topLevelChanged, this, [this] { scheduleWorkspaceSave(); });
        connect(dock, &QDockWidget::visibilityChanged, this, [this] { scheduleWorkspaceSave(); });
    }
}

void MainWindow::resetWorkspace() {
    QSettings settings;
    settings.remove(QStringLiteral("ui/workspaceState"));
    for (QDockWidget* dock : {watchDock_,
                              quoteDock_,
                              orderBookDock_,
                              tapeDock_,
                              positionsDock_,
                              ordersDock_,
                              tradesDock_,
                              accountDock_,
                              detailDock_,
                              orderDock_}) {
        if (dock != nullptr) {
            dock->setFloating(false);
            dock->show();
        }
    }
    if (positionsDock_ != nullptr) {
        positionsDock_->raise();
    }
    if (futuresDock_ != nullptr) {
        futuresDock_->hide();
    }
    if (optionsDock_ != nullptr) {
        optionsDock_->hide();
    }
    if (chartDialog_ != nullptr) {
        chartDialog_->close();
    }
    setCentralWidget(chartHost_);
    resizeDocks({watchDock_}, {360}, Qt::Horizontal);
    resizeDocks({quoteDock_}, {300}, Qt::Horizontal);
    resizeDocks({quoteDock_, orderBookDock_, tapeDock_}, {220, 260, 180}, Qt::Vertical);
    resizeDocks({positionsDock_, orderDock_}, {1000, 300}, Qt::Horizontal);
}

void MainWindow::restoreWorkspace() {
    QSettings        settings;
    bool             restored = false;
    const int        version  = settings.value(QStringLiteral("ui/workspaceVersion"), 0).toInt();
    const QByteArray state    = settings.value(QStringLiteral("ui/workspaceState")).toByteArray();
    if (version == kWorkspaceVersion && !state.isEmpty()) {
        restored = restoreState(state);
    } else {
        settings.remove(QStringLiteral("ui/workspaceState"));
    }
    const QByteArray geometry = settings.value(QStringLiteral("ui/windowGeometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    if (!restored) {
        QTimer::singleShot(0, this, [this] {
            resizeDocks({watchDock_}, {360}, Qt::Horizontal);
            resizeDocks({quoteDock_}, {300}, Qt::Horizontal);
            resizeDocks({quoteDock_, orderBookDock_, tapeDock_}, {220, 260, 180}, Qt::Vertical);
            resizeDocks({positionsDock_, orderDock_}, {1000, 300}, Qt::Horizontal);
        });
    }
}

// 持久化分两类：
// - saveState()/saveGeometry() 保存 QMainWindow 和 dock 的几何状态；
// - ui/chart/* 保存中央图表自身的合约、周期和视口。
// 表格列状态由 TablePanel 自己按 ui/columns/* 保存。
void MainWindow::saveWorkspace() {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/workspaceVersion"), kWorkspaceVersion);
    settings.setValue(QStringLiteral("ui/workspaceState"), saveState());
    settings.setValue(QStringLiteral("ui/windowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/chart/symbol"), selectedSymbol_);
    settings.setValue(QStringLiteral("ui/chart/interval"), selectedInterval_);
    settings.setValue(QStringLiteral("ui/chart/durationNs"), selectedDurationNs_);
    if (chart_ != nullptr) {
        settings.setValue(QStringLiteral("ui/chart/visibleCount"), chart_->visibleCount());
        settings.setValue(QStringLiteral("ui/chart/visibleStart"), chart_->visibleStartIndex());
        settings.setValue(QStringLiteral("ui/chart/followLatest"), chart_->followLatest());
    }
}

void MainWindow::scheduleWorkspaceSave() {
    if (workspaceSaveTimer_ != nullptr) {
        workspaceSaveTimer_->start();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        scheduleWorkspaceSave();
    }
    if (qobject_cast<QDockWidget*>(watched) != nullptr && event->type() == QEvent::Resize) {
        scheduleWorkspaceSave();
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/watchlist"), watchlist_);
    saveWorkspace();
    QMainWindow::closeEvent(event);
}

void MainWindow::toggleChartPopout() {
    if (chartDialog_ != nullptr) {
        chartDialog_->close();
        return;
    }
    if (chart_ == nullptr || chartHost_ == nullptr) {
        return;
    }
    chartDialog_ = new QDialog(this);
    chartDialog_->setWindowTitle(tr("TQSDK 图表"));
    chartDialog_->resize(1200, 800);
    auto* layout = new QVBoxLayout(chartDialog_);
    layout->addWidget(chart_);
    chartDialog_->show();
    connect(chartDialog_, &QDialog::finished, this, [this] {
        if (chart_ != nullptr && chartHost_ != nullptr) {
            chartHost_->layout()->addWidget(chart_);
            chart_->show();
        }
        QDialog* dialog = chartDialog_;
        chartDialog_    = nullptr;
        if (dialog != nullptr) {
            dialog->deleteLater();
        }
    });
}

void MainWindow::buildWorkspace() {
    chartHost_        = new QWidget(this);
    auto* chartLayout = new QVBoxLayout(chartHost_);
    chartLayout->setContentsMargins(0, 0, 0, 0);
    chart_ = new KLineChartWidget(chartHost_);
    chart_->setTitle(selectedSymbol_, selectedInterval_);
    {
        QSettings settings;
        chart_->setViewport(settings.value(QStringLiteral("ui/chart/visibleCount"), 120).toInt(),
                            settings.value(QStringLiteral("ui/chart/visibleStart"), 0).toInt(),
                            settings.value(QStringLiteral("ui/chart/followLatest"), true).toBool());
    }
    connect(chart_, &KLineChartWidget::viewportChanged, this, [this] { scheduleWorkspaceSave(); });
    chartLayout->addWidget(chart_);
    setCentralWidget(chartHost_);

    auto* watchPanel = new TablePanel(watchModel_, this);
    watchPanel->installColumnMenu(QStringLiteral("watchlist"));
    watchTable_ = watchPanel->view();
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
    watchDock_ = addModuleDock(
        tr("自选列表"), watchPanel, Qt::LeftDockWidgetArea, QStringLiteral("watchlistDock"));

    auto* futuresPanel = new TablePanel(futuresProxy_, this);
    futuresPanel->installColumnMenu(QStringLiteral("futures"));
    futuresTable_ = futuresPanel->view();
    connect(futuresTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const auto    source = futuresProxy_->mapToSource(index);
        const QString symbol = futuresModel_->symbolAt(source.row());
        if (!symbol.isEmpty()) {
            addSymbolToWatchlist(symbol);
        }
    });
    futuresDock_ = addModuleDock(
        tr("期货列表"), futuresPanel, Qt::LeftDockWidgetArea, QStringLiteral("futuresDock"));
    tabifyDockWidget(watchDock_, futuresDock_);
    futuresDock_->hide();

    auto* optionsContainer = new QWidget(this);
    auto* optionsLayout    = new QHBoxLayout(optionsContainer);
    optionsLayout->setContentsMargins(4, 4, 4, 4);
    auto* callsPanel = new TablePanel(callsModel_, optionsContainer);
    callsPanel->installColumnMenu(QStringLiteral("option-calls"));
    callsTable_     = callsPanel->view();
    auto* putsPanel = new TablePanel(putsModel_, optionsContainer);
    putsPanel->installColumnMenu(QStringLiteral("option-puts"));
    putsTable_ = putsPanel->view();
    auto addOption =
        [this](QTableView* table, InstrumentTableModel* model, const QModelIndex& index) {
            const QString symbol = model->symbolAt(index.row());
            if (!symbol.isEmpty()) {
                addSymbolToWatchlist(symbol);
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
    optionsLayout->addWidget(callsPanel, 1);
    optionsLayout->addWidget(putsPanel, 1);
    optionsDock_ = addModuleDock(
        tr("期权链"), optionsContainer, Qt::RightDockWidgetArea, QStringLiteral("optionsDock"));

    connect(optionsDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            requestOptionChain();
        }
    });
    optionsDock_->hide();

    quotePanel_ = new QuoteSnapshotPanel(this);
    quotePanel_->setSymbol(selectedSymbol_);
    quoteDock_ = addModuleDock(
        tr("行情快照"), quotePanel_, Qt::RightDockWidgetArea, QStringLiteral("quoteDock"));

    orderBookPanel_ = new OrderBookPanel(this);
    orderBookPanel_->setSymbol(selectedSymbol_);
    orderBookDock_ = addModuleDock(
        tr("订单簿"), orderBookPanel_, Qt::RightDockWidgetArea, QStringLiteral("orderBookDock"));
    splitDockWidget(quoteDock_, orderBookDock_, Qt::Vertical);

    tradeTapePanel_ = new TradeTapePanel(tapeModel_, this);
    tapeDock_       = addModuleDock(
        tr("分笔成交"), tradeTapePanel_, Qt::RightDockWidgetArea, QStringLiteral("tapeDock"));
    splitDockWidget(orderBookDock_, tapeDock_, Qt::Vertical);
}

void MainWindow::buildBottomDock() {
    positionsPanel_   = new PositionsPanel(positionsModel_, this);
    ordersPanel_      = new OrdersPanel(ordersModel_, this);
    tradesPanel_      = new TradesPanel(tradesModel_, this);
    accountPanel_     = new AccountPanel(this);
    orderDetailPanel_ = new OrderDetailPanel(this);

    // 持仓/委托/成交/账户/明细：先建主区域，再 tabify，保持一个紧凑的
    // 交易表格 dock 组。三键下单单独 split 到右侧，作为独立下单面板。
    positionsDock_ = addModuleDock(
        tr("持仓"), positionsPanel_, Qt::BottomDockWidgetArea, QStringLiteral("positionsDock"));

    orderTicket_ = new OrderTicketPanel(this);
    orderTicket_->setSymbol(selectedSymbol_);
    connect(orderTicket_, &OrderTicketPanel::symbolSubmitted, this, [this](const QString& input) {
        const QString symbol = resolveSymbol(input);
        if (symbol.isEmpty()) {
            orderTicket_->setStatus(tr("没有匹配的交易合约"), true);
            return;
        }
        addSymbolToWatchlist(symbol);
        orderTicket_->setSymbol(selectedSymbol_);
    });
    connect(orderTicket_, &OrderTicketPanel::buyOpenRequested, this, [this] {
        submitOrder(Direction::Buy, Offset::Open);
    });
    connect(orderTicket_, &OrderTicketPanel::sellOpenRequested, this, [this] {
        submitOrder(Direction::Sell, Offset::Open);
    });
    connect(orderTicket_, &OrderTicketPanel::closeRequested, this, [this] {
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
            orderTicket_->setStatus(tr("当前合约无持仓"), true);
        }
    });

    orderDock_ = addModuleDock(
        tr("三键下单"), orderTicket_, Qt::BottomDockWidgetArea, QStringLiteral("orderDock"));
    splitDockWidget(positionsDock_, orderDock_, Qt::Horizontal);

    ordersDock_ = addModuleDock(
        tr("委托"), ordersPanel_, Qt::BottomDockWidgetArea, QStringLiteral("ordersDock"));
    tradesDock_ = addModuleDock(
        tr("成交"), tradesPanel_, Qt::BottomDockWidgetArea, QStringLiteral("tradesDock"));
    accountDock_ = addModuleDock(
        tr("账户"), accountPanel_, Qt::BottomDockWidgetArea, QStringLiteral("accountDock"));
    detailDock_ = addModuleDock(
        tr("明细"), orderDetailPanel_, Qt::BottomDockWidgetArea, QStringLiteral("detailDock"));
    tabifyDockWidget(positionsDock_, ordersDock_);
    tabifyDockWidget(ordersDock_, tradesDock_);
    tabifyDockWidget(tradesDock_, accountDock_);
    tabifyDockWidget(accountDock_, detailDock_);
    positionsDock_->raise();

    connect(
        positionsPanel_, &PositionsPanel::positionActivated, this, [this](const QString& symbol) {
            if (!symbol.isEmpty() && symbol != selectedSymbol_) {
                selectSymbol(symbol);
            }
        });
    connect(ordersPanel_, &OrdersPanel::selectionChanged, this, [this](const QString&) {
        updateOrderControls();
        updateOrderDetail();
    });
    connect(ordersPanel_, &OrdersPanel::cancelRequested, this, [this] { cancelSelectedOrder(); });

    const auto refreshCachedQuotes = [this] {
        if (latestQuotes_.isEmpty()) {
            return;
        }
        const auto quotes = latestQuotes_.values();
        if (futuresDock_ != nullptr && futuresDock_->isVisible()) {
            futuresModel_->applyQuotes(quotes);
        }
        if (optionsDock_ != nullptr && optionsDock_->isVisible()) {
            callsModel_->applyQuotes(quotes);
            putsModel_->applyQuotes(quotes);
        }
    };
    if (futuresDock_ != nullptr) {
        connect(futuresDock_, &QDockWidget::visibilityChanged, this, [refreshCachedQuotes](bool) {
            refreshCachedQuotes();
        });
    }
    if (optionsDock_ != nullptr) {
        connect(optionsDock_, &QDockWidget::visibilityChanged, this, [refreshCachedQuotes](bool) {
            refreshCachedQuotes();
        });
    }
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
                    requestSelectedKlineHistory();
                    if (optionsDock_ != nullptr && optionsDock_->isVisible()) {
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
        if (orderTicket_ != nullptr) {
            orderTicket_->setStatus(error, true);
        }
    });

    // quote/kline 已由 worker 解析并合帧；这里只做 UI 状态应用。
    // futures/options 不可见时只更新缓存，避免隐藏的大表参与每帧刷新。
    connect(nodeClient_, &NodeClient::quotesUpdated, this, [this](const QVector<Quote>& quotes) {
        for (const auto& quote : quotes) {
            if (quote.instrumentId.isEmpty()) {
                continue;
            }
            auto it = latestQuotes_.find(quote.instrumentId);
            if (it == latestQuotes_.end()) {
                latestQuotes_.insert(quote.instrumentId, quote);
            } else {
                *it = mergeQuote(*it, quote);
            }
        }
        marketStore_->applyQuotes(quotes);
        watchModel_->applyQuotes(quotes);
        if (futuresDock_ != nullptr && futuresDock_->isVisible()) {
            futuresModel_->applyQuotes(quotes);
        }
        if (optionsDock_ != nullptr && optionsDock_->isVisible()) {
            callsModel_->applyQuotes(quotes);
            putsModel_->applyQuotes(quotes);
        }
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
                if (orderTicket_ != nullptr) {
                    QStringList suggestions;
                    suggestions.reserve(instruments.size());
                    for (const auto& instrument : instruments) {
                        suggestions.push_back(instrument.instrumentId + QStringLiteral("  ") +
                                              instrument.name);
                    }
                    orderTicket_->setCompletions(suggestions);
                }
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
        updateOrderDetail();
    });
    connect(nodeClient_, &NodeClient::tradesUpdated, this, [this](const QVector<Trade>& trades) {
        tradesModel_->applyTrades(trades);
    });
    connect(nodeClient_, &NodeClient::orderAckReceived, this, [this](const OrderAck& ack) {
        if (orderTicket_ != nullptr) {
            orderTicket_->setStatus(ack.success ? tr("命令已接受: %1").arg(ack.orderId)
                                                : tr("下单失败: %1").arg(ack.error),
                                    !ack.success);
        }
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
        QSettings settings;
        settings.setValue(QStringLiteral("ui/watchlist"), watchlist_);
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
    if (orderTicket_ != nullptr) {
        orderTicket_->setSymbol(symbol);
    }
    if (tapeModel_ != nullptr) {
        tapeModel_->clear();
    }
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

    if (quotePanel_ != nullptr) {
        quotePanel_->setSymbol(symbol);
    }
    if (orderBookPanel_ != nullptr) {
        orderBookPanel_->setSymbol(symbol);
    }
    chart_->setTitle(symbol, selectedInterval_);
    chart_->setBars(marketStore_->bars(symbol, selectedDurationNs_));
    updateQuotePanel();
    scheduleWorkspaceSave();

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
    requestSelectedKlineHistory();
    if (optionsDock_ != nullptr && optionsDock_->isVisible()) {
        requestOptionChain();
    }
}

void MainWindow::requestSelectedKlineHistory() {
    if (nodeClient_ == nullptr || selectedSymbol_.isEmpty() ||
        nodeClient_->state() == NodeClient::State::Disconnected) {
        return;
    }
    const QString symbol   = selectedSymbol_;
    const qint64  duration = selectedDurationNs_;
    nodeClient_->requestKlineHistory(symbol, duration, 200);
    for (const int delay : {1200, 4000}) {
        QTimer::singleShot(delay, this, [this, symbol, duration] {
            if (selectedSymbol_ != symbol || selectedDurationNs_ != duration ||
                nodeClient_ == nullptr || nodeClient_->state() == NodeClient::State::Disconnected) {
                return;
            }
            if (marketStore_->bars(symbol, duration).size() < 80) {
                nodeClient_->requestKlineHistory(symbol, duration, 200);
            }
        });
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
            requestSelectedKlineHistory();
        }
    }
    updateQuotePanel();
    scheduleWorkspaceSave();
}

void MainWindow::updateQuotePanel() {
    if (quotePanel_ == nullptr) {
        return;
    }
    const Quote*      quote      = marketStore_->quote(selectedSymbol_);
    const Instrument* instrument = findInstrument(selectedSymbol_);

    quotePanel_->setSymbol(selectedSymbol_);
    quotePanel_->setQuote(quote, instrument != nullptr ? instrument->name : QString{});
    if (orderBookPanel_ != nullptr) {
        orderBookPanel_->setSymbol(selectedSymbol_);
        orderBookPanel_->setQuote(quote);
    }
    chart_->setQuote(quote != nullptr ? *quote : Quote{});
    if (quote != nullptr && tapeModel_ != nullptr) {
        tapeModel_->applyQuote(*quote);
        if (orderTicket_ != nullptr) {
            orderTicket_->setPriceIfEmpty(quote->lastPrice);
        }
    }
    updateOrderControls();
}

void MainWindow::updateAccountPanel(const Account& account) {
    accountLoaded_ = true;
    if (accountPanel_ != nullptr) {
        accountPanel_->setAccount(account);
    }
    updateOrderControls();
}

void MainWindow::updateOrderControls() {
    if (orderTicket_ == nullptr) {
        return;
    }
    const bool canSubmit = nodeReady_ && accountLoaded_;
    orderTicket_->setTradingEnabled(canSubmit);

    qint64 longVolume  = 0;
    qint64 shortVolume = 0;
    for (const auto& position : positionsModel_->positions()) {
        if (position.instrumentId == selectedSymbol_) {
            longVolume += position.volumeLong;
            shortVolume += position.volumeShort;
        }
    }
    orderTicket_->setCloseEnabled(canSubmit && (longVolume > 0 || shortVolume > 0));

    if (ordersPanel_ != nullptr) {
        const auto current = ordersPanel_->view()->currentIndex();
        ordersPanel_->setCancelEnabled(current.isValid() &&
                                       ordersModel_->isOrderAlive(current.row()));
    }
}

void MainWindow::updateOrderDetail() {
    if (orderDetailPanel_ == nullptr || ordersPanel_ == nullptr) {
        return;
    }
    const auto current = ordersPanel_->view()->currentIndex();
    if (!current.isValid() || current.row() >= ordersModel_->orders().size()) {
        orderDetailPanel_->clear();
        return;
    }
    orderDetailPanel_->setOrder(&ordersModel_->orders().at(current.row()));
}

void MainWindow::submitOrder(Direction direction, Offset offset) {
    if (orderTicket_ == nullptr) {
        return;
    }
    if (nodeClient_ == nullptr || !nodeReady_ || !accountLoaded_) {
        orderTicket_->setStatus(tr("交易未就绪"), true);
        return;
    }
    if (orderTicket_->price() <= 0.0 || orderTicket_->volume() <= 0) {
        orderTicket_->setStatus(tr("价格/数量无效"), true);
        return;
    }

    OrderRequest request;
    request.instrumentId = selectedSymbol_;
    request.direction    = direction;
    request.offset       = offset;
    request.priceType    = PriceType::Limit;
    request.limitPrice   = orderTicket_->price();
    request.volume       = orderTicket_->volume();
    nodeClient_->placeOrder(request);
    orderTicket_->setStatus(tr("已提交 %1 %2 手 @ %3")
                                .arg(selectedSymbol_)
                                .arg(request.volume)
                                .arg(priceText(request.limitPrice)));
    updateOrderControls();
}

void MainWindow::cancelSelectedOrder() {
    if (nodeClient_ == nullptr || ordersPanel_ == nullptr) {
        return;
    }
    const auto current = ordersPanel_->view()->currentIndex();
    if (!current.isValid() || !ordersModel_->isOrderAlive(current.row())) {
        return;
    }
    const QString orderId = ordersModel_->orderIdAt(current.row());
    if (orderId.isEmpty()) {
        return;
    }
    nodeClient_->cancelOrder(orderId);
    if (orderTicket_ != nullptr) {
        orderTicket_->setStatus(tr("撤单请求: %1").arg(orderId));
    }
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
