#include "tqsdk/pch.h"

#include "tqsdk/data/data_downloader.h"
#include "tqsdk/data/data_series.h"
#include "tqsdk/data/tick_series.h"

#include <gtest/gtest.h>

namespace {

using namespace tqsdk;

TEST(DataSeries, KeepsOnlyLatestCapacityItems) {
    DataSeries<int> series(3);
    for (int i = 0; i < 5; ++i) {
        series.append(i);
    }
    ASSERT_EQ(series.size(), 3u);
    EXPECT_EQ(*series.at(0), 2);
    EXPECT_EQ(*series.at(2), 4);
    EXPECT_EQ(*series.back(), 4);
    EXPECT_EQ(series.at(3), nullptr);
    EXPECT_EQ(series.revision(), 5u);

    series.reset(2);
    EXPECT_EQ(series.size(), 0u);
    EXPECT_EQ(series.capacity(), 2u);
}

TEST(TickSeries, DeduplicatesSameQuoteFingerprint) {
    TickSeries series("SHFE.rb2701", 2);
    Quote      q;
    q.instrument_id = "SHFE.rb2701";
    q.datetime      = "2026-11-09 21:00:00.000000";
    q.last_price    = 3000;
    q.bid_price1    = 2999;
    q.ask_price1    = 3001;
    q.volume        = 10;

    EXPECT_TRUE(series.append_quote(q));
    EXPECT_FALSE(series.append_quote(q));
    q.datetime = "2026-11-09 21:00:01.000000";
    q.volume   = 11;
    EXPECT_TRUE(series.append_quote(q));
    EXPECT_EQ(series.data.size(), 2u);
}

TEST(DataDownloader, ExportsTicksAndKlinesCsv) {
    TickSeries series("SHFE.rb2701", 5);
    Quote      q;
    q.instrument_id = "SHFE.rb2701";
    q.datetime      = "2026-11-09 21:00:00.000000";
    q.last_price    = 3000;
    q.bid_price1 = 2999;
    q.ask_price1 = 3001;
    q.volume     = 10;
    series.append_quote(q);

    DataDownloader downloader;
    const auto     ticks = downloader.ticks_csv(series);
    EXPECT_NE(ticks.find("datetime,instrument_id,last_price"), std::string::npos);
    EXPECT_NE(ticks.find("2026-11-09 21:00:00.000000,SHFE.rb2701,3000"), std::string::npos);

    KlineStream kline;
    kline.instrumnet_id = "SHFE.rb2701";
    kline.duration_ns   = 60'000'000'000;
    kline.bars.push_back(
        KlineBar{1'000, 10, 11, 9, 10.5, 100, 50, 60});
    const auto klines = downloader.klines_csv(kline);
    EXPECT_NE(klines.find("datetime,instrument_id,duration_ns"), std::string::npos);
    EXPECT_NE(klines.find("SHFE.rb2701,60000000000,10,11,9,10.5,100,50,60"),
              std::string::npos);
}

} // namespace
