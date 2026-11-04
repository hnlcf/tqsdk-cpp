#include "app/NodeDataController.h"

#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "model/TradingModels.h"
#include "net/NodeClient.h"

namespace tqsdk::ui {

NodeDataController::NodeDataController(NodeClient*           nodeClient,
                                       MarketStore*          marketStore,
                                       QuoteTableModel*      watchModel,
                                       InstrumentTableModel* futuresModel,
                                       InstrumentTableModel* callsModel,
                                       InstrumentTableModel* putsModel,
                                       PositionTableModel*   positionsModel,
                                       OrderTableModel*      ordersModel,
                                       TradeTableModel*      tradesModel,
                                       TradeTapeModel*       tapeModel,
                                       QObject*              parent)
    : QObject(parent),
      nodeClient_(nodeClient),
      marketStore_(marketStore),
      watchModel_(watchModel),
      futuresModel_(futuresModel),
      callsModel_(callsModel),
      putsModel_(putsModel),
      positionsModel_(positionsModel),
      ordersModel_(ordersModel),
      tradesModel_(tradesModel),
      tapeModel_(tapeModel) {
    if (nodeClient_ == nullptr) {
        return;
    }

    connect(nodeClient_,
            &NodeClient::stateChanged,
            this,
            [this](NodeClient::State state, const QString& message) {
                emit stateChanged(state, message);
            });
    connect(nodeClient_, &NodeClient::errorOccurred, this, &NodeDataController::errorOccurred);
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
        if (marketStore_ != nullptr) {
            marketStore_->applyQuotes(quotes);
        }
        if (watchModel_ != nullptr) {
            watchModel_->applyQuotes(quotes);
        }
        applyQuotesToVisibleTables(quotes);
        emit quotesApplied();
    });
    connect(nodeClient_,
            &NodeClient::klineUpdated,
            this,
            [this](const QString& symbol, qint64 durationNs, const KlineFrame& frame) {
                if (marketStore_ != nullptr) {
                    marketStore_->applyKline(symbol, durationNs, frame);
                }
                if (symbol == selectedSymbol_ && durationNs == selectedDurationNs_ &&
                    marketStore_ != nullptr) {
                    emit klineSeriesChanged(
                        symbol, durationNs, marketStore_->bars(symbol, durationNs));
                }
            });
    connect(nodeClient_,
            &NodeClient::klineHistoryLoaded,
            this,
            [this](const QString& symbol, qint64 durationNs, const QVector<KlineBar>& bars) {
                if (marketStore_ != nullptr) {
                    KlineFrame frame;
                    frame.reset = true;
                    frame.bars  = bars;
                    marketStore_->applyKline(symbol, durationNs, frame);
                }
                if (symbol == selectedSymbol_ && durationNs == selectedDurationNs_) {
                    emit klineSeriesChanged(symbol, durationNs, bars);
                }
            });
    connect(nodeClient_,
            &NodeClient::catalogLoaded,
            this,
            [this](const QVector<Instrument>& instruments) {
                if (futuresModel_ != nullptr) {
                    futuresModel_->setInstrumentsIncremental(instruments);
                }
                emit catalogLoaded(instruments);
            });
    connect(nodeClient_, &NodeClient::optionChainLoaded, this, [this](const OptionChain& chain) {
        if (callsModel_ != nullptr) {
            callsModel_->setInstrumentsIncremental(chain.calls);
        }
        if (putsModel_ != nullptr) {
            putsModel_->setInstrumentsIncremental(chain.puts);
        }
        emit optionChainLoaded(chain);
    });
    connect(nodeClient_, &NodeClient::accountUpdated, this, &NodeDataController::accountChanged);
    connect(nodeClient_,
            &NodeClient::positionsUpdated,
            this,
            [this](const QVector<Position>& positions) {
                if (positionsModel_ != nullptr) {
                    positionsModel_->applyPositions(positions);
                }
                emit positionsChanged();
            });
    connect(nodeClient_, &NodeClient::ordersUpdated, this, [this](const QVector<Order>& orders) {
        if (ordersModel_ != nullptr) {
            ordersModel_->applyOrders(orders);
        }
        for (const auto& order : orders) {
            if (order.orderId.isEmpty()) {
                continue;
            }
            if (!order.isError) {
                continue;
            }
            if (!ordersInitialized_) {
                // The first snapshot may contain historical errors. Do not
                // pop up old messages when the UI starts.
                notifiedOrderErrors_.insert(order.orderId);
                continue;
            }
            if (notifiedOrderErrors_.contains(order.orderId)) {
                continue;
            }
            notifiedOrderErrors_.insert(order.orderId);
            emit orderFailed(order.orderId,
                             order.message.isEmpty() ? tr("委托被柜台拒绝") : order.message);
        }
        ordersInitialized_ = true;
        emit ordersChanged();
    });
    connect(nodeClient_, &NodeClient::tradesUpdated, this, [this](const QVector<Trade>& trades) {
        if (tradesModel_ != nullptr) {
            tradesModel_->applyTrades(trades);
        }
        emit tradesChanged();
    });
    connect(
        nodeClient_, &NodeClient::orderAckReceived, this, &NodeDataController::orderAckReceived);
}

void NodeDataController::setSelectedContract(const QString& symbol, qint64 durationNs) {
    selectedSymbol_     = symbol;
    selectedDurationNs_ = durationNs;
}

void NodeDataController::setFuturesVisible(bool visible) {
    futuresVisible_ = visible;
    if (visible) {
        applyQuotesToVisibleTables();
    }
}

void NodeDataController::setOptionsVisible(bool visible) {
    optionsVisible_ = visible;
    if (visible) {
        applyQuotesToVisibleTables();
    }
}

void NodeDataController::applyQuotesToVisibleTables() {
    if (latestQuotes_.isEmpty()) {
        return;
    }
    applyQuotesToVisibleTables(latestQuotes_.values());
}

void NodeDataController::applyQuotesToVisibleTables(const QVector<Quote>& quotes) {
    if (quotes.isEmpty()) {
        return;
    }
    if (futuresVisible_ && futuresModel_ != nullptr) {
        futuresModel_->applyQuotes(quotes);
    }
    if (optionsVisible_) {
        if (callsModel_ != nullptr) {
            callsModel_->applyQuotes(quotes);
        }
        if (putsModel_ != nullptr) {
            putsModel_->applyQuotes(quotes);
        }
    }
}

} // namespace tqsdk::ui
