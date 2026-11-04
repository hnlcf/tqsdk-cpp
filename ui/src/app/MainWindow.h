#pragma once

#include "net/Protocol.h"

#include <QHash>
#include <QMainWindow>
#include <QStringList>

class QCompleter;
class QCloseEvent;
class QDockWidget;
class QDialog;
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QStringListModel;
class QTableView;
class QTimer;

namespace tqsdk::ui {

class AccountPanel;
class InstrumentTableModel;
class OrderBookPanel;
class OrderDetailPanel;
class OrderTicketPanel;
class OrdersPanel;
class PositionsPanel;
class QuoteSnapshotPanel;
class TradeTapePanel;
class TradesPanel;
class KLineChartWidget;
class MarketStore;
class NodeClient;
class OrderTableModel;
class PositionTableModel;
class QuoteTableModel;
class TradeTableModel;
class TradeTapeModel;

/// 主窗口是 UI 工作区的编排器，而不是数据源。
///
/// 职责：
/// - 组装 QDockWidget 工作区和工具栏；
/// - 连接 NodeClient 的强类型信号到模型/图表；
/// - 处理选中合约、交易入口和布局持久化。
///
/// 线程约束：本类所有函数都运行在 GUI 线程；网络 JSON 解析由
/// HttpWsNodeClient 内部的 JsonParserWorker 线程完成。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(NodeClient* nodeClient, QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    // ---- UI 构建与工作区 ----
    void         buildUi();
    void         buildToolbar();
    void         buildWorkspace();
    void         buildBottomDock();
    void         buildViewMenu();
    QDockWidget* addModuleDock(const QString&     title,
                               QWidget*           content,
                               Qt::DockWidgetArea area,
                               const QString&     objectName);
    // ---- 布局/图表状态持久化 ----
    void resetWorkspace();
    void restoreWorkspace();
    void saveWorkspace();
    void scheduleWorkspaceSave();
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void toggleChartPopout();
    // ---- 数据/交易事件 ----
    void              connectNode();
    void              selectSymbol(const QString& symbol);
    void              addSymbolToWatchlist(const QString& symbol, bool request = true);
    void              addSymbolFromInput(const QString& input);
    QString           resolveSymbol(const QString& input) const;
    void              updateSearchCompletions(const QString& input);
    void              setInterval(qint64 durationNs, const QString& label);
    void              requestSelectedKlineHistory();
    void              updateQuotePanel();
    void              updateAccountPanel(const Account& account);
    void              updateOrderControls();
    void              updateOrderDetail();
    void              submitOrder(Direction direction, Offset offset);
    void              cancelSelectedOrder();
    const Instrument* findInstrument(const QString& symbol) const;
    void              requestOptionChain();
    void              applyTheme();

    // 网络抽象与 UI 侧数据缓存；不包含后端 core 对象。
    NodeClient*            nodeClient_         = nullptr;
    MarketStore*           marketStore_        = nullptr;
    QuoteTableModel*       watchModel_         = nullptr;
    InstrumentTableModel*  futuresModel_       = nullptr;
    QSortFilterProxyModel* futuresProxy_       = nullptr;
    InstrumentTableModel*  callsModel_         = nullptr;
    InstrumentTableModel*  putsModel_          = nullptr;
    PositionTableModel*    positionsModel_     = nullptr;
    OrderTableModel*       ordersModel_        = nullptr;
    TradeTableModel*       tradesModel_        = nullptr;
    TradeTapeModel*        tapeModel_          = nullptr;
    KLineChartWidget*      chart_              = nullptr;
    QWidget*               chartHost_          = nullptr;
    QDialog*               chartDialog_        = nullptr;
    QTimer*                workspaceSaveTimer_ = nullptr;
    QHash<QString, Quote>  latestQuotes_;

    QTableView*         watchTable_        = nullptr;
    QTableView*         futuresTable_      = nullptr;
    QTableView*         callsTable_        = nullptr;
    QTableView*         putsTable_         = nullptr;
    QLineEdit*          search_            = nullptr;
    QCompleter*         searchCompleter_   = nullptr;
    QStringListModel*   searchCompletions_ = nullptr;
    QPushButton*        addSymbolButton_   = nullptr;
    QPushButton*        componentsButton_  = nullptr;
    QuoteSnapshotPanel* quotePanel_        = nullptr;
    OrderBookPanel*     orderBookPanel_    = nullptr;
    TradeTapePanel*     tradeTapePanel_    = nullptr;
    OrderTicketPanel*   orderTicket_       = nullptr;
    PositionsPanel*     positionsPanel_    = nullptr;
    OrdersPanel*        ordersPanel_       = nullptr;
    TradesPanel*        tradesPanel_       = nullptr;
    AccountPanel*       accountPanel_      = nullptr;
    OrderDetailPanel*   orderDetailPanel_  = nullptr;

    QDockWidget* watchDock_     = nullptr;
    QDockWidget* quoteDock_     = nullptr;
    QDockWidget* orderBookDock_ = nullptr;
    QDockWidget* tapeDock_      = nullptr;
    QDockWidget* positionsDock_ = nullptr;
    QDockWidget* ordersDock_    = nullptr;
    QDockWidget* tradesDock_    = nullptr;
    QDockWidget* accountDock_   = nullptr;
    QDockWidget* detailDock_    = nullptr;
    QDockWidget* orderDock_     = nullptr;
    QDockWidget* futuresDock_   = nullptr;
    QDockWidget* optionsDock_   = nullptr;

    QLabel*      connectionLabel_ = nullptr;
    QLabel*      futuresStatus_   = nullptr;
    QPushButton* themeButton_     = nullptr;

    QStringList watchlist_;
    QString     selectedSymbol_     = QStringLiteral("SHFE.rb2701");
    qint64      selectedDurationNs_ = 86'400'000'000'000LL;
    QString     selectedInterval_   = QStringLiteral("1d");
    bool        dark_               = false;
    bool        nodeReady_          = false;
    bool        accountLoaded_      = false;
};

} // namespace tqsdk::ui
