#include "app/SymbolController.h"

#include "model/InstrumentTableModel.h"
#include "model/MarketStore.h"
#include "model/QuoteTableModel.h"
#include "net/NodeClient.h"

#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUrl>

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

QString symbolChartPrefix(const QString& symbol) {
    return QStringLiteral("ui/chart/symbols/%1/")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(symbol)));
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
      marketStore_(marketStore) {
    settingsSaveTimer_ = new QTimer(this);
    settingsSaveTimer_->setSingleShot(true);
    settingsSaveTimer_->setInterval(300);
    connect(settingsSaveTimer_, &QTimer::timeout, this, [this] { saveSettings(); });
}

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
    loadIntervalForSymbol(selectedSymbol_);

    if (watchModel_ != nullptr) {
        watchModel_->setSymbols(watchlist_);
    }
}

void SymbolController::scheduleSettingsSave() {
    if (settingsSaveTimer_ != nullptr) {
        settingsSaveTimer_->start();
    }
}

void SymbolController::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/watchlist"), watchlist_);
    settings.setValue(QStringLiteral("ui/chart/symbol"), selectedSymbol_);
    settings.setValue(QStringLiteral("ui/chart/interval"), selectedInterval_);
    settings.setValue(QStringLiteral("ui/chart/durationNs"), selectedDurationNs_);
    saveIntervalForSymbol(selectedSymbol_);
}

void SymbolController::saveIntervalForSymbol(const QString& symbol) const {
    if (symbol.isEmpty()) {
        return;
    }
    QSettings     settings;
    const QString prefix = symbolChartPrefix(symbol);
    settings.setValue(prefix + QStringLiteral("interval"), selectedInterval_);
    settings.setValue(prefix + QStringLiteral("durationNs"), selectedDurationNs_);
}

void SymbolController::loadIntervalForSymbol(const QString& symbol) {
    if (symbol.isEmpty()) {
        return;
    }
    QSettings     settings;
    const QString prefix   = symbolChartPrefix(symbol);
    const qint64  duration = settings.value(prefix + QStringLiteral("durationNs"), 0).toLongLong();
    if (duration <= 0) {
        return;
    }
    const QString interval =
        settings.value(prefix + QStringLiteral("interval"), selectedInterval_).toString();
    if (duration == selectedDurationNs_ && interval == selectedInterval_) {
        return;
    }
    selectedDurationNs_ = duration;
    selectedInterval_   = interval;
    emit intervalChanged(selectedDurationNs_, selectedInterval_);
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
    // Row lookup is O(1); this function is called on every quote repaint.
    if (futuresModel_ != nullptr) {
        if (const auto* instrument = futuresModel_->instrument(symbol)) {
            return instrument;
        }
    }
    if (callsModel_ != nullptr) {
        if (const auto* instrument = callsModel_->instrument(symbol)) {
            return instrument;
        }
    }
    return putsModel_ != nullptr ? putsModel_->instrument(symbol) : nullptr;
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
        scheduleSettingsSave();
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

bool SymbolController::removeFromWatchlist(const QString& symbol) {
    const int index = watchlist_.indexOf(symbol);
    if (index < 0) {
        return false;
    }
    watchlist_.removeAt(index);
    if (watchModel_ != nullptr) {
        watchModel_->setSymbols(watchlist_);
    }
    scheduleSettingsSave();
    emit watchlistChanged();
    emit statusMessage(tr("已从自选移除: %1").arg(symbol), 2500);

    if (symbol == selectedSymbol_ && !watchlist_.isEmpty()) {
        const int next = std::min(index, static_cast<int>(watchlist_.size()) - 1);
        selectSymbol(watchlist_.at(next));
    } else {
        updateSubscription();
    }
    return true;
}

bool SymbolController::moveWatchlistSymbol(const QString& symbol, int delta) {
    const int from = watchlist_.indexOf(symbol);
    if (from < 0 || delta == 0) {
        return false;
    }
    const int to = std::clamp(from + delta, 0, static_cast<int>(watchlist_.size()) - 1);
    if (to == from) {
        return false;
    }
    watchlist_.move(from, to);
    if (watchModel_ != nullptr) {
        watchModel_->setSymbols(watchlist_);
    }
    scheduleSettingsSave();
    emit watchlistChanged();
    emit statusMessage(tr("已调整自选顺序: %1").arg(symbol), 2000);
    return true;
}

void SymbolController::setWatchlistOrder(const QStringList& symbols) {
    if (symbols == watchlist_ || symbols.size() != watchlist_.size()) {
        return;
    }
    QStringList sortedOld = watchlist_;
    QStringList sortedNew = symbols;
    sortedOld.sort();
    sortedNew.sort();
    if (sortedOld != sortedNew) {
        return;
    }
    watchlist_ = symbols;
    if (watchModel_ != nullptr) {
        watchModel_->setSymbols(watchlist_);
    }
    scheduleSettingsSave();
    emit watchlistChanged();
}

void SymbolController::selectSymbol(const QString& symbol) {
    if (symbol.isEmpty()) {
        return;
    }
    saveIntervalForSymbol(selectedSymbol_);
    selectedSymbol_ = symbol;
    loadIntervalForSymbol(symbol);
    if (!watchlist_.contains(symbol)) {
        watchlist_.push_back(symbol);
        if (watchModel_ != nullptr) {
            watchModel_->setSymbols(watchlist_);
        }
        emit watchlistChanged();
    }
    scheduleSettingsSave();

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
    saveIntervalForSymbol(selectedSymbol_);
    scheduleSettingsSave();
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
    nodeClient_->requestTradingSnapshot();
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
