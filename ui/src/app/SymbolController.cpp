#include "app/SymbolController.h"

#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "net/NodeClient.h"

#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include <algorithm>

namespace tqsdk::ui {
namespace {

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

SymbolController::SymbolController(NodeClient*           nodeClient,
                                   QuoteTableModel*      watchModel,
                                   InstrumentTableModel* futuresModel,
                                   InstrumentTableModel* callsModel,
                                   InstrumentTableModel* putsModel,
                                   MarketStore*          marketStore,
                                   QObject*              parent)
    : QObject(parent),
      nodeClient_(nodeClient),
      watchModel_(watchModel),
      futuresModel_(futuresModel),
      callsModel_(callsModel),
      putsModel_(putsModel),
      marketStore_(marketStore) {}

void SymbolController::loadSettings() {
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

    if (watchModel_ != nullptr) {
        watchModel_->setSymbols(watchlist_);
    }
}

void SymbolController::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/watchlist"), watchlist_);
    settings.setValue(QStringLiteral("ui/chart/symbol"), selectedSymbol_);
    settings.setValue(QStringLiteral("ui/chart/interval"), selectedInterval_);
    settings.setValue(QStringLiteral("ui/chart/durationNs"), selectedDurationNs_);
}

QString SymbolController::resolveSymbol(const QString& input) const {
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

QStringList SymbolController::searchCompletions(const QString& input, int limit) const {
    const QString query = input.trimmed();
    if (query.isEmpty() || futuresModel_ == nullptr) {
        return {};
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
    for (int i = 0; i < candidates.size() && i < limit; ++i) {
        suggestions.push_back(candidates.at(i).text);
    }
    return suggestions;
}

const Instrument* SymbolController::findInstrument(const QString& symbol) const {
    const auto findIn = [&symbol](const InstrumentTableModel* model) -> const Instrument* {
        if (model == nullptr) {
            return nullptr;
        }
        for (const auto& instrument : model->instruments()) {
            if (instrument.instrumentId == symbol) {
                return &instrument;
            }
        }
        return nullptr;
    };
    if (const auto* instrument = findIn(futuresModel_)) {
        return instrument;
    }
    if (const auto* instrument = findIn(callsModel_)) {
        return instrument;
    }
    return findIn(putsModel_);
}

QString SymbolController::addSymbolFromInput(const QString& input) {
    const QString symbol = resolveSymbol(input);
    if (symbol.isEmpty()) {
        emit statusMessage(tr("没有匹配的合约: %1").arg(input.trimmed()), 5000);
        return {};
    }
    addSymbolToWatchlist(symbol);
    emit statusMessage(tr("已加入自选: %1").arg(symbol), 3000);
    return symbol;
}

void SymbolController::addSymbolToWatchlist(const QString& symbol, bool request) {
    const QString trimmed = symbol.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    int row = watchlist_.indexOf(trimmed);
    if (row < 0) {
        watchlist_.push_back(trimmed);
        if (watchModel_ != nullptr) {
            watchModel_->setSymbols(watchlist_);
        }
        saveSettings();
        emit watchlistChanged();
        row = watchlist_.indexOf(trimmed);
    }
    if (request) {
        selectSymbol(trimmed);
        if (nodeClient_ != nullptr && nodeClient_->state() != NodeClient::State::Disconnected) {
            nodeClient_->requestQuotes({trimmed});
        }
    }
    Q_UNUSED(row);
}

void SymbolController::selectSymbol(const QString& symbol) {
    if (symbol.isEmpty()) {
        return;
    }
    selectedSymbol_ = symbol;
    if (!watchlist_.contains(symbol)) {
        watchlist_.push_back(symbol);
        if (watchModel_ != nullptr) {
            watchModel_->setSymbols(watchlist_);
        }
        emit watchlistChanged();
    }
    saveSettings();

    if (marketStore_ != nullptr && marketStore_->quote(symbol) == nullptr) {
        const Quote fallback = catalogFallbackQuote(symbol);
        if (!fallback.instrumentId.isEmpty()) {
            marketStore_->applyQuotes({fallback});
            if (watchModel_ != nullptr) {
                watchModel_->applyQuotes({fallback});
            }
        }
    }

    emit selectedSymbolChanged(symbol);
    updateSubscription();
    if (nodeClient_ != nullptr && nodeClient_->state() != NodeClient::State::Disconnected) {
        nodeClient_->requestQuotes({symbol});
        requestSelectedKlineHistory();
    }
}

void SymbolController::setInterval(qint64 durationNs, const QString& label) {
    if (durationNs <= 0) {
        return;
    }
    selectedDurationNs_ = durationNs;
    selectedInterval_   = label;
    saveSettings();
    emit intervalChanged(durationNs, label);
    updateSubscription();
    if (nodeClient_ != nullptr && nodeClient_->state() != NodeClient::State::Disconnected) {
        requestSelectedKlineHistory();
    }
}

void SymbolController::requestSelectedKlineHistory() {
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
            if (marketStore_ != nullptr && marketStore_->bars(symbol, duration).size() < 80) {
                nodeClient_->requestKlineHistory(symbol, duration, 200);
            }
        });
    }
}

void SymbolController::requestOptionChain() {
    if (nodeClient_ != nullptr && !selectedSymbol_.isEmpty()) {
        nodeClient_->requestOptionChain(selectedSymbol_);
    }
}

void SymbolController::requestInitialData() {
    if (nodeClient_ == nullptr || nodeClient_->state() == NodeClient::State::Disconnected) {
        return;
    }
    updateSubscription();

    CatalogQuery query;
    query.classes = QStringLiteral("FUTURE,FUTURE_CONT,FUTURE_INDEX");
    query.limit   = 3000;
    nodeClient_->requestCatalog(query);
    nodeClient_->requestQuotes(watchlist_);
    requestSelectedKlineHistory();
    nodeClient_->requestAccountSnapshot();
    nodeClient_->requestOrders();
    nodeClient_->requestPositions();
    nodeClient_->requestTrades();
}

void SymbolController::updateSubscription() {
    if (nodeClient_ == nullptr || selectedSymbol_.isEmpty()) {
        return;
    }
    Subscription subscription;
    subscription.symbols = watchlist_;
    subscription.klines.push_back({selectedSymbol_, selectedDurationNs_, 200});
    subscription.topics = {QStringLiteral("account"),
                           QStringLiteral("orders"),
                           QStringLiteral("trades"),
                           QStringLiteral("positions")};
    nodeClient_->setSubscription(subscription);
}

Quote SymbolController::catalogFallbackQuote(const QString& symbol) const {
    if (futuresModel_ == nullptr) {
        return {};
    }
    for (const auto& instrument : futuresModel_->instruments()) {
        if (instrument.instrumentId == symbol && instrument.lastPrice > 0.0) {
            Quote quote;
            quote.instrumentId = symbol;
            quote.lastPrice    = instrument.lastPrice;
            return quote;
        }
    }
    return {};
}

} // namespace tqsdk::ui
