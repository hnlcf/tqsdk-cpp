#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/constants.h"
#include "tqsdk/domain/kline.h"

namespace SQLite {
class Database;
} // namespace SQLite

namespace tqsdk {

/// node 侧 K 线历史缓存。
///
/// 设计目标：
///   - 重启后先给 TqApi 一个本地历史截面，避免每次都向远端索取完整 200 根；
///   - 写入异步批处理，不阻塞行情合并/HTTP 线程；
///   - WAL + 独立读写连接，读请求和后台写入互不阻塞；
///   - SQLite 打开/损坏时降级为 disabled，不影响行情主链路。
class KlineStore {
public:
    struct Config {
        bool        enabled             = false;
        std::string file                = std::string(constants::kDefaultMarketCacheFile);
        int         max_bars_per_series = constants::kDefaultKlineMaxBars;
        int         flush_interval_ms   = constants::kDefaultKlineFlushMs;
        std::size_t max_pending_bars    = constants::kDefaultKlinePendingBars;
        /// 缓存最近一次写入距现在不超过该秒数时，远端只请求最新 2 根；
        /// 超过则请求完整 view_width，避免停机期间缺失太多 bar。
        int64_t fresh_after_sec = constants::kDefaultKlineFreshSec;
    };

    struct SeriesMeta {
        int64_t  last_bar_ns   = 0;
        int64_t  last_write_ns = 0;
        uint64_t bar_count     = 0;
    };

    struct LoadResult {
        std::vector<KlineBar> bars;
        SeriesMeta            meta;
    };

    explicit KlineStore(Config cfg);
    ~KlineStore();

    KlineStore(const KlineStore&)            = delete;
    KlineStore& operator=(const KlineStore&) = delete;

    /// SQLite 读写连接是否初始化成功。失败时仍可继续跑行情，只是不缓存。
    bool          ready() const noexcept { return ready_; }
    bool          enabled() const noexcept { return cfg_.enabled; }
    const Config& config() const noexcept { return cfg_; }

    /// 读取最近 limit 根，按 datetime 升序返回。失败返回空。
    LoadResult load_recent(const std::string& symbol, int64_t duration_ns, int limit);

    /// 判断缓存是否足够新；无记录返回 false。
    /// duration_ns > 0 时还会限制最大可接受年龄为 2 根 bar，避免 1m 周期
    /// 停机 10 分钟后只补 2 根而留下缺口。
    bool is_fresh(const SeriesMeta& meta, int64_t duration_ns = 0) const noexcept;

    /// 入队异步写入；同一 (symbol,duration,datetime) 会在内存中合并。
    void
    put_bars(const std::string& symbol, int64_t duration_ns, const std::vector<KlineBar>& bars);

    /// 等待队列清空并完成一次落盘。用于测试/优雅退出。
    void flush();

    /// 停止后台写线程（会先 flush）。幂等。
    void stop();

    nlohmann::json status_json() const;

private:
    struct PendingKey {
        std::string symbol;
        int64_t     duration_ns = 0;
        int64_t     datetime_ns = 0;

        bool operator<(const PendingKey& rhs) const {
            return std::tie(symbol, duration_ns, datetime_ns) <
                   std::tie(rhs.symbol, rhs.duration_ns, rhs.datetime_ns);
        }
    };

    void writer_loop(std::stop_token stop);
    void write_batch(SQLite::Database& db, const std::map<PendingKey, KlineBar>& batch);
    void update_series_meta(SQLite::Database& db, const std::string& symbol, int64_t duration_ns);
    void prune_series(SQLite::Database& db, const std::string& symbol, int64_t duration_ns);

    Config            cfg_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> writer_failed_{false};

    std::unique_ptr<SQLite::Database> reader_;
    mutable std::mutex                reader_mutex_;

    std::jthread                   writer_;
    mutable std::mutex             queue_mutex_;
    std::condition_variable_any    queue_cv_;
    std::map<PendingKey, KlineBar> pending_;
    bool                           writing_ = false;
    std::string                    last_error_;

    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> flushes_{0};
};

} // namespace tqsdk
