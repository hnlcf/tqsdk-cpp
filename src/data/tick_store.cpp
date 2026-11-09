#include "tqsdk/data/tick_store.h"

#include <SQLiteCpp/SQLiteCpp.h>

namespace tqsdk {

namespace {

int64_t wall_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double key_double(double value) {
    return std::isfinite(value) ? value : -std::numeric_limits<double>::max();
}

constexpr const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA busy_timeout=5000;
CREATE TABLE IF NOT EXISTS ticks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    symbol      TEXT    NOT NULL,
    datetime    TEXT    NOT NULL,
    last_price  REAL    NOT NULL,
    volume      INTEGER NOT NULL DEFAULT 0,
    bid1        REAL,
    ask1        REAL,
    payload     TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ticks_symbol_datetime
    ON ticks(symbol, datetime, id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_ticks_unique
    ON ticks(symbol, datetime, last_price, volume, bid1, ask1);
CREATE TABLE IF NOT EXISTS tick_series (
    symbol        TEXT PRIMARY KEY,
    last_datetime TEXT    NOT NULL DEFAULT '',
    last_write_ns INTEGER NOT NULL DEFAULT 0,
    tick_count    INTEGER NOT NULL DEFAULT 0
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

static TickStore::Config normalize_config(TickStore::Config cfg) {
    cfg.max_ticks_per_series = std::max<std::size_t>(1, cfg.max_ticks_per_series);
    cfg.flush_interval_ms    = std::max(1, cfg.flush_interval_ms);
    cfg.max_pending_ticks    = std::max<std::size_t>(1, cfg.max_pending_ticks);
    return cfg;
}

TickStore::TickStore(Config cfg)
    : cfg_(normalize_config(std::move(cfg))) {
    if (!cfg_.enabled) {
        return;
    }
    try {
        open_db(reader_, cfg_.file);
        ready_.store(true, std::memory_order_relaxed);
        writer_ = std::jthread([this](std::stop_token stop) { writer_loop(std::move(stop)); });
        SPDLOG_INFO("[TickStore] opened {} (max/ticks={}, flush={}ms)",
                    cfg_.file,
                    cfg_.max_ticks_per_series,
                    cfg_.flush_interval_ms);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        SPDLOG_ERROR("[TickStore] failed to open '{}': {}", cfg_.file, e.what());
    }
}

TickStore::~TickStore() {
    try {
        stop();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[TickStore] destructor stop failed: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[TickStore] destructor stop failed: unknown exception");
    }
}

TickStore::PendingKey TickStore::make_key(const Tick& tick) {
    PendingKey key;
    key.symbol     = tick.instrument_id;
    key.datetime   = tick.datetime;
    key.last_price = key_double(tick.last_price);
    key.bid1       = key_double(tick.bid_price[0]);
    key.ask1       = key_double(tick.ask_price[0]);
    key.volume     = tick.volume;
    return key;
}

void TickStore::append(const Tick& tick) {
    if (!ready_ || stopped_ || writer_failed_ || tick.instrument_id.empty() ||
        !std::isfinite(tick.last_price)) {
        return;
    }
    auto key = make_key(tick);
    {
        std::lock_guard lock(queue_mutex_);
        if (pending_.find(key) == pending_.end() && pending_.size() >= cfg_.max_pending_ticks) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pending_[std::move(key)] = tick;
        accepted_.fetch_add(1, std::memory_order_relaxed);
    }
    queue_cv_.notify_one();
}

std::vector<Tick> TickStore::load_recent(const std::string& symbol, std::size_t limit) {
    std::vector<Tick> result;
    if (!ready_ || limit == 0) {
        return result;
    }
    std::lock_guard lock(reader_mutex_);
    try {
        SQLite::Statement query(
            *reader_,
            "SELECT payload FROM ticks WHERE symbol = ? ORDER BY datetime DESC, id DESC LIMIT ?");
        query.bind(1, symbol);
        query.bind(2, static_cast<int64_t>(limit));
        while (query.executeStep()) {
            try {
                result.push_back(nlohmann::json::parse(query.getColumn(0).getString()).get<Tick>());
            } catch (const std::exception& e) {
                SPDLOG_WARN("[TickStore] bad payload for {}: {}", symbol, e.what());
            }
        }
        std::reverse(result.begin(), result.end());
    } catch (const std::exception& e) {
        SPDLOG_WARN("[TickStore] load_recent {} failed: {}", symbol, e.what());
        result.clear();
    }
    return result;
}

std::vector<Tick> TickStore::load_range(const std::string& symbol,
                                        const std::string& start_datetime,
                                        const std::string& end_datetime,
                                        std::size_t        limit) {
    std::vector<Tick> result;
    if (!ready_ || limit == 0) {
        return result;
    }
    std::lock_guard lock(reader_mutex_);
    try {
        SQLite::Statement query(
            *reader_,
            "SELECT payload FROM ticks WHERE symbol = ? AND datetime >= ? AND datetime <= ? "
            "ORDER BY datetime ASC, id ASC LIMIT ?");
        query.bind(1, symbol);
        query.bind(2, start_datetime);
        query.bind(3, end_datetime);
        query.bind(4, static_cast<int64_t>(limit));
        while (query.executeStep()) {
            try {
                result.push_back(nlohmann::json::parse(query.getColumn(0).getString()).get<Tick>());
            } catch (const std::exception& e) {
                SPDLOG_WARN("[TickStore] bad payload for {}: {}", symbol, e.what());
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("[TickStore] load_range {} failed: {}", symbol, e.what());
        result.clear();
    }
    return result;
}

TickStore::SeriesMeta TickStore::meta(const std::string& symbol) {
    SeriesMeta result;
    if (!ready_) {
        return result;
    }
    std::lock_guard lock(reader_mutex_);
    try {
        SQLite::Statement query(
            *reader_,
            "SELECT last_datetime, last_write_ns, tick_count FROM tick_series WHERE symbol = ?");
        query.bind(1, symbol);
        if (query.executeStep()) {
            result.last_datetime = query.getColumn(0).getString();
            result.last_write_ns = query.getColumn(1).getInt64();
            result.tick_count    = static_cast<uint64_t>(query.getColumn(2).getInt64());
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("[TickStore] meta {} failed: {}", symbol, e.what());
    }
    return result;
}

void TickStore::flush() {
    if (!writer_.joinable()) {
        return;
    }
    std::unique_lock lock(queue_mutex_);
    queue_cv_.notify_all();
    queue_cv_.wait(lock, [&] {
        return writer_failed_.load(std::memory_order_relaxed) || (pending_.empty() && !writing_);
    });
    flushes_.fetch_add(1, std::memory_order_relaxed);
}

void TickStore::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    if (writer_.joinable()) {
        queue_cv_.notify_all();
        writer_.request_stop();
        writer_.join();
    }
}

void TickStore::writer_loop(std::stop_token stop) {
    std::unique_ptr<SQLite::Database> db;
    try {
        open_db(db, cfg_.file);
    } catch (const std::exception& e) {
        writer_failed_.store(true, std::memory_order_relaxed);
        std::lock_guard lock(queue_mutex_);
        last_error_ = e.what();
        SPDLOG_ERROR("[TickStore] writer open failed: {}", e.what());
        return;
    }

    while (true) {
        std::map<PendingKey, Tick> batch;
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
        } catch (const std::exception& e) {
            writer_failed_.store(true, std::memory_order_relaxed);
            {
                std::lock_guard lock(queue_mutex_);
                last_error_ = e.what();
            }
            SPDLOG_ERROR("[TickStore] write batch failed: {}", e.what());
        }

        {
            std::lock_guard lock(queue_mutex_);
            writing_ = false;
        }
        queue_cv_.notify_all();
    }
}

void TickStore::write_batch(SQLite::Database& db, const std::map<PendingKey, Tick>& batch) {
    SQLite::Transaction tx(db);
    SQLite::Statement   insert(
        db,
        "INSERT OR IGNORE INTO ticks(symbol, datetime, last_price, volume, bid1, ask1, payload) "
        "VALUES(?, ?, ?, ?, ?, ?, ?)");

    std::set<std::string> symbols;
    for (const auto& [_, tick] : batch) {
        insert.bind(1, tick.instrument_id);
        insert.bind(2, tick.datetime);
        insert.bind(3, tick.last_price);
        insert.bind(4, static_cast<int64_t>(tick.volume));
        if (std::isfinite(tick.bid_price[0])) {
            insert.bind(5, tick.bid_price[0]);
        } else {
            insert.bind(5);
        }
        if (std::isfinite(tick.ask_price[0])) {
            insert.bind(6, tick.ask_price[0]);
        } else {
            insert.bind(6);
        }
        insert.bind(7, nlohmann::json(tick).dump());
        insert.executeStep();
        insert.reset();
        symbols.insert(tick.instrument_id);
    }

    for (const auto& symbol : symbols) {
        prune_series(db, symbol);
        update_series(db, symbol);
    }
    tx.commit();
}

void TickStore::update_series(SQLite::Database& db, const std::string& symbol) {
    SQLite::Statement count(db, "SELECT COUNT(*) FROM ticks WHERE symbol = ?");
    count.bind(1, symbol);
    int64_t n = 0;
    if (count.executeStep())
        n = count.getColumn(0).getInt64();

    SQLite::Statement last(db,
                           "SELECT datetime FROM ticks WHERE symbol = ? "
                           "ORDER BY datetime DESC, id DESC LIMIT 1");
    last.bind(1, symbol);
    std::string datetime;
    if (last.executeStep())
        datetime = last.getColumn(0).getString();

    SQLite::Statement upsert(
        db,
        "INSERT INTO tick_series(symbol, last_datetime, last_write_ns, tick_count) "
        "VALUES(?, ?, ?, ?) ON CONFLICT(symbol) DO UPDATE SET "
        "last_datetime=excluded.last_datetime, last_write_ns=excluded.last_write_ns, "
        "tick_count=excluded.tick_count");
    upsert.bind(1, symbol);
    upsert.bind(2, datetime);
    upsert.bind(3, wall_clock_ns());
    upsert.bind(4, n);
    upsert.executeStep();
}

void TickStore::prune_series(SQLite::Database& db, const std::string& symbol) {
    SQLite::Statement prune(
        db,
        "DELETE FROM ticks WHERE symbol = ? AND id NOT IN "
        "(SELECT id FROM ticks WHERE symbol = ? ORDER BY datetime DESC, id DESC LIMIT ?)");
    prune.bind(1, symbol);
    prune.bind(2, symbol);
    prune.bind(3, static_cast<int64_t>(cfg_.max_ticks_per_series));
    prune.executeStep();
}

nlohmann::json TickStore::status_json() const {
    nlohmann::json out{
        {"enabled", cfg_.enabled},
        {"ready", ready_.load(std::memory_order_relaxed)},
        {"writer_failed", writer_failed_.load(std::memory_order_relaxed)},
        {"file", cfg_.file},
        {"max_ticks_per_series", cfg_.max_ticks_per_series},
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
