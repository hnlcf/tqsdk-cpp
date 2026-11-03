#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/ipc/gateway_server.h"
#include "tqsdk/ipc/ipc_client.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/remote_md_gateway.h"
#include "tqsdk/ipc/remote_td_gateway.h"
#include "tqsdk/ipc/uds_channel.h"

#include "support/test_helpers.h"

namespace tqsdk {
namespace ipc {
namespace {

// gw 侧（生产端）+ node 侧（消费端）的真实拓扑集成测试：
//   gw  : IpcSession(Gateway) + GatewayIpcServer + SimTdGateway
//   node: IpcSession(Node)    + IpcClient        + RemoteTdGateway
// 走真实 UDS + 真实帧编解码，验证命令往返 + 成交 diff 上推。

std::string make_uds_path(const char* tag) {
    static std::atomic<int> c{0};
    return "/tmp/tqsdk_gw_test_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(c.fetch_add(1)) + ".sock";
}

struct Pair {
    std::unique_ptr<IIpcChannel> srv;
    std::unique_ptr<IIpcChannel> cli;
};
Pair make_uds_pair(asio::io_context& io, const std::string& path) {
    ::unlink(path.c_str());
    asio::local::stream_protocol::acceptor acc(io, {path});
    std::unique_ptr<IIpcChannel>           srv;
    auto accept = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto sock = co_await acc.async_accept(asio::use_awaitable);
        srv       = std::make_unique<UdsChannel>(std::move(sock));
    });
    auto cli    = UdsChannel::connect(io, path);
    io.restart();
    io.run();
    accept.get();
    return {std::move(srv), std::move(cli)};
}

IpcSession::Options opt(IpcRole role, const char* name) {
    IpcSession::Options o;
    o.local_role         = role;
    o.local_name         = name;
    o.handshake_timeout  = std::chrono::seconds(2);
    o.heartbeat_interval = std::chrono::seconds(60);
    o.heartbeat_timeout  = std::chrono::seconds(120);
    return o;
}

ContractSpec rb_spec() {
    ContractSpec s;
    s.volume_multiple    = 10.0;
    s.price_tick         = 1.0;
    s.margin_rate        = 0.1;
    s.commission_per_lot = 2.0;
    return s;
}

InsertOrderReq market_buy(const char* sym, uint64_t vol) {
    InsertOrderReq r;
    r.instrument_id = sym;
    r.direction     = Direction::BUY;
    r.offset        = Offset::OPEN;
    r.price_type    = PriceType::ANY;
    r.volume        = vol;
    return r;
}

// ---------------------------------------------------------------------------
// 1) 下单：node --Command--> gw(SimTd) 回 CommandAck(success)；
//    成交 diff 以 TradePush 回到 node 的 RemoteTdGateway.event_output()。
// ---------------------------------------------------------------------------
TEST(GatewayIpcServerTest, InsertOrderRoundtripAndFill) {
    const std::string path = make_uds_path("order");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    // ---- gw 侧：先握手，再交给 GatewayIpcServer 自持 ----
    SimTdGateway sim(io.get_executor(), 1'000'000.0);
    sim.set_contract_spec("SHFE.rb2610", rb_spec());
    sim.on_tick("SHFE.rb2610", 3800.0);

    auto gw_sess = std::make_unique<IpcSession>(
        std::move(pair.srv), io.get_executor(), opt(IpcRole::Gateway, "gw"));
    auto gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw_sess->start(); });

    // ---- node 侧 ----
    auto node_sess = std::make_unique<IpcSession>(
        std::move(pair.cli), io.get_executor(), opt(IpcRole::Node, "node"));
    auto node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess->start(); });

    test::wait_all(io, std::chrono::seconds(2), gw_start, node_start);
    gw_start.get();
    node_start.get();

    auto gw_server =
        std::make_shared<GatewayIpcServer>(std::move(gw_sess), nullptr, &sim, io.get_executor());
    gw_server->start();

    // node：IpcClient 包住 node_sess（move 转移其所有权）。
    IpcClient       client(std::move(node_sess), io.get_executor());
    RemoteTdGateway td(client, io.get_executor(), std::chrono::milliseconds(1000));
    client.start();

    // 先挂一个成交 diff 消费者（跳过命令级 Ack 事件，直到收到 Diff）
    std::optional<TradeEvent> fill;
    auto                      consumer = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        while (true) {
            auto e = co_await td.event_output().async_receive();
            if (!e) {
                co_return;
            }
            if (e->kind == TradeEvent::Kind::Diff) {
                fill = std::move(*e);
                co_return;
            }
        }
    });

    std::optional<CommandAck> ack;
    auto                      inserter = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        ack = co_await td.insert_order(market_buy("SHFE.rb2610", 2), "rid_1");
    });

    test::wait_all(io, std::chrono::seconds(3), inserter, consumer);
    inserter.rethrow_if_error();
    consumer.rethrow_if_error();

    ASSERT_TRUE(ack.has_value());
    EXPECT_TRUE(ack->success) << "ack error: " << ack->error_msg;
    EXPECT_EQ(ack->order_id, "rid_1");

    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->kind, TradeEvent::Kind::Diff);

    client.stop();
    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(50));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 2) 撤单：下单挂住（限价未穿越）-> 撤单 -> ack success
