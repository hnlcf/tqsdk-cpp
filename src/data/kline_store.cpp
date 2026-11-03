#include "tqsdk/data/kline_store.h"

#include <SQLiteCpp/SQLiteCpp.h>

namespace tqsdk {

namespace {

int64_t wall_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA busy_timeout=5000;
CREATE TABLE IF NOT EXISTS kline_bars (
    symbol       TEXT    NOT NULL,
    duration_ns  INTEGER NOT NULL,
    datetime_ns  INTEGER NOT NULL,
    open         REAL    NOT NULL,
    high         REAL    NOT NULL,
    low          REAL    NOT NULL,
    close        REAL    NOT NULL,
    volume       INTEGER NOT NULL DEFAULT 0,
    open_oi      INTEGER NOT NULL DEFAULT 0,
    close_oi     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (symbol, duration_ns, datetime_ns)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS kline_series (
    symbol        TEXT    NOT NULL,
    duration_ns   INTEGER NOT NULL,
    last_bar_ns   INTEGER NOT NULL DEFAULT 0,
    last_write_ns INTEGER NOT NULL DEFAULT 0,
    bar_count     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (symbol, duration_ns)
) WITHOUT ROWID;
)SQL";

void open_db(std::unique_ptr<SQLite::Database>& db, const std::string& file) {
    if (file != ":memory:") {
        const auto parent = std::filesystem::path(file).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
    }
    db = std::make_unique<SQLite::Database>(
        file, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE | SQLite::OPEN_FULLMUTEX);
    db->exec(kSchema);
}

} // namespace

KlineStore::KlineStore(Config cfg)
    : cfg_(std::move(cfg)) {
    if (!cfg_.enabled) {
        return;
    }
    cfg_.flush_interval_ms   = std::max(1, cfg_.flush_interval_ms);
    cfg_.max_bars_per_series = std::max(1, cfg_.max_bars_per_series);
    cfg_.max_pending_bars    = std::max<std::size_t>(1, cfg_.max_pending_bars);
    cfg_.fresh_after_sec     = std::max<int64_t>(0, cfg_.fresh_after_sec);

    try {
        open_db(reader_, cfg_.file);
        ready_.store(true, std::memory_order_relaxed);
        writer_ = std::jthread([this](std::stop_token stop) { writer_loop(std::move(stop)); });
        SPDLOG_INFO("[KlineStore] opened {} (max_bars/series={}, flush={}ms)",
                    cfg_.file,
                    cfg_.max_bars_per_series,
                    cfg_.flush_interval_ms);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        SPDLOG_ERROR("[KlineStore] failed to open '{}': {}", cfg_.file, e.what());
    }
}

KlineStore::~KlineStore() {
    try {
        stop();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[KlineStore] destructor stop failed: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[KlineStore] destructor stop failed: unknown exception");
    }
}

bool KlineStore::is_fresh(const SeriesMeta& meta, int64_t duration_ns) const noexcept {
    if (meta.last_write_ns <= 0) {
        return false;
    }
    const int64_t age = wall_clock_ns() - meta.last_write_ns;
    if (age < 0) {
        return false;
    }
    int64_t max_age_ns = cfg_.fresh_after_sec * 1'000'000'000LL;
    if (duration_ns > 0) {
        const int64_t two_bars_ns = duration_ns * 2;
        const int64_t floor_ns    = 60LL * 1'000'000'000LL;
        max_age_ns                = std::min(max_age_ns, std::max(floor_ns, two_bars_ns));
    }
    return age <= max_age_ns;
}

KlineStore::LoadResult
KlineStore::load_recent(const std::string& symbol, int64_t duration_ns, int limit) {
    LoadResult result;
    if (!ready_ || limit <= 0) {
        return result;
    }

    std::lock_guard lock(reader_mutex_);
    try {
        SQLite::Statement bars(
            *reader_,
            "SELECT datetime_ns, open, high, low, close, volume, open_oi, close_oi "
            "FROM kline_bars WHERE symbol = ? AND duration_ns = ? "
            "ORDER BY datetime_ns DESC LIMIT ?");
        bars.bind(1, symbol);
        bars.bind(2, static_cast<int64_t>(duration_ns));
        bars.bind(3, limit);
        while (bars.executeStep()) {
            KlineBar bar;
            bar.datetime = static_cast<TimeStamp>(bars.getColumn(0).getInt64());
            bar.open     = bars.getColumn(1).getDouble();
            bar.high     = bars.getColumn(2).getDouble();
            bar.low      = bars.getColumn(3).getDouble();
            bar.close    = bars.getColumn(4).getDouble();
            bar.volume   = static_cast<uint64_t>(bars.getColumn(5).getInt64());
            bar.open_oi  = static_cast<uint64_t>(bars.getColumn(6).getInt64());
            bar.close_oi = static_cast<uint64_t>(bars.getColumn(7).getInt64());
            result.bars.push_back(bar);
        }
        std::reverse(result.bars.begin(), result.bars.end());

        SQLite::Statement meta(*reader_,
                               "SELECT last_bar_ns, last_write_ns, bar_count FROM kline_series "
                               "WHERE symbol = ? AND duration_ns = ?");
        meta.bind(1, symbol);
        meta.bind(2, static_cast<int64_t>(duration_ns));
        if (meta.executeStep()) {
            result.meta.last_bar_ns   = meta.getColumn(0).getInt64();
            result.meta.last_write_ns = meta.getColumn(1).getInt64();
            result.meta.bar_count     = static_cast<uint64_t>(meta.getColumn(2).getInt64());
        } else if (!result.bars.empty()) {
            result.meta.last_bar_ns = static_cast<int64_t>(result.bars.back().datetime);
            result.meta.bar_count   = result.bars.size();
        }
    } catch (const std::exception& e) {
        {
            std::lock_guard lock(queue_mutex_);
            last_error_ = e.what();
        }
        SPDLOG_WARN("[KlineStore] load {}:{} failed: {}", symbol, duration_ns, e.what());
        result = {};
    }
    return result;
}

void KlineStore::put_bars(const std::string&           symbol,
                          int64_t                      duration_ns,
                          const std::vector<KlineBar>& bars) {
    if (!ready_ || stopped_ || bars.empty()) {
        return;
    }

    std::lock_guard lock(queue_mutex_);
    for (const auto& bar : bars) {
        // SQLite 会把 NaN 绑定成 NULL；价格不完整的 bar 不写入，避免整批事务失败。
        if (!std::isfinite(bar.open) || !std::isfinite(bar.high) || !std::isfinite(bar.low) ||
            !std::isfinite(bar.close)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        PendingKey key{symbol, duration_ns, static_cast<int64_t>(bar.datetime)};
        if (pending_.find(key) == pending_.end() && pending_.size() >= cfg_.max_pending_bars) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        pending_[std::move(key)] = bar;
        accepted_.fetch_add(1, std::memory_order_relaxed);
    }
    queue_cv_.notify_one();
}

void KlineStore::flush() {
    if (!writer_.joinable()) {
        return;
    }
    std::unique_lock lock(queue_mutex_);
    queue_cv_.notify_all();
    queue_cv_.wait(lock, [&] { return pending_.empty() && !writing_; });
}

void KlineStore::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    if (writer_.joinable()) {
        queue_cv_.notify_all();
        writer_.request_stop();
        writer_.join();
    }
}

void KlineStore::writer_loop(std::stop_token stop) {
    std::unique_ptr<SQLite::Database> db;
    try {
        open_db(db, cfg_.file);
    } catch (const std::exception& e) {
        writer_failed_.store(true, std::memory_order_relaxed);
        std::lock_guard lock(queue_mutex_);
        last_error_ = e.what();
        SPDLOG_ERROR("[KlineStore] writer open failed: {}", e.what());
        return;
    }

    while (!stop.stop_requested()) {
        std::map<PendingKey, KlineBar> batch;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(cfg_.flush_interval_ms), [&] {
                return stop.stop_requested() || !pending_.empty();
            });
            if (pending_.empty()) {
                if (stop.stop_requested()) {
                    break;
                }
                continue;
            }
            batch = std::move(pending_);
            pending_.clear();
            writing_ = true;
        }

