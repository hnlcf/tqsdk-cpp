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
        gw_->query_in_flight_ = true;
        gw_->query_waiting_   = true;
        gw_->pending_queries_ = 1;
        ++gw_->query_generation_;
        gw_->active_query_ = tqsdk::CtpTdGateway::ActiveQuery{
            tqsdk::CtpTdGateway::QueryKind::Account, "", request_id};
    }

    bool        in_flight() const { return gw_->query_in_flight_; }
    bool        has_active() const { return gw_->active_query_.has_value(); }
    int         pending() const { return gw_->pending_queries_; }
    void        set_timeout(std::chrono::milliseconds timeout) { gw_->query_timeout_ = timeout; }
    void        arm_watchdog() { gw_->arm_query_watchdog(); }
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
