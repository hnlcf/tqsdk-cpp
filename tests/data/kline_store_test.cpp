#include "tqsdk/pch.h"

#include "tqsdk/data/kline_store.h"

#include <fstream>

namespace {

class TempDb {
public:
    TempDb() {
        path_ = std::filesystem::temp_directory_path() /
                ("tqsdk_kline_store_" + std::to_string(std::random_device{}()) + ".sqlite3");
    }
    ~TempDb() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(path_.string() + "-wal", ec);
        std::filesystem::remove(path_.string() + "-shm", ec);
    }
    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

tqsdk::KlineBar bar(int64_t datetime, double close, uint64_t volume = 1) {
    tqsdk::KlineBar b;
    b.datetime = datetime;
    b.open     = close - 1;
    b.high     = close + 1;
    b.low      = close - 2;
    b.close    = close;
    b.volume   = volume;
    b.open_oi  = 10;
    b.close_oi = 11;
    return b;
}

tqsdk::KlineStore::Config config(const std::string& path, int max_bars = 100) {
    tqsdk::KlineStore::Config cfg;
    cfg.enabled             = true;
    cfg.file                = path;
    cfg.max_bars_per_series = max_bars;
    cfg.flush_interval_ms   = 10;
    cfg.fresh_after_sec     = 900;
    return cfg;
}

} // namespace

TEST(KlineStore, PersistsAndReloads) {
    TempDb db;
    {
        tqsdk::KlineStore store(config(db.path()));
        ASSERT_TRUE(store.ready());
        store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar(1, 100), bar(2, 101), bar(3, 102)});
        store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar(2, 201)});
        store.flush();

        auto loaded = store.load_recent("SHFE.rb2701", 60'000'000'000LL, 10);
        ASSERT_EQ(loaded.bars.size(), 3u);
        EXPECT_EQ(loaded.bars[0].datetime, 1);
        EXPECT_DOUBLE_EQ(loaded.bars[1].close, 201);
        EXPECT_EQ(loaded.bars[2].datetime, 3);
        EXPECT_EQ(loaded.meta.bar_count, 3u);
        EXPECT_TRUE(store.is_fresh(loaded.meta));
        EXPECT_GE(store.status_json()["written"].get<uint64_t>(), 3u);
    }

    {
        tqsdk::KlineStore reopened(config(db.path()));
        ASSERT_TRUE(reopened.ready());
        auto loaded = reopened.load_recent("SHFE.rb2701", 60'000'000'000LL, 10);
        ASSERT_EQ(loaded.bars.size(), 3u);
        EXPECT_DOUBLE_EQ(loaded.bars[1].close, 201);
        EXPECT_EQ(loaded.meta.bar_count, 3u);
    }
}

TEST(KlineStore, PrunesToNewestBars) {
    TempDb            db;
    tqsdk::KlineStore store(config(db.path(), 3));
    ASSERT_TRUE(store.ready());
    std::vector<tqsdk::KlineBar> bars;
    for (int i = 1; i <= 5; ++i) {
        bars.push_back(bar(i, 100 + i));
    }
    store.put_bars("DCE.m2701", 60'000'000'000LL, bars);
    store.flush();

    auto loaded = store.load_recent("DCE.m2701", 60'000'000'000LL, 10);
    ASSERT_EQ(loaded.bars.size(), 3u);
    EXPECT_EQ(loaded.bars.front().datetime, 3);
    EXPECT_EQ(loaded.bars.back().datetime, 5);
    EXPECT_EQ(loaded.meta.bar_count, 3u);
}

TEST(KlineStore, SkipsIncompleteBars) {
    TempDb            db;
    tqsdk::KlineStore store(config(db.path()));
    ASSERT_TRUE(store.ready());
    auto incomplete  = bar(2, 101);
    incomplete.close = std::numeric_limits<double>::quiet_NaN();
    store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar(1, 100), incomplete});
    store.flush();

    auto loaded = store.load_recent("SHFE.rb2701", 60'000'000'000LL, 10);
    ASSERT_EQ(loaded.bars.size(), 1u);
    EXPECT_EQ(loaded.bars[0].datetime, 1);
    EXPECT_EQ(store.status_json()["dropped"].get<uint64_t>(), 1u);
}

TEST(KlineStore, DisabledIsNoop) {
    TempDb db;
    auto   cfg  = config(db.path());
    cfg.enabled = false;
    tqsdk::KlineStore store(cfg);
    EXPECT_FALSE(store.ready());
    EXPECT_FALSE(store.enabled());
    store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar(1, 100)});
    store.flush();
    EXPECT_FALSE(std::filesystem::exists(db.path()));
}

TEST(KlineStore, CorruptDatabaseDegrades) {
    TempDb db;
    {
        std::ofstream out(db.path(), std::ios::binary | std::ios::trunc);
        out << "this is not a sqlite database";
    }
    tqsdk::KlineStore store(config(db.path()));
    EXPECT_FALSE(store.ready());
    EXPECT_TRUE(store.load_recent("SHFE.rb2701", 60'000'000'000LL, 10).bars.empty());
    store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar(1, 100)});
    store.flush();
    EXPECT_FALSE(store.status_json()["last_error"].get<std::string>().empty());
}

TEST(KlineStore, ConcurrentProducersAndFlush) {
    TempDb db;
    auto   cfg            = config(db.path(), 10'000);
    cfg.flush_interval_ms = 1;
    tqsdk::KlineStore store(cfg);
    ASSERT_TRUE(store.ready());

    constexpr int            kThreads   = 4;
    constexpr int            kPerThread = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int64_t dt = t * 10'000 + i + 1;
                store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar(dt, 100 + t)});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    store.flush();

    auto loaded = store.load_recent("SHFE.rb2701", 60'000'000'000LL, 10'000);
    EXPECT_EQ(loaded.bars.size(), kThreads * kPerThread);
    EXPECT_EQ(store.status_json()["dropped"].get<uint64_t>(), 0u);
}

TEST(KlineStore, FreshnessAdaptsToBarInterval) {
    TempDb                        db;
    tqsdk::KlineStore             store(config(db.path()));
    tqsdk::KlineStore::SeriesMeta meta;
    meta.last_write_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count() -
                         5 * 60 * 1'000'000'000LL;

    // 1 分钟 K 线：5 分钟前的缓存不够新，必须全量重取。
    EXPECT_FALSE(store.is_fresh(meta, 60LL * 1'000'000'000LL));
    // 1 小时 K 线：5 分钟前仍在新窗口内。
    EXPECT_TRUE(store.is_fresh(meta, 3600LL * 1'000'000'000LL));
    // 无 duration 的调用仍按配置上限判断。
    EXPECT_TRUE(store.is_fresh(meta));
}
