#pragma once

#include <QMainWindow>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTabWidget;
class QTableView;

namespace tqsdk::ui {

class InstrumentTableModel;
class KLineChartWidget;
class MarketStore;
class NodeClient;
class QuoteTableModel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(NodeClient* nodeClient, QWidget* parent = nullptr);
    ~MainWindow() override = default;

private:
    void buildUi();
    void buildToolbar();
    void buildWorkspace();
    void buildBottomDock();
    void connectNode();
    void selectSymbol(const QString& symbol);
    void setInterval(qint64 durationNs, const QString& label);
    void updateQuotePanel();
    void applyTheme();

    NodeClient*            nodeClient_   = nullptr;
    MarketStore*           marketStore_  = nullptr;
    QuoteTableModel*       watchModel_   = nullptr;
    InstrumentTableModel*  futuresModel_ = nullptr;
    QSortFilterProxyModel* futuresProxy_ = nullptr;
    KLineChartWidget*      chart_        = nullptr;

    QTableView* watchTable_   = nullptr;
    QTableView* futuresTable_ = nullptr;
    QTabWidget* centerTabs_   = nullptr;
    QLineEdit*  search_       = nullptr;

    QLabel*      connectionLabel_ = nullptr;
    QLabel*      symbolLabel_     = nullptr;
    QLabel*      lastLabel_       = nullptr;
    QLabel*      changeLabel_     = nullptr;
    QLabel*      bidAskLabel_     = nullptr;
    QPushButton* themeButton_     = nullptr;

    QStringList watchlist_;
    QString     selectedSymbol_     = QStringLiteral("SHFE.rb2701");
    qint64      selectedDurationNs_ = 60'000'000'000LL;
    QString     selectedInterval_   = QStringLiteral("1m");
    bool        dark_               = true;
};

} // namespace tqsdk::ui
