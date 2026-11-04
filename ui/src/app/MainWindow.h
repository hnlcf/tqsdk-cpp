#pragma once

#include "net/Protocol.h"

#include <QHash>
#include <QMainWindow>
#include <QStringList>

class QCompleter;
class QCloseEvent;
class QDockWidget;
class QDialog;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QSpinBox;
class QStringListModel;
class QTabWidget;
class QTableView;
class QTimer;

namespace tqsdk::ui {

class InstrumentTableModel;
class KLineChartWidget;
class MarketStore;
class NodeClient;
class OrderTableModel;
class PositionTableModel;
class QuoteTableModel;
class TradeTableModel;
class TradeTapeModel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(NodeClient* nodeClient, QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    void              buildUi();
    void              buildToolbar();
    void              buildWorkspace();
    void              buildOrderPanel(QWidget* parent);
    void              buildBottomDock();
    void              buildViewMenu();
    QDockWidget*      addModuleDock(const QString&     title,
                                    QWidget*           content,
                                    Qt::DockWidgetArea area,
                                    const QString&     objectName);
    void              resetWorkspace();
    void              restoreWorkspace();
    void              saveWorkspace();
    void              scheduleWorkspaceSave();
    void              closeEvent(QCloseEvent* event) override;
    bool              eventFilter(QObject* watched, QEvent* event) override;
    void              toggleChartPopout();
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
    void              submitOrder(Direction direction, Offset offset);
    void              cancelSelectedOrder();
    const Instrument* findInstrument(const QString& symbol) const;
    void              requestOptionChain();
    void              applyTheme();

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

    QTableView*       watchTable_        = nullptr;
    QTableView*       futuresTable_      = nullptr;
    QTableView*       callsTable_        = nullptr;
    QTableView*       putsTable_         = nullptr;
    QTableView*       positionTable_     = nullptr;
    QTableView*       orderTable_        = nullptr;
    QTableView*       tradeTable_        = nullptr;
    QTableView*       tapeTable_         = nullptr;
    QLineEdit*        search_            = nullptr;
    QLineEdit*        tradeSymbolEdit_   = nullptr;
    QCompleter*       searchCompleter_   = nullptr;
    QStringListModel* searchCompletions_ = nullptr;
    QCompleter*       tradeCompleter_    = nullptr;
    QStringListModel* tradeCompletions_  = nullptr;
    QPushButton*      addSymbolButton_   = nullptr;
    QPushButton*      componentsButton_  = nullptr;

    QDockWidget* watchDock_   = nullptr;
    QDockWidget* quoteDock_   = nullptr;
    QDockWidget* tapeDock_    = nullptr;
    QDockWidget* tradingDock_ = nullptr;
    QDockWidget* orderDock_   = nullptr;
    QDockWidget* futuresDock_ = nullptr;
    QDockWidget* optionsDock_ = nullptr;

    QLabel*      connectionLabel_   = nullptr;
    QLabel*      symbolLabel_       = nullptr;
    QLabel*      lastLabel_         = nullptr;
    QLabel*      nameLabel_         = nullptr;
    QLabel*      changeLabel_       = nullptr;
    QLabel*      changePctLabel_    = nullptr;
    QLabel*      bidAskLabel_       = nullptr;
    QLabel*      volumeLabel_       = nullptr;
    QLabel*      openInterestLabel_ = nullptr;
    QLabel*      oiChangeLabel_     = nullptr;
    QLabel*      highLowLabel_      = nullptr;
    QLabel*      futuresStatus_     = nullptr;
    QLabel*      orderStatus_       = nullptr;
    QLabel*      accountBalance_    = nullptr;
    QLabel*      accountAvailable_  = nullptr;
    QLabel*      accountMargin_     = nullptr;
    QLabel*      accountRisk_       = nullptr;
    QPushButton* themeButton_       = nullptr;

    QDoubleSpinBox* priceBox_     = nullptr;
    QSpinBox*       volumeBox_    = nullptr;
    QPushButton*    buyButton_    = nullptr;
    QPushButton*    sellButton_   = nullptr;
    QPushButton*    closeButton_  = nullptr;
    QPushButton*    cancelButton_ = nullptr;

    QStringList watchlist_;
    QString     selectedSymbol_     = QStringLiteral("SHFE.rb2701");
    qint64      selectedDurationNs_ = 86'400'000'000'000LL;
    QString     selectedInterval_   = QStringLiteral("1d");
    bool        dark_               = false;
    bool        nodeReady_          = false;
    bool        accountLoaded_      = false;
};

} // namespace tqsdk::ui
