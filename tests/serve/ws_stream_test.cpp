#include "tqsdk/pch.h"

#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/gateway/mock/mock_md_gateway.h"
#include "tqsdk/serve/http_server.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;
using json      = nlohmann::json;
namespace beast = boost::beast;

namespace {

namespace beast_ws = boost::beast::websocket;

/// 极简 WebSocket 测试客户端：连接 / 收发文本 JSON / 关闭。
class WsClient {
public:
    explicit WsClient(asio::io_context& ctx)
        : ws_(ctx) {}

    asio::awaitable<void> connect(std::string host, uint16_t port, std::string path) {
        asio::ip::tcp::endpoint ep{asio::ip::make_address(host), port};
        co_await ws_.next_layer().async_connect(ep, asio::use_awaitable);
        co_await ws_.async_handshake(host, path, asio::use_awaitable);
    }

    asio::awaitable<json> read_json() {
        buffer_.clear();
        co_await ws_.async_read(buffer_, asio::use_awaitable);
        std::string s = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        co_return json::parse(s);
    }

    // 按值接收，协程帧持有消息内容，避免调用方临时对象在挂起期间失效。
    asio::awaitable<void> send_json(json j) {
        std::string body = j.dump();
        co_await ws_.async_write(asio::buffer(body), asio::use_awaitable);
    }

    asio::awaitable<void> close() {
        boost::system::error_code ec;
        co_await ws_.async_close(beast_ws::close_reason(beast_ws::close_code::normal),
                                 asio::redirect_error(asio::use_awaitable, ec));
    }

private:
    beast_ws::stream<asio::ip::tcp::socket> ws_;
    beast::flat_buffer                      buffer_;
};

/// 起一个真实 HTTP 监听 + mock 行情网关 + TqApi；测试结束统一收敛后台协程。
struct WsFixture {
    asio::io_context                   ctx;
    MockMdGateway*                     md = nullptr;
    std::unique_ptr<TqApi>             api;
    std::unique_ptr<serve::HttpServer> http;
    uint16_t                           port = 0;

    explicit WsFixture(serve::WsOptions opts = {}) {
        auto md_owner = std::make_unique<MockMdGateway>(ctx.get_executor());
        md            = md_owner.get();
        api           = std::make_unique<TqApi>(ctx.get_executor(), std::move(md_owner), nullptr);
        api->start();

        http = std::make_unique<serve::HttpServer>(api.get(), ctx.get_executor(), std::move(opts));
        EXPECT_TRUE(http->listen("127.0.0.1", 0));
        port = http->port();
    }

    ~WsFixture() {
        if (http) {
            http->stop();
        }
        if (api) {
            api->close();
        }
        // 驱动 close 触发的回调：hub push_loop / md_loop / 客户端 reader/writer。
        test::drive_for(ctx, std::chrono::milliseconds(150));
        http.reset();
        api.reset();
        test::drive_for(ctx, std::chrono::milliseconds(50));
    }
};

json quote_diff(std::string_view sym, json fields) {
    return json{{"quotes", {{std::string(sym), std::move(fields)}}}};
}

json kline_diff(std::string_view sym, int64_t dur, json bars) {
    return json{{"klines", {{std::string(sym), {{std::to_string(dur), std::move(bars)}}}}}};
}

} // namespace

// 订阅 -> welcome/subscribed/snapshot -> 注入行情 -> 收到只含变化字段的 diff。
TEST(WsStream, QuoteSnapshotThenPartialDiff) {
    WsFixture f;

    int      welcome_protocol = 0;
    bool     subscribed_ok    = false;
    bool     snapshot_null    = false;
    bool     has_symbol       = false;
    double   diff_last        = 0.0;
    uint64_t diff_volume      = 0;
    bool     only_changed     = false;
    uint64_t seq              = 0;

    test::run_coro(f.ctx, [&]() -> asio::awaitable<void> {
        WsClient c(f.ctx);
        co_await c.connect("127.0.0.1", f.port, "/stream");

        json welcome     = co_await c.read_json();
        welcome_protocol = welcome.value("protocol", 0);

        co_await c.send_json(
            json{{"op", "subscribe"}, {"symbols", {"SHFE.rb2610"}}, {"topics", json::array()}});
        json sub      = co_await c.read_json();
        subscribed_ok = sub.value("type", "") == "subscribed";

        json snapshot = co_await c.read_json();
        has_symbol    = snapshot["data"]["quotes"].contains("SHFE.rb2610");
        snapshot_null =
            has_symbol && snapshot["data"]["quotes"]["SHFE.rb2610"]["last_price"].is_null();

        f.md->inject(quote_diff("SHFE.rb2610", {{"last_price", 3800.0}, {"volume", 42}}));

        json diff    = co_await c.read_json();
        seq          = diff.value("seq", uint64_t{0});
        auto& q      = diff["data"]["quotes"]["SHFE.rb2610"];
        diff_last    = q.value("last_price", 0.0);
        diff_volume  = q.value("volume", uint64_t{0});
        only_changed = !q.contains("open") && !q.contains("pre_close");

        co_await c.close();
    }());

    EXPECT_EQ(welcome_protocol, 1);
    EXPECT_TRUE(subscribed_ok);
    EXPECT_TRUE(has_symbol);
    EXPECT_TRUE(snapshot_null) << "empty quote placeholder should serialize NaN as null";
    EXPECT_GT(seq, 0u);
    EXPECT_DOUBLE_EQ(diff_last, 3800.0);
    EXPECT_EQ(diff_volume, 42u);
    EXPECT_TRUE(only_changed) << "quote diff must contain only changed fields";
}

