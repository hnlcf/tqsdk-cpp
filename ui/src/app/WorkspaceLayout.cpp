#include "app/WorkspaceLayout.h"

#include "chart/KLineChartWidget.h"
#include "model/InstrumentTableModel.h"
#include "model/QuoteTableModel.h"
#include "model/TradingModels.h"
#include "widgets/OrderBookPanel.h"
#include "widgets/OrderTicketPanel.h"
#include "widgets/QuoteSnapshotPanel.h"
#include "widgets/TablePanel.h"
#include "widgets/TradeTapePanel.h"
#include "widgets/TradingPanels.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QDialog>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QMainWindow>
#include <QMenu>
#include <QSettings>
#include <QShortcut>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace tqsdk::ui {
namespace {
constexpr int kWorkspaceVersion = 5;

QString symbolChartPrefix(const QString& symbol) {
    return QStringLiteral("ui/chart/symbols/%1/")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(symbol)));
}

} // namespace

WorkspaceLayout::WorkspaceLayout(QMainWindow*          window,
                                 QuoteTableModel*      watchModel,
                                 InstrumentTableModel* futuresModel,
                                 QAbstractItemModel*   futuresTableModel,
                                 InstrumentTableModel* callsModel,
                                 InstrumentTableModel* putsModel,
                                 PositionTableModel*   positionsModel,
                                 OrderTableModel*      ordersModel,
                                 TradeTableModel*      tradesModel,
                                 TradeTapeModel*       tapeModel,
                                 QObject*              parent)
    : QObject(parent),
      window_(window),
      watchModel_(watchModel),
      futuresModel_(futuresModel),
      futuresTableModel_(futuresTableModel),
      callsModel_(callsModel),
      putsModel_(putsModel),
      positionsModel_(positionsModel),
      ordersModel_(ordersModel),
      tradesModel_(tradesModel),
      tapeModel_(tapeModel) {
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(250);
    connect(saveTimer_, &QTimer::timeout, this, &WorkspaceLayout::saveLayout);
    build();
    if (window_ != nullptr) {
        window_->installEventFilter(this);
    }
}

QDockWidget* WorkspaceLayout::addDock(const QString&     title,
                                      QWidget*           content,
                                      Qt::DockWidgetArea area,
                                      const QString&     objectName) {
    auto* dock = new QDockWidget(title, window_);
    dock->setObjectName(objectName);
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setWidget(content);
    dock->installEventFilter(this);
    window_->addDockWidget(area, dock);
    connect(dock, &QDockWidget::dockLocationChanged, this, &WorkspaceLayout::scheduleSave);
    connect(dock, &QDockWidget::topLevelChanged, this, &WorkspaceLayout::scheduleSave);
    connect(dock, &QDockWidget::visibilityChanged, this, &WorkspaceLayout::scheduleSave);
    return dock;
}

