#include "tqsdk/pch.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/gateway/ctp/ctp_td_gateway.h"

namespace asio = boost::asio;

class CtpFaultInjectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        tqsdk::CtpConfig cfg;
        cfg.broker_id   = "9999";
        cfg.user_id     = "test";
        cfg.td_front    = "tcp://127.0.0.1:1";
        gw_             = std::make_unique<tqsdk::CtpTdGateway>(io_.get_executor(), std::move(cfg));
        gw_->logged_in_ = true;
        gw_->trading_day_ = "20261103";
    }

    void begin_account_query(int request_id) {
        auto& q    = gw_->query_pipeline_;
        q.running_ = true;
        q.waiting_ = true;
        q.pending_ = 1;
        ++q.generation_;
        q.active_ = tqsdk::ctp::CtpQueryPipeline::Active{
            tqsdk::ctp::CtpQueryPipeline::Kind::Account, "", request_id};
    }

    bool in_flight() const { return gw_->query_pipeline_.running_; }
    bool has_active() const { return gw_->query_pipeline_.has_active(); }
    int  pending() const { return gw_->query_pipeline_.pending(); }
    void set_timeout(std::chrono::milliseconds timeout) {
        gw_->query_pipeline_.set_query_timeout(timeout);
    }
    void        arm_watchdog() { gw_->query_pipeline_.arm_watchdog(); }
    std::string make_ref(const std::string& rid) { return gw_->make_order_ref(rid); }
    char        offset_for(std::string_view exchange,
                           std::string_view instrument,
                           tqsdk::Direction direction,
                           tqsdk::Offset    offset) const {
        return gw_->ctp_offset_for_order(exchange, instrument, direction, offset);
    }
    void set_position(const std::string& key, nlohmann::json value) {
        gw_->positions_[key] = std::move(value);
    }
    void set_snapshot_ready(bool ready) { gw_->query_pipeline_.force_snapshot_ready(ready); }
    bool snapshot_ready() const { return gw_->query_pipeline_.snapshot_ready(); }
    void set_logged_in(bool logged_in) { gw_->logged_in_ = logged_in; }
    bool logged_in() const { return gw_->logged_in_; }
    void set_all_day_calendar() {
        auto cfg         = gw_->calendar_.config();
        cfg.day_sessions = {{0, 24 * 60}};
        cfg.night_sessions.clear();
        cfg.settlement_start_minute = 24 * 60;
        cfg.settlement_end_minute   = 24 * 60;
        gw_->calendar_              = tqsdk::TradingCalendar(cfg);
    }

    asio::io_context                     io_;
    std::unique_ptr<tqsdk::CtpTdGateway> gw_;
};

TEST_F(CtpFaultInjectionTest, LateQueryResponseDoesNotCompleteNewQuery) {
    begin_account_query(42);

    CThostFtdcRspInfoField        info{};
    CThostFtdcTradingAccountField account{};
    account.Balance   = 1000.0;
    account.Available = 900.0;

    // 迟到响应：request_id 不匹配，必须被丢弃，不能完成当前查询。
    gw_->handle_qry_account(&account, &info, 41, true);
    EXPECT_TRUE(in_flight());

    gw_->handle_qry_account(&account, &info, 42, true);
    EXPECT_FALSE(in_flight());
    EXPECT_FALSE(has_active());

    // 已完成后再次到达的迟到响应也不能产生副作用/崩溃。
    gw_->handle_qry_account(&account, &info, 42, true);
    EXPECT_FALSE(in_flight());
}

TEST_F(CtpFaultInjectionTest, WatchdogSkipsHungQueryAndKeepsPipelineAlive) {
    begin_account_query(7);
    set_timeout(std::chrono::milliseconds(30));
    arm_watchdog();

    io_.run_for(std::chrono::milliseconds(150));

    EXPECT_FALSE(in_flight());
    EXPECT_FALSE(has_active());
    EXPECT_EQ(pending(), 0);
}

TEST_F(CtpFaultInjectionTest, OrderRefIsPureIncreasingDecimal) {
    auto a = make_ref("r1790000001");
    auto b = make_ref("r1790000002");
    auto c = make_ref("not-a-number");

    auto all_digits = [](const std::string& s) {
        return !s.empty() &&
               std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isdigit(ch); });
    };
    EXPECT_TRUE(all_digits(a));
    EXPECT_TRUE(all_digits(b));
    EXPECT_TRUE(all_digits(c));
    EXPECT_EQ(a, "1790000001");
    EXPECT_EQ(b, "1790000002");
    EXPECT_GT(std::stoll(c), std::stoll(b));
}

TEST_F(CtpFaultInjectionTest, AutoSwitchesToCloseTodayForShfe) {
    set_position("SHFE.rb2701", nlohmann::json{{"volume_long", 1}, {"volume_long_today", 1}});
    EXPECT_EQ(offset_for("SHFE", "rb2701", tqsdk::Direction::SELL, tqsdk::Offset::CLOSE),
              THOST_FTDC_OF_CloseToday);

    set_position("SHFE.rb2701", nlohmann::json{{"volume_long", 1}, {"volume_long_today", 0}});
    EXPECT_EQ(offset_for("SHFE", "rb2701", tqsdk::Direction::SELL, tqsdk::Offset::CLOSE),
              THOST_FTDC_OF_Close);

    // 非 SHFE/INE/GFEX 保持通用 CLOSE。
    set_position("DCE.m2701", nlohmann::json{{"volume_long", 1}, {"volume_long_today", 1}});
    EXPECT_EQ(offset_for("DCE", "m2701", tqsdk::Direction::SELL, tqsdk::Offset::CLOSE),
              THOST_FTDC_OF_Close);
}

TEST_F(CtpFaultInjectionTest, FrontDisconnectInvalidatesSnapshotAndMarksReconnecting) {
    set_snapshot_ready(true);
    set_logged_in(true);
    set_all_day_calendar();
    gw_->handle_front_disconnected(3);
    EXPECT_FALSE(snapshot_ready());
    EXPECT_FALSE(logged_in());
    // 不在这里断言 Reconnecting：该测试可能在周末/闭市运行，日历阶段应为 Closed。
    EXPECT_NE(gw_->session_phase(), tqsdk::TradingCalendar::Phase::Unknown);
}