// ---------------------------------------------------------------------------
TEST(GatewayIpcServerTest, CancelOrderRoundtrip) {
    const std::string path = make_uds_path("cancel");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    SimTdGateway sim(io.get_executor(), 1'000'000.0);
    sim.set_contract_spec("SHFE.rb2610", rb_spec());

    auto gw_sess = std::make_unique<IpcSession>(
        std::move(pair.srv), io.get_executor(), opt(IpcRole::Gateway, "gw"));
    auto gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw_sess->start(); });

    auto node_sess = std::make_unique<IpcSession>(
        std::move(pair.cli), io.get_executor(), opt(IpcRole::Node, "node"));
    auto node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess->start(); });

    test::wait_all(io, std::chrono::seconds(2), gw_start, node_start);
    gw_start.get();
    node_start.get();

    auto gw_server =
        std::make_shared<GatewayIpcServer>(std::move(gw_sess), nullptr, &sim, io.get_executor());
    gw_server->start();

    IpcClient       client(std::move(node_sess), io.get_executor());
    RemoteTdGateway td(client, io.get_executor(), std::chrono::milliseconds(1000));
    client.start();

    InsertOrderReq limit;
    limit.instrument_id = "SHFE.rb2610";
    limit.direction     = Direction::BUY;
    limit.offset        = Offset::OPEN;
    limit.price_type    = PriceType::LIMIT;
    limit.limit_price   = 3700.0; // 无现价 -> 挂住
    limit.volume        = 1;

    std::optional<CommandAck> ins_ack, cxl_ack;
    auto                      task = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        ins_ack = co_await td.insert_order(limit, "rid_open");
        CancelOrderReq c;
        c.order_id = "rid_open";
        cxl_ack    = co_await td.cancel_order(c);
    });

    test::wait_all(io, std::chrono::seconds(3), task);
    task.rethrow_if_error();

    ASSERT_TRUE(ins_ack.has_value());
    EXPECT_TRUE(ins_ack->success);
    ASSERT_TRUE(cxl_ack.has_value());
    EXPECT_TRUE(cxl_ack->success) << "cancel error: " << cxl_ack->error_msg;
    EXPECT_EQ(cxl_ack->order_id, "rid_open");

    client.stop();
    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(50));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 3) 断联退订：node 订阅行情(quote+kline)后断开，gw 应对 md 网关调用 release，
//    释放本连接的订阅集合（引用计数退订）。验证"node 断联 -> unsubscribe"语义。
// ---------------------------------------------------------------------------
class RecordingMdGateway : public MarketDataGateway {
public:
    explicit RecordingMdGateway(asio::any_io_executor ex)
        : ex_(ex),
          ch_(ex) {}