void WorkspaceLayout::build() {
    chartHost_        = new QWidget(window_);
    auto* chartLayout = new QVBoxLayout(chartHost_);
    chartLayout->setContentsMargins(0, 0, 0, 0);
    chart_ = new KLineChartWidget(chartHost_);
    chartLayout->addWidget(chart_);
    window_->setCentralWidget(chartHost_);
    connect(chart_, &KLineChartWidget::viewportChanged, this, [this] {
        emit chartViewportChanged();
        scheduleSave();
    });

    auto* watchPanel = new TablePanel(watchModel_, window_);
    watchPanel->installColumnMenu(QStringLiteral("watchlist"));
    watchTable_ = watchPanel->view();
    watchTable_->setObjectName(QStringLiteral("watchTable"));
    watchPanel->setRowHeight(30);
    watchTable_->setDragEnabled(true);
    watchTable_->setAcceptDrops(true);
    watchTable_->setDropIndicatorShown(true);
    watchTable_->setDragDropMode(QAbstractItemView::InternalMove);
    watchTable_->setDefaultDropAction(Qt::MoveAction);
    connect(watchTable_->selectionModel(),
            &QItemSelectionModel::currentRowChanged,
            this,
            [this](const QModelIndex& current) {
                if (!current.isValid()) {
                    return;
                }
                const QString symbol = watchModel_->symbolAt(current.row());
                if (!symbol.isEmpty()) {
                    emit watchSymbolActivated(symbol);
                }
            });
    connect(watchTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const QString symbol = watchModel_->symbolAt(index.row());
        if (!symbol.isEmpty()) {
            emit watchSymbolActivated(symbol);
        }
    });
    watchTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(watchTable_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = watchTable_->indexAt(pos);
        if (!index.isValid()) {
            return;
        }
        const QString symbol = watchModel_->symbolAt(index.row());
        if (symbol.isEmpty()) {
            return;
        }
        QMenu    menu(watchTable_);
        QAction* remove = menu.addAction(tr("从自选移除"));
        menu.addSeparator();
        QAction* up     = menu.addAction(tr("上移"));
        QAction* down   = menu.addAction(tr("下移"));
        QAction* chosen = menu.exec(watchTable_->viewport()->mapToGlobal(pos));
        if (chosen == remove) {
            emit watchRemoveRequested(symbol);
        } else if (chosen == up) {
            emit watchMoveRequested(symbol, -1);
        } else if (chosen == down) {
            emit watchMoveRequested(symbol, 1);
        }
    });
    auto* removeShortcut = new QShortcut(QKeySequence::Delete, watchTable_);
    connect(removeShortcut, &QShortcut::activated, this, [this] {
        const QModelIndex current = watchTable_->currentIndex();
        if (!current.isValid()) {
            return;
        }
        const QString symbol = watchModel_->symbolAt(current.row());
        if (!symbol.isEmpty()) {
            emit watchRemoveRequested(symbol);
        }
    });
    watchDock_ = addDock(
        tr("自选列表"), watchPanel, Qt::LeftDockWidgetArea, QStringLiteral("watchlistDock"));

    auto* futuresPanel = new TablePanel(futuresTableModel_, window_);
    futuresPanel->installColumnMenu(QStringLiteral("futures"));
    futuresTable_ = futuresPanel->view();
    connect(futuresTable_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        const QString symbol =
            futuresTableModel_->data(futuresTableModel_->index(index.row(), 0)).toString();
        if (!symbol.isEmpty()) {
            emit futuresSymbolActivated(symbol);
        }
    });
    futuresDock_ = addDock(
        tr("期货列表"), futuresPanel, Qt::LeftDockWidgetArea, QStringLiteral("futuresDock"));
    window_->tabifyDockWidget(watchDock_, futuresDock_);
    futuresDock_->hide();

    auto* optionsContainer = new QWidget(window_);
    auto* optionsLayout    = new QHBoxLayout(optionsContainer);
    optionsLayout->setContentsMargins(4, 4, 4, 4);
    auto* callsPanel = new TablePanel(callsModel_, optionsContainer);
    callsPanel->installColumnMenu(QStringLiteral("option-calls"));
    callsTable_     = callsPanel->view();
    auto* putsPanel = new TablePanel(putsModel_, optionsContainer);
    putsPanel->installColumnMenu(QStringLiteral("option-puts"));
    putsTable_ = putsPanel->view();
    auto activateOption =
        [this](QTableView* table, InstrumentTableModel* model, const QModelIndex& index) {
            const QString symbol = model->symbolAt(index.row());
            if (!symbol.isEmpty()) {
                emit optionSymbolActivated(symbol);
            }
        };
    connect(callsTable_,
            &QTableView::doubleClicked,
            this,
            [this, activateOption](const QModelIndex& index) {
                activateOption(callsTable_, callsModel_, index);
            });
    connect(putsTable_,
            &QTableView::doubleClicked,
            this,
            [this, activateOption](const QModelIndex& index) {
                activateOption(putsTable_, putsModel_, index);
            });
    optionsLayout->addWidget(callsPanel, 1);
    optionsLayout->addWidget(putsPanel, 1);
    optionsDock_ = addDock(
        tr("期权链"), optionsContainer, Qt::RightDockWidgetArea, QStringLiteral("optionsDock"));
    connect(optionsDock_,
            &QDockWidget::visibilityChanged,
            this,
            &WorkspaceLayout::optionsVisibilityChanged);
    optionsDock_->hide();

    quotePanel_ = new QuoteSnapshotPanel(window_);
    quoteDock_ =
        addDock(tr("行情快照"), quotePanel_, Qt::RightDockWidgetArea, QStringLiteral("quoteDock"));
    orderBookPanel_ = new OrderBookPanel(window_);
    orderBookDock_  = addDock(
        tr("订单簿"), orderBookPanel_, Qt::RightDockWidgetArea, QStringLiteral("orderBookDock"));
    window_->splitDockWidget(quoteDock_, orderBookDock_, Qt::Vertical);
    tradeTapePanel_ = new TradeTapePanel(tapeModel_, window_);
    tapeDock_       = addDock(
        tr("分笔成交"), tradeTapePanel_, Qt::RightDockWidgetArea, QStringLiteral("tapeDock"));
    window_->splitDockWidget(orderBookDock_, tapeDock_, Qt::Vertical);

    positionsPanel_      = new PositionsPanel(positionsModel_, window_);
    ordersPanel_         = new OrdersPanel(ordersModel_, window_);
    tradesPanel_         = new TradesPanel(tradesModel_, window_);
    accountPanel_        = new AccountPanel(window_);
    positionDetailPanel_ = new PositionDetailPanel(window_);
    positionsDock_       = addDock(
        tr("持仓"), positionsPanel_, Qt::BottomDockWidgetArea, QStringLiteral("positionsDock"));
    orderTicket_ = new OrderTicketPanel(window_);
    connect(
        orderBookPanel_, &OrderBookPanel::priceClicked, orderTicket_, &OrderTicketPanel::setPrice);
    connect(
        quotePanel_, &QuoteSnapshotPanel::priceClicked, orderTicket_, &OrderTicketPanel::setPrice);
    orderDock_ = addDock(
        tr("三键下单"), orderTicket_, Qt::BottomDockWidgetArea, QStringLiteral("orderDock"));
    window_->splitDockWidget(positionsDock_, orderDock_, Qt::Horizontal);
    ordersDock_ =
        addDock(tr("委托"), ordersPanel_, Qt::BottomDockWidgetArea, QStringLiteral("ordersDock"));
    tradesDock_ =
        addDock(tr("成交"), tradesPanel_, Qt::BottomDockWidgetArea, QStringLiteral("tradesDock"));
    accountDock_ =
        addDock(tr("账户"), accountPanel_, Qt::BottomDockWidgetArea, QStringLiteral("accountDock"));
    detailDock_ = addDock(tr("持仓明细"),
                          positionDetailPanel_,
                          Qt::BottomDockWidgetArea,
                          QStringLiteral("detailDock"));
    window_->tabifyDockWidget(positionsDock_, ordersDock_);
    window_->tabifyDockWidget(ordersDock_, tradesDock_);
    window_->tabifyDockWidget(tradesDock_, accountDock_);
    window_->tabifyDockWidget(accountDock_, detailDock_);
    positionsDock_->raise();
    connect(positionsPanel_,
            &PositionsPanel::positionActivated,
            this,
            &WorkspaceLayout::positionActivated);
    connect(ordersPanel_, &OrdersPanel::selectionChanged, this, [this](const QString& id) {
        emit orderSelectionChanged();
        emit orderSelected(id);
    });
    connect(ordersPanel_, &OrdersPanel::cancelRequested, this, &WorkspaceLayout::cancelRequested);
    connect(futuresDock_,
            &QDockWidget::visibilityChanged,
            this,
            &WorkspaceLayout::futuresVisibilityChanged);
}

