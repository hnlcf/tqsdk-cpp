#pragma once

#include "net/Protocol.h"

#include <QObject>
#include <QStringList>

class QTimer;

namespace tqsdk::ui {

class InstrumentTableModel;
class MarketStore;
class NodeClient;
class QuoteTableModel;

/// 合约目录/自选/当前图表合约/周期/订阅协调器。
///
/// MainWindow 不再负责 fuzzy resolve、watchlist 或 subscription 细节；它只接收
/// selectedSymbolChanged/intervalChanged 并更新展示组件。
class SymbolController final : public QObject {
    Q_OBJECT

public:
    SymbolController(NodeClient*           nodeClient,
                     QuoteTableModel*      watchModel,
                     InstrumentTableModel* futuresModel,
                     InstrumentTableModel* callsModel,
                     InstrumentTableModel* putsModel,
                     MarketStore*          marketStore,
                     QObject*              parent = nullptr);

    void loadSettings();
    void saveSettings() const;

    QStringList watchlist() const { return watchlist_; }
    int         watchRow(const QString& symbol) const { return watchlist_.indexOf(symbol); }
    QString     selectedSymbol() const { return selectedSymbol_; }
    QString     selectedInterval() const { return selectedInterval_; }
    qint64      selectedDurationNs() const { return selectedDurationNs_; }

    QString           resolveSymbol(const QString& input) const;
    QStringList       searchCompletions(const QString& input, int limit = 20) const;
    const Instrument* findInstrument(const QString& symbol) const;

    /// 返回解析后的 symbol；无匹配返回空。调用方负责状态栏提示。
    QString addSymbolFromInput(const QString& input);
    void    addSymbolToWatchlist(const QString& symbol, bool request = true);
    void    selectSymbol(const QString& symbol);
    void    setInterval(qint64 durationNs, const QString& label);
    void    requestSelectedKlineHistory();
    void    requestOptionChain();
    void    requestInitialData();

signals:
    void selectedSymbolChanged(const QString& symbol);
    void intervalChanged(qint64 durationNs, const QString& label);
    void watchlistChanged();
    void statusMessage(const QString& message, int timeoutMs);

private:
    void  updateSubscription();
    void  scheduleSettingsSave();
    Quote catalogFallbackQuote(const QString& symbol) const;

    NodeClient*           nodeClient_        = nullptr;
    QuoteTableModel*      watchModel_        = nullptr;
    InstrumentTableModel* futuresModel_      = nullptr;
    InstrumentTableModel* callsModel_        = nullptr;
    InstrumentTableModel* putsModel_         = nullptr;
    MarketStore*          marketStore_       = nullptr;
    QTimer*               settingsSaveTimer_ = nullptr;

    QStringList watchlist_;
    QString     selectedSymbol_     = QStringLiteral("SHFE.rb2701");
    QString     selectedInterval_   = QStringLiteral("1d");
    qint64      selectedDurationNs_ = 86'400'000'000'000LL;
};

} // namespace tqsdk::ui
