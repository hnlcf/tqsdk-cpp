#pragma once

#include <QList>
#include <QObject>

class QAbstractItemModel;
class QDialog;
class QDockWidget;
class QMainWindow;
class QMenu;
class QTableView;
class QTimer;
class QWidget;

namespace tqsdk::ui {

class AccountPanel;
class InstrumentTableModel;
class KLineChartWidget;
class OrderBookPanel;
class OrderTableModel;
class OrderTicketPanel;
class OrdersPanel;
class PositionDetailPanel;
class PositionTableModel;
class PositionsPanel;
class QuoteSnapshotPanel;
class QuoteTableModel;
class TradeTapeModel;
class TradeTableModel;
class TradeTapePanel;
class TradesPanel;

/// 工作区布局和 dock 组装器。
///
/// MainWindow 不再逐个 new 面板/dock；这里负责组件拓扑、图表 popout、
/// dock 尺寸和 QSettings 布局恢复，并通过语义信号暴露用户选择动作。
class WorkspaceLayout final : public QObject {
    Q_OBJECT

public:
    WorkspaceLayout(QMainWindow*          window,
                    QuoteTableModel*      watchModel,
                    InstrumentTableModel* futuresModel,
                    QAbstractItemModel*   futuresTableModel,
                    InstrumentTableModel* callsModel,
                    InstrumentTableModel* putsModel,
                    PositionTableModel*   positionsModel,
                    OrderTableModel*      ordersModel,
                    TradeTableModel*      tradesModel,
                    TradeTapeModel*       tapeModel,
                    QObject*              parent = nullptr);

    void restoreLayout();
    void saveLayout();
    void scheduleSave();
    void resetLayout();
    void addDockActions(QMenu* menu) const;
    void toggleChartPopout();
    void setChartSymbol(const QString& symbol);

    KLineChartWidget*    chart() const { return chart_; }
    QWidget*             chartHost() const { return chartHost_; }
    QuoteSnapshotPanel*  quotePanel() const { return quotePanel_; }
    OrderBookPanel*      orderBookPanel() const { return orderBookPanel_; }
    TradeTapePanel*      tradeTapePanel() const { return tradeTapePanel_; }
    OrderTicketPanel*    orderTicket() const { return orderTicket_; }
    PositionsPanel*      positionsPanel() const { return positionsPanel_; }
    OrdersPanel*         ordersPanel() const { return ordersPanel_; }
    TradesPanel*         tradesPanel() const { return tradesPanel_; }
    AccountPanel*        accountPanel() const { return accountPanel_; }
    PositionDetailPanel* positionDetailPanel() const { return positionDetailPanel_; }

    QTableView* watchTable() const { return watchTable_; }
    QTableView* futuresTable() const { return futuresTable_; }
    QTableView* callsTable() const { return callsTable_; }
    QTableView* putsTable() const { return putsTable_; }

    QDockWidget* futuresDock() const { return futuresDock_; }
    QDockWidget* optionsDock() const { return optionsDock_; }

signals:
    void watchSymbolActivated(const QString& symbol);
    void watchRemoveRequested(const QString& symbol);
    void watchMoveRequested(const QString& symbol, int delta);
    void futuresSymbolActivated(const QString& symbol);
    void optionSymbolActivated(const QString& symbol);
    void positionActivated(const QString& symbol);
    void orderSelectionChanged();
    void orderSelected(const QString& orderId);
    void cancelRequested();
    void futuresVisibilityChanged(bool visible);
    void optionsVisibilityChanged(bool visible);
    void chartViewportChanged();

private:
    void         build();
    QDockWidget* addDock(const QString&     title,
                         QWidget*           content,
                         Qt::DockWidgetArea area,
                         const QString&     objectName);
    bool         eventFilter(QObject* watched, QEvent* event) override;

    QMainWindow*          window_            = nullptr;
    QuoteTableModel*      watchModel_        = nullptr;
    InstrumentTableModel* futuresModel_      = nullptr;
    QAbstractItemModel*   futuresTableModel_ = nullptr;
    InstrumentTableModel* callsModel_        = nullptr;
    InstrumentTableModel* putsModel_         = nullptr;
    PositionTableModel*   positionsModel_    = nullptr;
    OrderTableModel*      ordersModel_       = nullptr;
    TradeTableModel*      tradesModel_       = nullptr;
    TradeTapeModel*       tapeModel_         = nullptr;

    KLineChartWidget*    chart_               = nullptr;
    QWidget*             chartHost_           = nullptr;
    QDialog*             chartDialog_         = nullptr;
    QTableView*          watchTable_          = nullptr;
    QTableView*          futuresTable_        = nullptr;
    QTableView*          callsTable_          = nullptr;
    QTableView*          putsTable_           = nullptr;
    QuoteSnapshotPanel*  quotePanel_          = nullptr;
    OrderBookPanel*      orderBookPanel_      = nullptr;
    TradeTapePanel*      tradeTapePanel_      = nullptr;
    OrderTicketPanel*    orderTicket_         = nullptr;
    PositionsPanel*      positionsPanel_      = nullptr;
    OrdersPanel*         ordersPanel_         = nullptr;
    TradesPanel*         tradesPanel_         = nullptr;
    AccountPanel*        accountPanel_        = nullptr;
    PositionDetailPanel* positionDetailPanel_ = nullptr;

    QDockWidget* watchDock_     = nullptr;
    QDockWidget* futuresDock_   = nullptr;
    QDockWidget* optionsDock_   = nullptr;
    QDockWidget* quoteDock_     = nullptr;
    QDockWidget* orderBookDock_ = nullptr;
    QDockWidget* tapeDock_      = nullptr;
    QDockWidget* positionsDock_ = nullptr;
    QDockWidget* ordersDock_    = nullptr;
    QDockWidget* tradesDock_    = nullptr;
    QDockWidget* accountDock_   = nullptr;
    QDockWidget* detailDock_    = nullptr;
    QDockWidget* orderDock_     = nullptr;

    QTimer* saveTimer_ = nullptr;
    QString chartSymbol_;

    void saveChartViewFor(const QString& symbol) const;
    void loadChartViewFor(const QString& symbol);
};

} // namespace tqsdk::ui
