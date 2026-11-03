#include "tqsdk/pch.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/gateway/ctp/ctp_task_queue.h"

#include "support/test_helpers.h"

namespace asio = boost::asio;

using namespace tqsdk;
using namespace tqsdk::ctp;

TEST(CtpConvert, OrderAndTradeJson) {
    CThostFtdcOrderField order{};
    set_field(order.OrderRef, sizeof(order.OrderRef), "rid_7");
    set_field(order.OrderSysID, sizeof(order.OrderSysID), "sys_7");
    set_field(order.ExchangeID, sizeof(order.ExchangeID), "SHFE");
    set_field(order.InstrumentID, sizeof(order.InstrumentID), "rb2610");
    order.Direction           = THOST_FTDC_D_Buy;
    order.CombOffsetFlag[0]   = THOST_FTDC_OF_Open;
    order.OrderPriceType      = THOST_FTDC_OPT_LimitPrice;
    order.LimitPrice          = 3800.0;
    order.VolumeTotalOriginal = 2;
    order.VolumeTotal         = 1;
    order.OrderStatus         = THOST_FTDC_OST_PartTradedQueueing;
    set_field(order.StatusMsg, sizeof(order.StatusMsg), "部分成交");

    auto oj = order_json(order, "rid_7");
    EXPECT_EQ(oj["order_id"], "rid_7");
    EXPECT_EQ(oj["exchange_order_id"], "sys_7");
    EXPECT_EQ(oj["instrument_id"], "SHFE.rb2610");
    EXPECT_EQ(oj["direction"], "BUY");
    EXPECT_EQ(oj["offset"], "OPEN");
    EXPECT_EQ(oj["volume_orign"], 2);
    EXPECT_EQ(oj["volume_left"], 1);
    EXPECT_EQ(oj["status"], "ALIVE");
    EXPECT_FALSE(oj["is_dead"].get<bool>());

    CThostFtdcTradeField trade{};
    set_field(trade.OrderRef, sizeof(trade.OrderRef), "rid_7");
    set_field(trade.TradeID, sizeof(trade.TradeID), "trade_9");
    set_field(trade.InstrumentID, sizeof(trade.InstrumentID), "rb2610");
    set_field(trade.ExchangeID, sizeof(trade.ExchangeID), "SHFE");
    trade.Direction  = THOST_FTDC_D_Buy;
    trade.OffsetFlag = THOST_FTDC_OF_Open;
    trade.Volume     = 1;
    trade.Price      = 3799.0;
    auto tj          = trade_json(trade, "rid_7");
    EXPECT_EQ(tj["trade_id"], "trade_9");
    EXPECT_EQ(tj["order_id"], "rid_7");
    EXPECT_EQ(tj["volume"], 1);
    EXPECT_EQ(tj["instrument_id"], "SHFE.rb2610");
    EXPECT_DOUBLE_EQ(tj["price"], 3799.0);
}

TEST(CtpConvert, SymbolNormalization) {
    EXPECT_EQ(tq_symbol("SHFE", "rb2701"), "SHFE.rb2701");
    EXPECT_EQ(tq_symbol("", "rb2701"), "rb2701");
    auto [exchange, instrument] = split_tq_symbol("SHFE.rb2701");
    EXPECT_EQ(exchange, "SHFE");
    EXPECT_EQ(instrument, "rb2701");
}

TEST(CtpConvert, ConvertsGbkStatusMessageToUtf8) {
    // "部分成交" 的 GBK 编码；CTP StatusMsg/ErrorMsg 是 GBK。
    const char gbk[] = "\xb2\xbf\xb7\xd6\xb3\xc9\xbd\xbb";
    EXPECT_EQ(cstr(gbk), "部分成交");
}

TEST(CtpConvert, AccountPositionDepth) {
    CThostFtdcTradingAccountField account{};
    account.Balance    = 1'000'000.0;
    account.Available  = 900'000.0;
    account.CurrMargin = 100'000.0;
    account.Commission = 10.0;
    auto aj            = account_json(account);
    EXPECT_DOUBLE_EQ(aj["balance"], 1'000'000.0);
    EXPECT_DOUBLE_EQ(aj["available"], 900'000.0);
    EXPECT_DOUBLE_EQ(aj["margin"], 100'000.0);
    EXPECT_NEAR(aj["risk_ratio"].get<double>(), 0.1, 1e-12);

    InstrumentSpec spec;
    spec.volume_multiple = 10.0;
    PositionAgg                     agg;
    CThostFtdcInvestorPositionField pos{};
    pos.PosiDirection  = THOST_FTDC_PD_Long;
    pos.Position       = 2;
    pos.TodayPosition  = 1;
    pos.OpenCost       = 76'000.0; // 3800 * 10 * 2
    pos.UseMargin      = 7'600.0;
    pos.PositionProfit = 20.0;
    add_position(agg, pos, spec);
    auto pj = position_json("rb2610", agg, spec);
    EXPECT_EQ(pj["volume_long"], 2);
    EXPECT_EQ(pj["volume_long_today"], 1);
    EXPECT_DOUBLE_EQ(pj["open_price_long"], 3800.0);
    EXPECT_DOUBLE_EQ(pj["float_profit_long"], 20.0);
    EXPECT_DOUBLE_EQ(pj["margin_long"], 7600.0);

    CThostFtdcDepthMarketDataField depth{};
    set_field(depth.ExchangeID, sizeof(depth.ExchangeID), "SHFE");
    set_field(depth.InstrumentID, sizeof(depth.InstrumentID), "rb2610");
    depth.LastPrice = 3800.0;
    depth.BidPrice1 = DBL_MAX;
    depth.AskPrice1 = 3801.0;
    depth.Volume    = 123;
    auto dj         = depth_json(depth);
    EXPECT_EQ(dj["instrument_id"], "SHFE.rb2610");
    EXPECT_DOUBLE_EQ(dj["last_price"], 3800.0);
    EXPECT_FALSE(std::isfinite(dj["bid_price1"].get<double>()));
    EXPECT_DOUBLE_EQ(dj["ask_price1"], 3801.0);
}

TEST(CtpTaskQueue, HighPriorityRunsBeforeQueuedQueries) {
    asio::io_context         ctx;
    auto                     queue = std::make_shared<CtpTaskQueue>(ctx.get_executor());
    std::vector<std::string> order;
    queue->push([&] { order.push_back("query1"); });
    queue->push([&] { order.push_back("query2"); });
    queue->push_high([&] { order.push_back("trade"); });

    tqsdk::test::drive_until(ctx, [&]() { return order.size() == 3; }, std::chrono::seconds(2));
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "trade");
    queue->stop();
}

TEST(CtpTaskQueue, RunsOnTargetExecutorAndDropsAfterStop) {
    asio::io_context ctx;
    auto             queue = std::make_shared<CtpTaskQueue>(ctx.get_executor());

    std::atomic<int> executed{0};
    std::thread::id  ran_on;
    std::thread      producer([&] {
        queue->push([&] {
            ran_on = std::this_thread::get_id();
            ++executed;
        });
    });
    producer.join();

    tqsdk::test::drive_until(ctx, [&]() { return executed.load() == 1; }, std::chrono::seconds(2));
    ASSERT_EQ(executed.load(), 1);
    EXPECT_EQ(ran_on, std::this_thread::get_id());

    queue->stop();
    queue->push([&] { ++executed; });
    ctx.restart();
    ctx.run_for(std::chrono::milliseconds(20));
    EXPECT_EQ(executed.load(), 1);
}
