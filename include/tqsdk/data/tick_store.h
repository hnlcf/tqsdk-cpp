#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/domain/tick.h"

namespace SQLite {
class Database;
} // namespace SQLite

namespace tqsdk {

/// node 侧 tick（quote 快照）SQLite 持久化。
///
/// 设计目标：
///   - TqApi 每固化一条 tick，通过 sink 投递到无锁读、异步写的存储；
///   - 写入在独立线程批处理，不阻塞行情合并/HTTP；
///   - WAL + FULLMUTEX，读查询和后台写入互不阻塞；
///   - SQLite 打开失败时自动降级为 disabled，不影响交易主链路；
///   - 按 symbol 保留最近 N 条，避免长期 live 把磁盘写满。
///
/// 注意：这里持久化的是 quote 快照，不是逐笔成交 tape；字段语义与 Tick 一致。
class TickStore {
public:
    struct Config {
        bool        enabled              = false;
        std::string file                 = "tick_cache.sqlite3";
        std::size_t max_ticks_per_series = 1'000'000;
        int         flush_interval_ms    = 250;
        std::size_t max_pending_ticks    = 50'000;
    };

    struct SeriesMeta {
        std::string last_datetime;
        int64_t     last_write_ns = 0;
        uint64_t    tick_count    = 0;
    };

    explicit TickStore(Config cfg);
    ~TickStore();

    TickStore(const TickStore&)            = delete;
    TickStore& operator=(const TickStore&) = delete;

    bool          ready() const noexcept { return ready_; }
    bool          enabled() const noexcept { return cfg_.enabled; }
    const Config& config() const noexcept { return cfg_; }

    /// 投递一条 tick；线程安全，满队列会 drop 并计数。
    void append(const Tick& tick);

    /// 读最近 limit 条，按 datetime/id 升序返回。
    std::vector<Tick> load_recent(const std::string& symbol, std::size_t limit);

    /// 读 [start_datetime, end_datetime] 区间；datetime 为 ISO 字符串，按字典序。
    std::vector<Tick> load_range(const std::string& symbol,
                                 const std::string& start_datetime,
                                 const std::string& end_datetime,
                                 std::size_t        limit);

    SeriesMeta meta(const std::string& symbol);

    /// 等待内存队列清空并完成一次落盘。
    void flush();

    /// 停止写线程（先 drain 队列）；幂等。
    void stop();

    nlohmann::json status_json() const;

private:
    struct PendingKey {
        std::string symbol;
        std::string datetime;
        double      last_price = 0.0;
        double      bid1       = 0.0;
        double      ask1       = 0.0;
        uint64_t    volume     = 0;

        bool operator<(const PendingKey& rhs) const {
            return std::tie(symbol, datetime, last_price, bid1, ask1, volume) <
                   std::tie(
                       rhs.symbol, rhs.datetime, rhs.last_price, rhs.bid1, rhs.ask1, rhs.volume);
        }
    };

    static PendingKey make_key(const Tick& tick);
    void              writer_loop(std::stop_token stop);
    void              write_batch(SQLite::Database& db, const std::map<PendingKey, Tick>& batch);
    void              update_series(SQLite::Database& db, const std::string& symbol);
    void              prune_series(SQLite::Database& db, const std::string& symbol);

    Config            cfg_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> writer_failed_{false};

    std::unique_ptr<SQLite::Database> reader_;
    mutable std::mutex                reader_mutex_;

    std::jthread                writer_;
    mutable std::mutex          queue_mutex_;
    std::condition_variable_any queue_cv_;
    std::map<PendingKey, Tick>  pending_;
    bool                        writing_ = false;
    std::string                 last_error_;

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> flushes_{0};
};

} // namespace tqsdk
