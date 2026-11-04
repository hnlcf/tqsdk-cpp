#pragma once

#include "net/NodeClient.h"
#include "net/Protocol.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

namespace tqsdk::ui {

class InstrumentTableModel;
class MarketStore;
class NodeClient;
class OrderTableModel;
class PositionTableModel;
class QuoteTableModel;
class TradeTableModel;
class TradeTapeModel;

/// Node 强类型信号 -> UI 模型/缓存 的应用层。
///
/// MainWindow 不再直接连接 quotes/klines/catalog/account/... 的逐字段处理；
/// 这里负责模型写入和可见性裁剪，UI 只接收“某块数据已变化”的语义信号。
class NodeDataController final : public QObject {
    Q_OBJECT

public:
    NodeDataController(NodeClient*           nodeClient,
                       MarketStore*          marketStore,
                       QuoteTableModel*      watchModel,
                       InstrumentTableModel* futuresModel,
                       InstrumentTableModel* callsModel,
                       InstrumentTableModel* putsModel,
                       PositionTableModel*   positionsModel,
                       OrderTableModel*      ordersModel,
                       TradeTableModel*      tradesModel,
                       TradeTapeModel*       tapeModel,
                       QObject*              parent = nullptr);

    void setSelectedContract(const QString& symbol, qint64 durationNs);
    void setFuturesVisible(bool visible);
    void setOptionsVisible(bool visible);

signals:
    void stateChanged(tqsdk::ui::NodeClient::State state, const QString& message);
    void errorOccurred(const QString& error);
    void quotesApplied();
    void klineSeriesChanged(const QString&                      symbol,
                            qint64                              durationNs,
                            const QVector<tqsdk::ui::KlineBar>& bars);
    void catalogLoaded(const QVector<tqsdk::ui::Instrument>& instruments);
    void optionChainLoaded(const tqsdk::ui::OptionChain& chain);
    void accountChanged(const tqsdk::ui::Account& account);
    void positionsChanged();
    void ordersChanged();
    void tradesChanged();
    void orderAckReceived(const tqsdk::ui::OrderAck& ack);
    void orderFailed(const QString& orderId, const QString& message);

private:
    void applyQuotesToVisibleTables();
    void applyQuotesToVisibleTables(const QVector<Quote>& quotes);

    NodeClient*           nodeClient_     = nullptr;
    MarketStore*          marketStore_    = nullptr;
    QuoteTableModel*      watchModel_     = nullptr;
    InstrumentTableModel* futuresModel_   = nullptr;
    InstrumentTableModel* callsModel_     = nullptr;
    InstrumentTableModel* putsModel_      = nullptr;
    PositionTableModel*   positionsModel_ = nullptr;
    OrderTableModel*      ordersModel_    = nullptr;
    TradeTableModel*      tradesModel_    = nullptr;
    TradeTapeModel*       tapeModel_      = nullptr;

    QHash<QString, Quote> latestQuotes_;
    QString               selectedSymbol_;
    qint64                selectedDurationNs_ = 0;
    bool                  futuresVisible_     = false;
    bool                  optionsVisible_     = false;
    bool                  ordersInitialized_  = false;
    QSet<QString>         notifiedOrderErrors_;
};

} // namespace tqsdk::ui