// K 线：订阅后空快照；历史两根触发 reset=true；单根更新只推最后一根。
TEST(WsStream, KlineResetThenLastBarDiff) {
    WsFixture         f;
    constexpr int64_t kDur = 60'000'000'000LL;

    bool   subscribed_kline    = false;
    bool   snapshot_empty      = false;
    bool   upstream_subscribed = false;
    bool   reset_first         = false;
    size_t first_bars          = 0;
    bool   reset_second        = false;
    size_t second_bars         = 0;
    double second_close        = 0.0;

    test::run_coro(f.ctx, [&]() -> asio::awaitable<void> {
        WsClient c(f.ctx);
        co_await c.connect("127.0.0.1", f.port, "/stream");
        (void)co_await c.read_json(); // welcome

        co_await c.send_json(json{
            {"op", "subscribe"},
            {"klines", {{{"symbol", "SHFE.rb2610"}, {"duration_ns", kDur}, {"view_width", 50}}}},
            {"topics", json::array()}});
        json sub         = co_await c.read_json(); // subscribed
        subscribed_kline = sub.value("type", std::string{}) == "subscribed";

        json  snapshot      = co_await c.read_json();
        auto& sk            = snapshot["data"]["klines"]["SHFE.rb2610"][std::to_string(kDur)];
        snapshot_empty      = sk["bars"].is_array() && sk["bars"].empty();
        upstream_subscribed = f.md->is_kline_subscribed("SHFE.rb2610", kDur);

        json bars = json::array({
            {{"datetime", 1},
             {"open", 10.0},
             {"high", 12.0},
             {"low", 9.0},
             {"close", 11.0},
             {"volume", 100},
             {"open_oi", 5},
             {"close_oi", 6}},
            {{"datetime", 2},
             {"open", 11.0},
             {"high", 13.0},
             {"low", 10.0},
             {"close", 12.0},
             {"volume", 200},
             {"open_oi", 6},
             {"close_oi", 7}},
        });
        f.md->inject(kline_diff("SHFE.rb2610", kDur, bars));

        json  diff1 = co_await c.read_json();
        auto& k1    = diff1["data"]["klines"]["SHFE.rb2610"][std::to_string(kDur)];
        reset_first = k1.value("reset", false);
        first_bars  = k1["bars"].size();

        json single = json::array({{{"datetime", 2},
                                    {"open", 11.0},
                                    {"high", 14.0},
                                    {"low", 10.0},
                                    {"close", 13.5},
                                    {"volume", 250},
                                    {"open_oi", 6},
                                    {"close_oi", 8}}});
        f.md->inject(kline_diff("SHFE.rb2610", kDur, single));

        json  diff2  = co_await c.read_json();
        auto& k2     = diff2["data"]["klines"]["SHFE.rb2610"][std::to_string(kDur)];
        reset_second = k2.value("reset", true);
        second_bars  = k2["bars"].size();
        second_close = k2["bars"][0].value("close", 0.0);

        co_await c.close();
    }());

    EXPECT_TRUE(subscribed_kline);
    EXPECT_TRUE(snapshot_empty);
    EXPECT_TRUE(reset_first);
    EXPECT_EQ(first_bars, 2u);
    EXPECT_FALSE(reset_second);
    EXPECT_EQ(second_bars, 1u);
    EXPECT_DOUBLE_EQ(second_close, 13.5);
    EXPECT_TRUE(upstream_subscribed);

    // 客户端 close 后 reader 协程会 remove_client -> 上游 release。
    EXPECT_TRUE(test::drive_until(
        f.ctx,
        [&]() { return f.md->was_kline_released("SHFE.rb2610", kDur); },
        std::chrono::milliseconds(500)));
}