void WorkspaceLayout::scheduleSave() {
    if (saveTimer_ != nullptr) {
        saveTimer_->start();
    }
}

void WorkspaceLayout::saveLayout() {
    if (window_ == nullptr) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("ui/workspaceVersion"), kWorkspaceVersion);
    settings.setValue(QStringLiteral("ui/workspaceState"), window_->saveState());
    settings.setValue(QStringLiteral("ui/windowGeometry"), window_->saveGeometry());
    if (chart_ != nullptr) {
        settings.setValue(QStringLiteral("ui/chart/visibleCount"), chart_->visibleCount());
        settings.setValue(QStringLiteral("ui/chart/visibleStart"), chart_->visibleStartIndex());
        settings.setValue(QStringLiteral("ui/chart/followLatest"), chart_->followLatest());
        saveChartViewFor(chartSymbol_);
    }
}

void WorkspaceLayout::setChartSymbol(const QString& symbol) {
    if (symbol == chartSymbol_) {
        return;
    }
    if (!chartSymbol_.isEmpty()) {
        saveChartViewFor(chartSymbol_);
    }
    chartSymbol_ = symbol;
    if (!symbol.isEmpty()) {
        loadChartViewFor(symbol);
    }
}

void WorkspaceLayout::saveChartViewFor(const QString& symbol) const {
    if (symbol.isEmpty() || chart_ == nullptr) {
        return;
    }
    QSettings     settings;
    const QString prefix = symbolChartPrefix(symbol);
    settings.setValue(prefix + QStringLiteral("visibleCount"), chart_->visibleCount());
    settings.setValue(prefix + QStringLiteral("visibleStart"), chart_->visibleStartIndex());
    settings.setValue(prefix + QStringLiteral("followLatest"), chart_->followLatest());
}