    asio::awaitable<void> start() override { co_return; }
    asio::awaitable<void> stop() override {
        ch_.close();
        co_return;
    }
    asio::awaitable<void> subscribe(const std::vector<std::string>& symbols) override {
        for (const auto& s : symbols) {
            subscribed_syms_.insert(s);
        }
        co_return;
    }
    asio::awaitable<void>
    subscribe_kline(const std::string& symbol, int64_t duration_ns, int view_width) override {
        (void)view_width;
        subscribed_klines_.emplace_back(symbol, duration_ns);
        co_return;
    }
    asio::awaitable<void>
    release(const std::vector<std::string>&                     symbols,
            const std::vector<std::pair<std::string, int64_t>>& klines) override {
        released_syms_   = symbols;
        released_klines_ = klines;
        released_        = true;
        ++release_calls_;
        co_return;
    }
    Channel<MarketDataEvent>& output() override { return ch_; }

    std::set<std::string>                        subscribed_syms_;
    std::vector<std::pair<std::string, int64_t>> subscribed_klines_;
    bool                                         released_      = false;
    int                                          release_calls_ = 0;
    std::vector<std::string>                     released_syms_;
    std::vector<std::pair<std::string, int64_t>> released_klines_;

private:
    asio::any_io_executor    ex_;
    Channel<MarketDataEvent> ch_;
};

TEST(GatewayIpcServerTest, DisconnectReleasesSubscriptions) {
    const std::string path = make_uds_path("release");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    RecordingMdGateway md(io.get_executor());

    auto gw_sess = std::make_unique<IpcSession>(
        std::move(pair.srv), io.get_executor(), opt(IpcRole::Gateway, "gw"));
    auto gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw_sess->start(); });

    auto node_sess = std::make_unique<IpcSession>(
        std::move(pair.cli), io.get_executor(), opt(IpcRole::Node, "node"));
    auto node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess->start(); });

    test::wait_all(io, std::chrono::seconds(2), gw_start, node_start);
    gw_start.get();
    node_start.get();

    auto gw_server =
        std::make_shared<GatewayIpcServer>(std::move(gw_sess), &md, nullptr, io.get_executor());
    gw_server->start();

    IpcClient       client(std::move(node_sess), io.get_executor());
    RemoteMdGateway rmd(client, io.get_executor());
    client.start();

    // node 发订阅：quote(SHFE.rb2610) + kline(SHFE.rb2610, 1h)
    auto sub_task = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await rmd.subscribe({"SHFE.rb2610"});
        co_await rmd.subscribe_kline("SHFE.rb2610", 3'600'000'000'000LL, 200);
    });
    test::wait_all(io, std::chrono::seconds(2), sub_task);
    sub_task.rethrow_if_error();
    test::drive_for(io, std::chrono::milliseconds(50));

    // 订阅应已下发到 mock md
    EXPECT_TRUE(md.subscribed_syms_.count("SHFE.rb2610") == 1);
    ASSERT_EQ(md.subscribed_klines_.size(), 1u);
    EXPECT_EQ(md.subscribed_klines_[0].first, "SHFE.rb2610");
    EXPECT_EQ(md.subscribed_klines_[0].second, 3'600'000'000'000LL);

    // 断联：关闭 gw server 应触发 release
    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(100));

    EXPECT_TRUE(md.released_) << "gw should release subscriptions on disconnect";
    EXPECT_EQ(md.released_syms_.size(), 1u);
    if (!md.released_syms_.empty()) {
        EXPECT_EQ(md.released_syms_[0], "SHFE.rb2610");
    }
    ASSERT_EQ(md.released_klines_.size(), 1u);
    if (!md.released_klines_.empty()) {
        EXPECT_EQ(md.released_klines_[0].first, "SHFE.rb2610");
        EXPECT_EQ(md.released_klines_[0].second, 3'600'000'000'000LL);
    }

    client.stop();
    test::drive_for(io, std::chrono::milliseconds(50));
    ::unlink(path.c_str());
}

