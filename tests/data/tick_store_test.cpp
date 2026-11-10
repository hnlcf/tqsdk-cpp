#include "tqsdk/pch.h"

#include "tqsdk/data/kline_store.h"
#include "tqsdk/data/tick_store.h"

#include <gtest/gtest.h>

using tqsdk::KlineBar;
using tqsdk::KlineStore;
using tqsdk::Tick;
using tqsdk::TickStore;

namespace {

std::string temp_db_path() {
    static std::atomic<uint64_t> seq{0};
    return (std::filesystem::temp_directory_path() /
            ("tqsdk_tick_store_" + std::to_string(::getpid()) + "_" +
             std::to_string(seq.fetch_add(1)) + ".sqlite3"))
        .string();
}

Tick make_tick(std::string datetime, double last, uint64_t volume) {
    Tick tick;
    tick.instrument_id = "SHFE.rb2701";
    tick.datetime      = std::move(datetime);
    tick.last_price    = last;
    tick.volume        = volume;
    tick.bid_price[0]  = last - 1;
    tick.ask_price[0]  = last + 1;
    tick.bid_volume[0] = 10;
    tick.ask_volume[0] = 12;
    tick.open_interest = 12345;
    tick.average       = last - 0.5;
    return tick;
}

} // namespace

TEST(TickStoreTest, AppendLoadPruneAndJsonRoundtrip) {
    const std::string path = temp_db_path();
    TickStore::Config cfg;
    cfg.enabled              = true;
    cfg.file                 = path;
    cfg.max_ticks_per_series = 3;
    cfg.flush_interval_ms    = 5;
    cfg.max_pending_ticks    = 16;

    {
        TickStore store(cfg);
        ASSERT_TRUE(store.ready());
        store.append(make_tick("2026-11-09 21:00:00.000000", 3000, 1));
        store.append(make_tick("2026-11-09 21:00:00.500000", 3001, 2));
        // 同一关键字段重复投递，队列内合并。
        store.append(make_tick("2026-11-09 21:00:00.500000", 3001, 2));
        store.append(make_tick("2026-11-09 21:00:01.000000", 3002, 3));
        store.flush();

        auto recent = store.load_recent("SHFE.rb2701", 10);
        ASSERT_EQ(recent.size(), 3u);
        EXPECT_EQ(recent.back().last_price, 3002);
        EXPECT_EQ(recent.back().open_interest, 12345);
        EXPECT_EQ(recent.back().ask_price[0], 3003);

        auto range = store.load_range(
            "SHFE.rb2701", "2026-11-09 21:00:00.400000", "2026-11-09 21:00:01.000000", 10);
        ASSERT_EQ(range.size(), 2u);
        EXPECT_EQ(range.front().last_price, 3001);

        // 超过 max_ticks_per_series 后保留最新 3 条。
        store.append(make_tick("2026-11-09 21:00:02.000000", 3003, 4));
        store.append(make_tick("2026-11-09 21:00:03.000000", 3004, 5));
        store.flush();
        recent = store.load_recent("SHFE.rb2701", 10);
        ASSERT_EQ(recent.size(), 3u);
        EXPECT_EQ(recent.front().last_price, 3002);
        EXPECT_EQ(recent.back().last_price, 3004);

        auto meta = store.meta("SHFE.rb2701");
        EXPECT_EQ(meta.tick_count, 3u);
        EXPECT_EQ(meta.last_datetime, "2026-11-09 21:00:03.000000");

        auto status = store.status_json();
        EXPECT_TRUE(status["ready"].get<bool>());
        EXPECT_GE(status["written"].get<uint64_t>(), 5u);
        store.stop();
    }

    // 重新打开同一文件，验证落盘和 payload 反序列化。
    {
        TickStore reopened(cfg);
        ASSERT_TRUE(reopened.ready());
        auto ticks = reopened.load_recent("SHFE.rb2701", 10);
        ASSERT_EQ(ticks.size(), 3u);
        EXPECT_EQ(ticks.back().last_price, 3004);
        EXPECT_EQ(ticks.back().bid_volume[0], 10u);
        reopened.stop();
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(TickStoreTest, SharesSqliteFileWithKlineStore) {
    const std::string path = temp_db_path();

    TickStore::Config tick_cfg;
    tick_cfg.enabled           = true;
    tick_cfg.file              = path;
    tick_cfg.flush_interval_ms = 5;
    KlineStore::Config kline_cfg;
    kline_cfg.enabled           = true;
    kline_cfg.file              = path;
    kline_cfg.flush_interval_ms = 5;

    {
        TickStore  tick_store(tick_cfg);
        KlineStore kline_store(kline_cfg);
        ASSERT_TRUE(tick_store.ready());
        ASSERT_TRUE(kline_store.ready());

        tick_store.append(make_tick("2026-11-09 21:00:00.000000", 3000, 1));
        KlineBar bar;
        bar.datetime = 1'000'000'000;
        bar.open = bar.high = bar.low = bar.close = 3000;
        bar.volume                                = 1;
        kline_store.put_bars("SHFE.rb2701", 60'000'000'000LL, {bar});

        tick_store.flush();
        kline_store.flush();
        tick_store.stop();
        kline_store.stop();
    }

    {
        TickStore  tick_store(tick_cfg);
        KlineStore kline_store(kline_cfg);
        ASSERT_TRUE(tick_store.ready());
        ASSERT_TRUE(kline_store.ready());
        EXPECT_EQ(tick_store.load_recent("SHFE.rb2701", 10).size(), 1u);
        EXPECT_EQ(kline_store.load_recent("SHFE.rb2701", 60'000'000'000LL, 10).bars.size(), 1u);
        tick_store.stop();
        kline_store.stop();
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