        try {
            write_batch(*db, batch);
            written_.fetch_add(batch.size(), std::memory_order_relaxed);
            flushes_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            writer_failed_.store(true, std::memory_order_relaxed);
            std::lock_guard lock(queue_mutex_);
            last_error_ = e.what();
            dropped_.fetch_add(batch.size(), std::memory_order_relaxed);
            SPDLOG_ERROR("[KlineStore] flush failed ({} bars): {}", batch.size(), e.what());
        }

        {
            std::lock_guard lock(queue_mutex_);
            writing_ = false;
        }
        queue_cv_.notify_all();
    }

    // 退出前再排空一次，确保 SIGTERM 前的最后一根 bar 不丢。
    std::map<PendingKey, KlineBar> tail;
    {
        std::lock_guard lock(queue_mutex_);
        tail = std::move(pending_);
        pending_.clear();
        writing_ = !tail.empty();
    }
    if (!tail.empty()) {
        try {
            write_batch(*db, tail);
            written_.fetch_add(tail.size(), std::memory_order_relaxed);
        } catch (const std::exception& e) {
            writer_failed_.store(true, std::memory_order_relaxed);
            std::lock_guard lock(queue_mutex_);
            last_error_ = e.what();
            SPDLOG_ERROR("[KlineStore] final flush failed: {}", e.what());
        }
        std::lock_guard lock(queue_mutex_);
        writing_ = false;
        queue_cv_.notify_all();
    }
}