void WorkspaceLayout::loadChartViewFor(const QString& symbol) {
    if (symbol.isEmpty() || chart_ == nullptr) {
        return;
    }
    QSettings     settings;
    const QString prefix = symbolChartPrefix(symbol);
    if (!settings.contains(prefix + QStringLiteral("visibleCount"))) {
        return;
    }
    chart_->setViewport(settings.value(prefix + QStringLiteral("visibleCount"), 120).toInt(),
                        settings.value(prefix + QStringLiteral("visibleStart"), 0).toInt(),
                        settings.value(prefix + QStringLiteral("followLatest"), true).toBool());
}

void WorkspaceLayout::restoreLayout() {
    if (window_ == nullptr || chart_ == nullptr) {
        return;
    }
    QSettings        settings;
    const int        version  = settings.value(QStringLiteral("ui/workspaceVersion"), 0).toInt();
    const QByteArray state    = settings.value(QStringLiteral("ui/workspaceState")).toByteArray();
    bool             restored = false;
    if (version == kWorkspaceVersion && !state.isEmpty()) {
        restored = window_->restoreState(state);
    }
    const QByteArray geometry = settings.value(QStringLiteral("ui/windowGeometry")).toByteArray();
    if (!geometry.isEmpty()) {
        window_->restoreGeometry(geometry);
    }
    chart_->setViewport(settings.value(QStringLiteral("ui/chart/visibleCount"), 120).toInt(),
                        settings.value(QStringLiteral("ui/chart/visibleStart"), 0).toInt(),
                        settings.value(QStringLiteral("ui/chart/followLatest"), true).toBool());
    if (!restored) {
        resetLayout();
    }
}

void WorkspaceLayout::resetLayout() {
    if (window_ == nullptr) {
        return;
    }
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
    if (futuresDock_ != nullptr) {
        futuresDock_->hide();
    }
    if (optionsDock_ != nullptr) {
        optionsDock_->hide();
    }
    if (chartDialog_ != nullptr) {
        chartDialog_->close();
    }
    if (chartHost_ != nullptr) {
        window_->setCentralWidget(chartHost_);
    }
    if (positionsDock_ != nullptr) {
        positionsDock_->raise();
    }
    window_->resizeDocks({watchDock_}, {360}, Qt::Horizontal);
    window_->resizeDocks({quoteDock_}, {300}, Qt::Horizontal);
    window_->resizeDocks({quoteDock_, orderBookDock_, tapeDock_}, {220, 260, 180}, Qt::Vertical);
    window_->resizeDocks({positionsDock_, orderDock_}, {1000, 300}, Qt::Horizontal);
}

void WorkspaceLayout::addDockActions(QMenu* menu) const {
    if (menu == nullptr) {
        return;
    }
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
        QAction* action = dock->toggleViewAction();
        action->setText(dock->windowTitle());
        menu->addAction(action);
    }
}

void WorkspaceLayout::toggleChartPopout() {
    if (chartDialog_ != nullptr) {
        chartDialog_->close();
        return;
    }
    if (chart_ == nullptr || chartHost_ == nullptr) {
        return;
    }
    chartDialog_ = new QDialog(window_);
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

bool WorkspaceLayout::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == window_ || qobject_cast<QDockWidget*>(watched) != nullptr) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        scheduleSave();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace tqsdk::ui
