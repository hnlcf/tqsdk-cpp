#include "tqsdk/pch.h"

#include "tqsdk/tqapi.h"

namespace asio = boost::asio;

namespace {

/// 模拟 IPC ACK 超时/transport 断开：网关直接抛异常。
class ThrowingTdGateway final : public tqsdk::TradingGateway {
public:
    explicit ThrowingTdGateway(asio::any_io_executor ex)
        : ch_(ex) {}

    asio::awaitable<void> start() override { co_return; }
    asio::awaitable<void> stop() override {
        ch_.close();
        co_return;
    }
    asio::awaitable<tqsdk::CommandAck> insert_order(const tqsdk::InsertOrderReq&,
                                                    const tqsdk::OrderId&) override {
        throw std::runtime_error("simulated ack timeout");
    }
    asio::awaitable<tqsdk::CommandAck> cancel_order(const tqsdk::CancelOrderReq&) override {
        throw std::runtime_error("simulated cancel transport failure");
    }
    tqsdk::Channel<tqsdk::TradeEvent>& event_output() override { return ch_; }

private:
    tqsdk::Channel<tqsdk::TradeEvent> ch_;
};

tqsdk::InsertOrderReq make_request() {
    tqsdk::InsertOrderReq req;
    req.instrument_id = "SHFE.rb2701";
    req.direction     = tqsdk::Direction::BUY;
    req.offset        = tqsdk::Offset::OPEN;
    req.price_type    = tqsdk::PriceType::LIMIT;
    req.limit_price   = 3000.0;
    req.volume        = 1;
    return req;
}

} // namespace

TEST(TqApiGatewayFault, InsertExceptionBecomesLocalErrorOrder) {
    asio::io_context io;
    auto             ex = asio::make_strand(io);
    auto             td = std::make_unique<ThrowingTdGateway>(ex);
    tqsdk::TqApi     api(ex, nullptr, std::move(td));
    api.start();

    std::string rid;
    bool        done = false;
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            auto* order = co_await api.insert_order(make_request());
            rid         = order->order_id;
            done        = true;
            co_return;
        },
        asio::detached);
    io.run_for(std::chrono::milliseconds(50));

    ASSERT_TRUE(done);
    auto* order = api.get_order(rid);
    ASSERT_NE(order, nullptr);
    EXPECT_TRUE(order->is_error);
    EXPECT_TRUE(order->is_dead);
    EXPECT_EQ(order->last_msg, "trading gateway error: simulated ack timeout");

    api.close();
    io.restart();
    io.run_for(std::chrono::milliseconds(50));
}

TEST(TqApiGatewayFault, CancelExceptionReturnsRejectedAck) {
    asio::io_context io;
    auto             ex = asio::make_strand(io);
    auto             td = std::make_unique<ThrowingTdGateway>(ex);
    tqsdk::TqApi     api(ex, nullptr, std::move(td));
    api.start();

    tqsdk::CommandAck ack;
    bool              done = false;
    asio::co_spawn(
        ex,
        [&]() -> asio::awaitable<void> {
            ack = co_await api.cancel_order(
                tqsdk::CancelOrderReq{.order_id = "rid_cancel", .exchange_order_id = {}});
            done = true;
            co_return;
        },
        asio::detached);
    io.run_for(std::chrono::milliseconds(50));

    ASSERT_TRUE(done);
    EXPECT_FALSE(ack.success);
    EXPECT_EQ(ack.error_msg, "trading gateway error: simulated cancel transport failure");

    api.close();
    io.restart();
    io.run_for(std::chrono::milliseconds(50));
}
