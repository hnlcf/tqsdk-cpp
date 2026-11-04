#pragma once

#include "net/Protocol.h"

#include <QMainWindow>

class QCloseEvent;
class QTimer;
class QLabel;
class QSortFilterProxyModel;

namespace tqsdk::ui {

class InstrumentTableModel;
class MarketStore;
class MarketToolBar;
class NodeClient;
class NodeDataController;
class OrderTableModel;
class PositionTableModel;
class QuoteTableModel;
class SymbolController;
class TradeTableModel;
class TradeTapeModel;
class TradingController;
class Toast;
class WorkspaceLayout;

/// 主窗口 = 工作区根对象 + controller 注入点。
///
/// 具体面板构建、dock 布局、图表 popout 移入 WorkspaceLayout；
/// 合约/订阅在 SymbolController，交易在 TradingController，
/// Node 数据应用在 NodeDataController。MainWindow 只做连接和 UI 状态转发。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(NodeClient* nodeClient, QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    void buildUi();
    void buildToolbar();
    void buildViewMenu();
    void buildTradingController();
    void connectNode();

    void addSymbolToWatchlist(const QString& symbol, bool request = true);
    void addSymbolFromInput(const QString& input);
    void updateSearchCompletions(const QString& input);
    void scheduleSymbolSelection(const QString& symbol);
    void applySelectedSymbol(const QString& symbol);
    void applyInterval(qint64 durationNs, const QString& label);
    void queueChartBars(const QString& symbol, qint64 durationNs, const QVector<KlineBar>& bars);
    void flushChartBars();

    void updateQuotePanel();
    void updateAccountPanel(const Account& account);
    void updateOrderControls();
    void updatePositionDetail();
    void applyTheme();

    void closeEvent(QCloseEvent* event) override;

    NodeClient*            nodeClient_     = nullptr;
    MarketStore*           marketStore_    = nullptr;
    QuoteTableModel*       watchModel_     = nullptr;
    InstrumentTableModel*  futuresModel_   = nullptr;
    QSortFilterProxyModel* futuresProxy_   = nullptr;
    InstrumentTableModel*  callsModel_     = nullptr;
    InstrumentTableModel*  putsModel_      = nullptr;
    PositionTableModel*    positionsModel_ = nullptr;
    OrderTableModel*       ordersModel_    = nullptr;
    TradeTableModel*       tradesModel_    = nullptr;
    TradeTapeModel*        tapeModel_      = nullptr;

    MarketToolBar*      marketToolBar_      = nullptr;
    WorkspaceLayout*    workspace_          = nullptr;
    SymbolController*   symbolController_   = nullptr;
    NodeDataController* nodeDataController_ = nullptr;
    TradingController*  tradingController_  = nullptr;
    Toast*              toast_              = nullptr;

    QTimer*           chartRefreshTimer_ = nullptr;
    QTimer*           selectionTimer_    = nullptr;
    QString           pendingSelection_;
    QVector<KlineBar> pendingChartBars_;
    QString           pendingChartSymbol_;
    qint64            pendingChartDuration_ = 0;
    int               lastNodeState_        = -1;

    bool dark_ = true;
};

} // namespace tqsdk::ui