void KlineStore::write_batch(SQLite::Database& db, const std::map<PendingKey, KlineBar>& batch) {
    SQLite::Transaction tx(db);
    SQLite::Statement   upsert(
        db,
        "INSERT INTO kline_bars(symbol, duration_ns, datetime_ns, open, high, low, close, "
        "volume, open_oi, close_oi) VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(symbol, duration_ns, datetime_ns) DO UPDATE SET "
        "open=excluded.open, high=excluded.high, low=excluded.low, close=excluded.close, "
        "volume=excluded.volume, open_oi=excluded.open_oi, close_oi=excluded.close_oi");

    std::set<std::pair<std::string, int64_t>> touched;
    for (const auto& [key, bar] : batch) {
        upsert.bind(1, key.symbol);
        upsert.bind(2, static_cast<int64_t>(key.duration_ns));
        upsert.bind(3, static_cast<int64_t>(key.datetime_ns));
        upsert.bind(4, bar.open);
        upsert.bind(5, bar.high);
        upsert.bind(6, bar.low);
        upsert.bind(7, bar.close);
        upsert.bind(8, static_cast<int64_t>(bar.volume));
        upsert.bind(9, static_cast<int64_t>(bar.open_oi));
        upsert.bind(10, static_cast<int64_t>(bar.close_oi));
        upsert.exec();
        upsert.reset();
        upsert.clearBindings();
        touched.emplace(key.symbol, key.duration_ns);
    }

    for (const auto& [symbol, duration_ns] : touched) {
        prune_series(db, symbol, duration_ns);
        update_series_meta(db, symbol, duration_ns);
    }
    tx.commit();
}

void KlineStore::update_series_meta(SQLite::Database&  db,
                                    const std::string& symbol,
                                    int64_t            duration_ns) {
    const int64_t     now = wall_clock_ns();
    SQLite::Statement stmt(
        db,
        "INSERT INTO kline_series(symbol, duration_ns, last_bar_ns, last_write_ns, bar_count) "
        "VALUES(?,?,"
        "  COALESCE((SELECT MAX(datetime_ns) FROM kline_bars WHERE symbol=? AND duration_ns=?),0),"
        "  ?,"
        "  (SELECT COUNT(*) FROM kline_bars WHERE symbol=? AND duration_ns=?)) "
        "ON CONFLICT(symbol, duration_ns) DO UPDATE SET "
        "last_bar_ns=excluded.last_bar_ns, last_write_ns=excluded.last_write_ns, "
        "bar_count=excluded.bar_count");
    stmt.bind(1, symbol);
    stmt.bind(2, static_cast<int64_t>(duration_ns));
    stmt.bind(3, symbol);
    stmt.bind(4, static_cast<int64_t>(duration_ns));
    stmt.bind(5, static_cast<int64_t>(now));
    stmt.bind(6, symbol);
    stmt.bind(7, static_cast<int64_t>(duration_ns));
    stmt.exec();
}

void KlineStore::prune_series(SQLite::Database&  db,
                              const std::string& symbol,
                              int64_t            duration_ns) {
    SQLite::Statement stmt(
        db,
        "DELETE FROM kline_bars WHERE symbol=? AND duration_ns=? AND datetime_ns NOT IN ("
        "  SELECT datetime_ns FROM kline_bars WHERE symbol=? AND duration_ns=? "
        "  ORDER BY datetime_ns DESC LIMIT ?)");
    stmt.bind(1, symbol);
    stmt.bind(2, static_cast<int64_t>(duration_ns));
    stmt.bind(3, symbol);
    stmt.bind(4, static_cast<int64_t>(duration_ns));
    stmt.bind(5, cfg_.max_bars_per_series);
    stmt.exec();
}

nlohmann::json KlineStore::status_json() const {
    nlohmann::json out{
        {"enabled", cfg_.enabled},
        {"ready", ready_.load(std::memory_order_relaxed)},
        {"writer_failed", writer_failed_.load(std::memory_order_relaxed)},
        {"file", cfg_.file},
        {"max_bars_per_series", cfg_.max_bars_per_series},
        {"flush_interval_ms", cfg_.flush_interval_ms},
        {"fresh_after_sec", cfg_.fresh_after_sec},
        {"accepted", accepted_.load(std::memory_order_relaxed)},
        {"written", written_.load(std::memory_order_relaxed)},
        {"dropped", dropped_.load(std::memory_order_relaxed)},
        {"flushes", flushes_.load(std::memory_order_relaxed)},
    };
    {
        std::lock_guard lock(queue_mutex_);
        out["pending"]    = pending_.size();
        out["last_error"] = last_error_;
    }
    return out;
}

} // namespace tqsdk