// 显式退订会立即向网关 release，而不是等到连接断开。
TEST(WsStream, UnsubscribeReleasesUpstream) {
    WsFixture   f;
    std::string ack_type;

    test::run_coro(f.ctx, [&]() -> asio::awaitable<void> {
        WsClient c(f.ctx);
        co_await c.connect("127.0.0.1", f.port, "/stream");
        (void)co_await c.read_json(); // welcome
        co_await c.send_json(
            json{{"op", "subscribe"}, {"symbols", {"SHFE.rb2610"}}, {"topics", json::array()}});
        (void)co_await c.read_json(); // subscribed
        (void)co_await c.read_json(); // snapshot

        co_await c.send_json(
            json{{"op", "unsubscribe"}, {"symbols", {"SHFE.rb2610"}}, {"klines", json::array()}});
        json ack = co_await c.read_json();
        ack_type = ack.value("type", std::string{});
        co_await c.close();
    }());

    EXPECT_EQ(ack_type, "unsubscribed");
    EXPECT_TRUE(test::drive_until(
        f.ctx,
        [&]() { return f.md->was_released("SHFE.rb2610"); },
        std::chrono::milliseconds(500)));
    EXPECT_FALSE(f.md->is_subscribed("SHFE.rb2610"));
}

// 多客户端订阅同一标的：上游只 subscribe 一次；全部断开后只 release 一次。
TEST(WsStream, TwoClientsShareUpstreamSubscription) {
    WsFixture f;
    int       upstream_subscribes = 0;

    test::run_coro(f.ctx, [&]() -> asio::awaitable<void> {
        WsClient a(f.ctx);
        WsClient b(f.ctx);
        co_await a.connect("127.0.0.1", f.port, "/stream");
        co_await b.connect("127.0.0.1", f.port, "/stream");
        (void)co_await a.read_json(); // welcome
        (void)co_await b.read_json(); // welcome

        const json sub =
            json{{"op", "subscribe"}, {"symbols", {"SHFE.rb2610"}}, {"topics", json::array()}};
        co_await a.send_json(sub);
        (void)co_await a.read_json(); // subscribed
        (void)co_await a.read_json(); // snapshot

        co_await b.send_json(sub);
        (void)co_await b.read_json(); // subscribed
        (void)co_await b.read_json(); // snapshot

        co_await a.close();
        co_await b.close();
    }());

    upstream_subscribes = f.md->subscribe_events();
    EXPECT_EQ(upstream_subscribes, 1);
    EXPECT_TRUE(test::drive_until(
        f.ctx, [&]() { return f.md->release_events() == 1; }, std::chrono::milliseconds(500)));
    EXPECT_FALSE(f.md->is_subscribed("SHFE.rb2610"));
}

// 应用层 ping/pong，便于前端主动探活。
TEST(WsStream, AppLevelPingPong) {
    WsFixture f;

    std::string pong_type;
    test::run_coro(f.ctx, [&]() -> asio::awaitable<void> {
        WsClient c(f.ctx);
        co_await c.connect("127.0.0.1", f.port, "/stream");
        (void)co_await c.read_json(); // welcome
        co_await c.send_json(json{{"op", "ping"}});
        json pong = co_await c.read_json();
        pong_type = pong.value("type", std::string{});
        co_await c.close();
    }());
    EXPECT_EQ(pong_type, "pong");
}

// [serve.ws] 配置解析。
TEST(WsStreamConfig, ParseServeWsSection) {
    auto cfg = parse_node_config(R"(
        [serve]
        bind = "0.0.0.0"
        port = 9000

        [serve.ws]
        path = "/ws"
        heartbeat_interval_ms = 1500
        max_clients = 7
        max_queue = 9
        coalesce_ms = 12
    )")
                   .value();

    EXPECT_EQ(cfg.serve_bind, "0.0.0.0");
    EXPECT_EQ(cfg.serve_port, 9000);
    EXPECT_EQ(cfg.ws.path, "/ws");
    EXPECT_EQ(cfg.ws.heartbeat_interval, std::chrono::milliseconds(1500));
    EXPECT_EQ(cfg.ws.max_clients, 7u);
    EXPECT_EQ(cfg.ws.max_queue, 9u);
    EXPECT_EQ(cfg.ws.coalesce, std::chrono::milliseconds(12));
}

// 只发 symbols 不带 topics 时，服务端不应默认推送 account/orders/trades/positions。
TEST(WsStream, DefaultSubscriptionIsMarketDataOnly) {
    WsFixture f;
    bool      no_trading_topics = false;
    bool      ack_topics_empty  = false;
    test::run_coro(f.ctx, [&]() -> asio::awaitable<void> {
        WsClient c(f.ctx);
        co_await c.connect("127.0.0.1", f.port, "/stream");
        (void)co_await c.read_json(); // welcome
        co_await c.send_json(
            json{{"op", "subscribe"}, {"symbols", {"SHFE.rb2610"}}, {"klines", json::array()}});
        auto sub             = co_await c.read_json();
        ack_topics_empty     = sub.value("topics", json::array()).empty();
        auto        snapshot = co_await c.read_json();
        const auto& data     = snapshot["data"];
        no_trading_topics    = !data.contains("account") && !data.contains("orders") &&
                               !data.contains("trades") && !data.contains("positions");
        co_await c.close();
        co_return;
    }());
    EXPECT_TRUE(ack_topics_empty);
    EXPECT_TRUE(no_trading_topics);
}
