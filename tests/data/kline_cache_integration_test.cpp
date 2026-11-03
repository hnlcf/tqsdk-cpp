#include "tqsdk/pch.h"

#include "tqsdk/data/kline_store.h"
#include "tqsdk/gateway/mock/mock_md_gateway.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;

namespace {

class TempDb {
public:
    TempDb() {
        path_ = std::filesystem::temp_directory_path() /
                ("tqsdk_kline_cache_" + std::to_string(std::random_device{}()) + ".sqlite3");
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

KlineBar cached_bar(int64_t datetime, double close) {
    KlineBar b;
    b.datetime = datetime;
    b.open     = close - 1;
    b.high     = close + 1;
    b.low      = close - 2;
    b.close    = close;
    b.volume   = 10;
    b.open_oi  = 20;
    b.close_oi = 21;
    return b;
}

nlohmann::json remote_bar(int64_t datetime, double close) {
    return nlohmann::json{{"datetime", datetime},
                          {"open", close - 1},
                          {"high", close + 1},
                          {"low", close - 2},
                          {"close", close},
                          {"volume", 10},
                          {"open_oi", 20},
                          {"close_oi", 21}};
}

} // namespace

TEST(KlineCache, HydratesBeforeRemoteTailAndMerges) {
    TempDb             db;
    KlineStore::Config cfg;
    cfg.enabled           = true;
    cfg.file              = db.path();
    cfg.flush_interval_ms = 10;

    auto store = std::make_shared<KlineStore>(cfg);
    ASSERT_TRUE(store->ready());
    store->put_bars("SHFE.rb2701",
                    60'000'000'000LL,
                    {cached_bar(1, 100),
                     cached_bar(2, 101),
                     cached_bar(3, 102),
                     cached_bar(4, 103),
                     cached_bar(5, 104)});
    store->flush();
    ASSERT_EQ(store->load_recent("SHFE.rb2701", 60'000'000'000LL, 10).bars.size(), 5u);

    asio::io_context ctx;
    auto             md_owner = std::make_unique<MockMdGateway>(ctx.get_executor());
    auto*            md       = md_owner.get();
    auto api = std::make_unique<TqApi>(ctx.get_executor(), std::move(md_owner), nullptr, store);
    api->start();

    auto* stream = api->get_kline_serial("SHFE.rb2701", 60'000'000'000LL, 5);
    ASSERT_NE(stream, nullptr);
    EXPECT_TRUE(stream->cached_history);
    ASSERT_EQ(stream->bars.size(), 5u);
    EXPECT_EQ(stream->bars.front().datetime, 1);
    EXPECT_EQ(stream->bars.back().datetime, 5);

    ASSERT_TRUE(test::drive_until(
        ctx,
        [&] { return md->is_kline_subscribed("SHFE.rb2701", 60'000'000'000LL); },
        std::chrono::seconds(2)));
    EXPECT_EQ(md->kline_view_width("SHFE.rb2701", 60'000'000'000LL), 2);

    md->inject(nlohmann::json{
        {"klines",
         {{"SHFE.rb2701",
           {{"60000000000", nlohmann::json::array({remote_bar(5, 204), remote_bar(6, 205)})}}}}}});
    ASSERT_TRUE(test::drive_until(
        ctx, [&] { return stream->bars.back().datetime == 6; }, std::chrono::seconds(2)));

    EXPECT_EQ(stream->bars.size(), 5u);
    EXPECT_EQ(stream->bars.front().datetime, 2);
    EXPECT_EQ(stream->bars.back().datetime, 6);
    EXPECT_DOUBLE_EQ(stream->bars[3].close, 204);

    store->flush();
    auto persisted = store->load_recent("SHFE.rb2701", 60'000'000'000LL, 10);
    ASSERT_EQ(persisted.bars.size(), 6u);
    EXPECT_EQ(persisted.bars.back().datetime, 6);
    EXPECT_DOUBLE_EQ(persisted.bars.back().close, 205);

    api->close();
    test::pump(ctx, 20);
    store->flush();
    store->stop();
}

TEST(KlineCache, NoCacheRequestsFullWidth) {
    TempDb             db;
    KlineStore::Config cfg;
    cfg.enabled           = true;
    cfg.file              = db.path();
    cfg.flush_interval_ms = 10;

    auto             store = std::make_shared<KlineStore>(cfg);
    asio::io_context ctx;
    auto             md_owner = std::make_unique<MockMdGateway>(ctx.get_executor());
    auto*            md       = md_owner.get();
    auto api = std::make_unique<TqApi>(ctx.get_executor(), std::move(md_owner), nullptr, store);
    api->start();

    auto* stream = api->get_kline_serial("DCE.m2701", 60'000'000'000LL, 200);
    ASSERT_NE(stream, nullptr);
    EXPECT_FALSE(stream->cached_history);
    ASSERT_TRUE(test::drive_until(
        ctx,
        [&] { return md->kline_view_width("DCE.m2701", 60'000'000'000LL) == 200; },
        std::chrono::seconds(2)));

    api->close();
    test::pump(ctx, 20);
    store->stop();
}