// SubscribeBody.release=true：显式退订行情不应误伤 kline 依赖的合约行情，
// K 线退订后 symbol 使用计数归零才真正 release；重复 kline 订阅不重复计数。
TEST(GatewayIpcServerTest, ReleaseFrameUpdatesConnectionRefCounts) {
    const std::string path = make_uds_path("release_frame");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    RecordingMdGateway md(io.get_executor());

    auto gw_sess = std::make_unique<IpcSession>(
        std::move(pair.srv), io.get_executor(), opt(IpcRole::Gateway, "gw"));
    auto gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw_sess->start(); });

    auto node_sess = std::make_unique<IpcSession>(
        std::move(pair.cli), io.get_executor(), opt(IpcRole::Node, "node"));
    auto node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess->start(); });

    test::wait_all(io, std::chrono::seconds(2), gw_start, node_start);
    gw_start.get();
    node_start.get();

    auto gw_server =
        std::make_shared<GatewayIpcServer>(std::move(gw_sess), &md, nullptr, io.get_executor());
    gw_server->start();

    IpcClient       client(std::move(node_sess), io.get_executor());
    RemoteMdGateway rmd(client, io.get_executor());
    client.start();

    constexpr int64_t kDur = 3'600'000'000'000LL;

    auto sub_task = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await rmd.subscribe({"SHFE.rb2610"});
        co_await rmd.subscribe_kline("SHFE.rb2610", kDur, 200);
        co_await rmd.subscribe_kline("SHFE.rb2610", kDur, 200); // 重复：应被去重
    });
    test::wait_all(io, std::chrono::seconds(2), sub_task);
    sub_task.rethrow_if_error();
    test::drive_for(io, std::chrono::milliseconds(50));

    EXPECT_EQ(md.subscribed_syms_.count("SHFE.rb2610"), 1u);
    EXPECT_EQ(md.subscribed_klines_.size(), 1u);
    EXPECT_EQ(md.release_calls_, 0);

    // 只退订显式行情：kline 仍依赖该 symbol，不能 release。
    auto rel_quote = test::spawn_joined(
        io, [&]() -> asio::awaitable<void> { co_await rmd.release({"SHFE.rb2610"}, {}); });
    test::wait_all(io, std::chrono::seconds(2), rel_quote);
    rel_quote.rethrow_if_error();
    test::drive_for(io, std::chrono::milliseconds(50));
    EXPECT_EQ(md.release_calls_, 0) << "symbol still used by kline subscription";

    // 退订 kline：依赖计数归零，symbol + kline 一起释放。
    auto rel_kline = test::spawn_joined(
        io, [&]() -> asio::awaitable<void> { co_await rmd.release({}, {{"SHFE.rb2610", kDur}}); });
    test::wait_all(io, std::chrono::seconds(2), rel_kline);
    rel_kline.rethrow_if_error();
    test::drive_until(io, [&]() { return md.release_calls_ == 1; }, std::chrono::seconds(1));

    EXPECT_EQ(md.release_calls_, 1);
    ASSERT_EQ(md.released_syms_.size(), 1u);
    EXPECT_EQ(md.released_syms_[0], "SHFE.rb2610");
    ASSERT_EQ(md.released_klines_.size(), 1u);
    EXPECT_EQ(md.released_klines_[0].first, "SHFE.rb2610");
    EXPECT_EQ(md.released_klines_[0].second, kDur);

    // 已全部退订，断联不应再次 release。
    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(100));
    EXPECT_EQ(md.release_calls_, 1);

    client.stop();
    test::drive_for(io, std::chrono::milliseconds(50));
    ::unlink(path.c_str());
}

} // namespace
} // namespace ipc
} // namespace tqsdk
