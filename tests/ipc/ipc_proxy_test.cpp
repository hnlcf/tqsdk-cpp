#include "tqsdk/pch.h"

#include "tqsdk/ipc/ipc_client.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/remote_md_gateway.h"
#include "tqsdk/ipc/remote_td_gateway.h"
#include "tqsdk/ipc/uds_channel.h"

#include "support/test_helpers.h"

namespace tqsdk {
namespace ipc {
namespace {

// 代理端到端测试：一端扮演 node/core（裸 IpcSession + 手写应答），
// 另一端用 IpcClient + RemoteMdGateway / RemoteTdGateway 扮演 gw 侧。
//
// 同 ipc_session_test：后台常驻协程会让 io_context 永不排空，禁止无参 io.run()，
// 统一用 test::drive_until / wait_all 分片驱动。

std::string make_uds_path(const char* tag) {
    static std::atomic<int> c{0};
    return "/tmp/tqsdk_ipc_proxy_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(c.fetch_add(1)) + ".sock";
}

struct UdsPair {
    std::unique_ptr<IIpcChannel> srv; // node 侧
    std::unique_ptr<IIpcChannel> cli; // gw 侧
};
UdsPair make_uds_pair(asio::io_context& io, const std::string& path) {
    ::unlink(path.c_str());
    asio::local::stream_protocol::acceptor acc(io, {path});

    std::unique_ptr<IIpcChannel> srv;
    auto                         accept = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto sock = co_await acc.async_accept(asio::use_awaitable);
        srv       = std::make_unique<UdsChannel>(std::move(sock));
    });
    auto                         cli    = UdsChannel::connect(io, path);

    io.restart();
    io.run();
    accept.get();
    return {std::move(srv), std::move(cli)};
}

IpcSession::Options node_opt() {
    IpcSession::Options o;
    o.local_role         = IpcRole::Node;
    o.local_name         = "node";
    o.handshake_timeout  = std::chrono::seconds(2);
    o.heartbeat_interval = std::chrono::seconds(60);
    o.heartbeat_timeout  = std::chrono::seconds(120);
    return o;
}
IpcSession::Options gw_opt() {
    IpcSession::Options o = node_opt();
    o.local_role          = IpcRole::Gateway;
    o.local_name          = "gw";
    return o;
}

// gw 侧完整栈：session(cli) + client + md/td 代理。
// 代理在构造时向 client 注册 handler，故必须在 client.start() 前构造。
struct GwSide {
    std::unique_ptr<IpcSession>      session;
    std::unique_ptr<IpcClient>       client;
    std::unique_ptr<RemoteMdGateway> md;
    std::unique_ptr<RemoteTdGateway> td;

    void stop() {
        if (client) {
            client->stop();
        }
    }
};

GwSide build_gw_side(asio::io_context& io, std::unique_ptr<IIpcChannel> cli_ch) {
    GwSide g;
    g.session = std::make_unique<IpcSession>(std::move(cli_ch), io.get_executor(), gw_opt());
    // 注意：client 持有 session 的所有权；这里先不 move，握完手再由 finish 构造 client。
    return g;
}

// 在握手成功后组装 client + 代理并启动。
void finish_gw_side(GwSide& g, asio::io_context& io) {
    g.client = std::make_unique<IpcClient>(std::move(g.session), io.get_executor());
    g.md     = std::make_unique<RemoteMdGateway>(*g.client, io.get_executor());
    g.td     = std::make_unique<RemoteTdGateway>(
        *g.client, io.get_executor(), std::chrono::milliseconds(400));
    g.client->start();
}

// ---------------------------------------------------------------------------
// 1) 订阅：gw 代理 subscribe() -> node 收到 Subscribe -> 回 SubscribeAck
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, SubscribeReachesNode) {
    std::string      path = make_uds_path("sub");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    auto gw_session =
        std::make_unique<IpcSession>(std::move(pair.cli), io.get_executor(), gw_opt());
    auto gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw_session->start(); });

    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();

    // node 侧读循环：收到 Subscribe 回 SubscribeAck，并记录
    std::optional<SubscribeBody> got_sub;
    auto node_reader = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto opt = co_await node_sess.incoming().async_receive();
        if (!opt) {
            co_return;
        }
        got_sub = decode_as<FrameType::Subscribe>(*opt);
        co_await node_sess.send_body<FrameType::SubscribeAck>(SubscribeAckBody{true, ""});
    });

    IpcClient       client(std::move(gw_session), io.get_executor());
    RemoteMdGateway md(client, io.get_executor());
    client.start();

    auto sub = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await md.subscribe({"SHFE.rb2610", "DCE.i2601"});
    });

    test::wait_all(io, std::chrono::seconds(2), sub, node_reader);
    sub.rethrow_if_error();
    node_reader.rethrow_if_error();

    ASSERT_TRUE(got_sub.has_value());
    ASSERT_EQ(got_sub->symbols.size(), 2u);
    EXPECT_EQ(got_sub->symbols[0], "SHFE.rb2610");
    EXPECT_EQ(got_sub->symbols[1], "DCE.i2601");

    client.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 2) 行情推送：node 推 MarketDataPush / Kline -> 代理 output() 收到正确 Kind
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, MarketDataPushDelivered) {
    std::string      path = make_uds_path("md");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    GwSide g = build_gw_side(io, std::move(pair.cli));
    auto   gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await g.session->start(); });

    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();
    finish_gw_side(g, io);

    // node 推：一条 diff + 一条 kline
    auto node_pusher = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        MarketDataPushBody d;
        d.kind = MdPushKind::Diff;
        d.data = {{"quotes", {{"SHFE.rb2610", {{"last_price", 3811.0}}}}}};
        co_await node_sess.send_body<FrameType::MarketDataPush>(d);

        MarketDataPushBody k;
        k.kind = MdPushKind::Kline;
        k.data = {{"klines", {{"SHFE.rb2610", {{"last_bar", {{"close", 3812.0}}}}}}}};
        co_await node_sess.send_body<FrameType::MarketDataPush>(k);
    });

    // 代理侧消费两条事件
    std::vector<MarketDataEvent> evs;
    auto                         consumer = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        for (int i = 0; i < 2; ++i) {
            auto e = co_await g.md->output().async_receive();
            if (!e) {
                co_return;
            }
            evs.push_back(std::move(*e));
        }
    });

    test::wait_all(io, std::chrono::seconds(2), node_pusher, consumer);
    node_pusher.rethrow_if_error();
    consumer.rethrow_if_error();

    ASSERT_EQ(evs.size(), 2u);
    EXPECT_EQ(evs[0].kind, MarketDataEvent::Kind::Diff);
    EXPECT_DOUBLE_EQ(evs[0].payload["quotes"]["SHFE.rb2610"]["last_price"].get<double>(), 3811.0);
    EXPECT_EQ(evs[1].kind, MarketDataEvent::Kind::Kline);
    EXPECT_DOUBLE_EQ(evs[1].payload["klines"]["SHFE.rb2610"]["last_bar"]["close"].get<double>(),
                     3812.0);

    g.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 3) 下单：代理 insert_order -> node 收 Command 回 CommandAck -> 代理拿到 ack
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, InsertOrderRoundtrip) {
    std::string      path = make_uds_path("cmd");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    GwSide g = build_gw_side(io, std::move(pair.cli));
    auto   gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await g.session->start(); });

    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();
    finish_gw_side(g, io);

    // node 端：收到 Command 校验字段，回成功 CommandAck
    std::optional<CommandBody> got_cmd;
    auto node_responder = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto opt = co_await node_sess.incoming().async_receive();
        if (!opt) {
            co_return;
        }
        got_cmd = decode_as<FrameType::Command>(*opt);
        CommandAck ack{true, got_cmd->rid, ""};
        co_await node_sess.send_body<FrameType::CommandAck>(CommandAckBody{ack});
    });

    InsertOrderReq req;
    req.instrument_id = "SHFE.rb2610";
    req.exchange_id   = "SHFE";
    req.direction     = Direction::BUY;
    req.offset        = Offset::OPEN;
    req.price_type    = PriceType::LIMIT;
    req.limit_price   = 3800.5;
    req.volume        = 2;

    std::optional<CommandAck> ack_out;
    auto                      inserter = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        ack_out = co_await g.td->insert_order(req, "rid_1");
    });

    test::wait_all(io, std::chrono::seconds(2), node_responder, inserter);
    node_responder.rethrow_if_error();
    inserter.rethrow_if_error();

    ASSERT_TRUE(got_cmd.has_value());
    EXPECT_EQ(got_cmd->op, CommandOp::InsertOrder);
    EXPECT_EQ(got_cmd->rid, "rid_1");
    EXPECT_EQ(got_cmd->insert.instrument_id, "SHFE.rb2610");
    EXPECT_EQ(got_cmd->insert.direction, Direction::BUY);
    EXPECT_DOUBLE_EQ(got_cmd->insert.limit_price, 3800.5);
    EXPECT_EQ(got_cmd->insert.volume, 2u);

    ASSERT_TRUE(ack_out.has_value());
    EXPECT_TRUE(ack_out->success);
    EXPECT_EQ(ack_out->order_id, "rid_1");

    g.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 4) 撤单：代理 cancel_order -> node 收 Command(CancelOrder) 回 ack
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, CancelOrderRoundtrip) {
    std::string      path = make_uds_path("cancel");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    GwSide g = build_gw_side(io, std::move(pair.cli));
    auto   gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await g.session->start(); });
    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();
    finish_gw_side(g, io);

    std::optional<CommandBody> got_cmd;
    auto node_responder = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto opt = co_await node_sess.incoming().async_receive();
        if (!opt) {
            co_return;
        }
        got_cmd = decode_as<FrameType::Command>(*opt);
        co_await node_sess.send_body<FrameType::CommandAck>(
            CommandAckBody{CommandAck{true, got_cmd->cancel.order_id, ""}});
    });

    std::optional<CommandAck> ack_out;
    auto                      canceler = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        ack_out = co_await g.td->cancel_order(CancelOrderReq{"rid_9", "ex_9"});
    });

    test::wait_all(io, std::chrono::seconds(2), node_responder, canceler);
    node_responder.rethrow_if_error();
    canceler.rethrow_if_error();

    ASSERT_TRUE(got_cmd.has_value());
    EXPECT_EQ(got_cmd->op, CommandOp::CancelOrder);
    EXPECT_EQ(got_cmd->cancel.order_id, "rid_9");
    ASSERT_TRUE(ack_out.has_value());
    EXPECT_EQ(ack_out->order_id, "rid_9");

    g.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 5) 网关事件：node 推 GatewayEvent(Disconnected) -> 代理收到 Disconnected
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, GatewayEventDelivered) {
    std::string      path = make_uds_path("evt");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    GwSide g = build_gw_side(io, std::move(pair.cli));
    auto   gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await g.session->start(); });
    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();
    finish_gw_side(g, io);

    auto node_pusher = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await node_sess.send_body<FrameType::GatewayEvent>(
            GatewayEventBody{GatewayEventKind::Disconnected, "server closed"});
    });

    std::optional<MarketDataEvent> md_ev;
    auto                           consumer = test::spawn_joined(
        io, [&]() -> asio::awaitable<void> { md_ev = co_await g.md->output().async_receive(); });

    test::wait_all(io, std::chrono::seconds(2), node_pusher, consumer);
    node_pusher.rethrow_if_error();
    consumer.rethrow_if_error();

    ASSERT_TRUE(md_ev.has_value());
    EXPECT_EQ(md_ev->kind, MarketDataEvent::Kind::Disconnected);

    g.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 6) 命令 ack 超时：node 不回 ack -> 代理 insert_order 抛 std::runtime_error
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, CommandAckTimeout) {
    std::string      path = make_uds_path("tmo");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    GwSide g = build_gw_side(io, std::move(pair.cli));
    auto   gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await g.session->start(); });
    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();
    finish_gw_side(g, io); // td ack_timeout = 400ms

    // node 端读走 Command 但故意不回 ack
    auto node_reader = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto opt = co_await node_sess.incoming().async_receive();
        (void)opt;
    });

    bool timed_out = false;
    auto inserter  = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        InsertOrderReq req;
        req.instrument_id = "SHFE.rb2610";
        req.direction     = Direction::BUY;
        req.offset        = Offset::OPEN;
        req.price_type    = PriceType::LIMIT;
        req.limit_price   = 3800.0;
        req.volume        = 1;
        const auto ack    = co_await g.td->insert_order(req, "rid_tmo");
        timed_out         = !ack.success;
    });

    test::wait_all(io, std::chrono::seconds(2), node_reader, inserter);
    node_reader.rethrow_if_error();
    inserter.rethrow_if_error();

    EXPECT_TRUE(timed_out) << "expected insert_order to time out without ack";

    g.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 7) 并发多命令 + 乱序回 ack：按 rid 关联，结果各自正确
// ---------------------------------------------------------------------------
TEST(IpcProxyTest, ConcurrentCommandsMatchedByRid) {
    std::string      path = make_uds_path("conc");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession node_sess(std::move(pair.srv), io.get_executor(), node_opt());
    auto       node_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node_sess.start(); });

    GwSide g = build_gw_side(io, std::move(pair.cli));
    auto   gw_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await g.session->start(); });
    test::wait_all(io, std::chrono::seconds(2), node_start, gw_start);
    node_start.get();
    gw_start.get();
    finish_gw_side(g, io);

    // node 端：收集 N 条命令，然后**乱序**回 ack（用 order_id 反映回执）
    constexpr int N              = 5;
    auto          node_responder = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        std::vector<OrderId> rids;
        for (int i = 0; i < N; ++i) {
            auto opt = co_await node_sess.incoming().async_receive();
            if (!opt) {
                co_return;
            }
            rids.push_back(decode_as<FrameType::Command>(*opt).rid);
        }
        // 逆序回 ack，验证按 rid 关联而非到达顺序
        for (auto it = rids.rbegin(); it != rids.rend(); ++it) {
            co_await node_sess.send_body<FrameType::CommandAck>(
                CommandAckBody{CommandAck{true, *it, ""}});
        }
    });

    // gw 端：并发发起 N 条命令，各自记录拿到的 order_id
    std::map<std::string, std::string> got; // rid -> ack.order_id
    // 用一个协程并发发起并收集，避免多个 JoinedSpawn 管理麻烦
    auto client_task = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        // 依次发但并发等待（用 co_spawn 到同一 executor）
        std::atomic<int> pending{N};
        for (int i = 0; i < N; ++i) {
            std::string rid = "rid_" + std::to_string(i);
            co_spawn_logged(
                io.get_executor(),
                [&, rid]() -> asio::awaitable<void> {
                    InsertOrderReq req;
                    req.instrument_id = "SHFE.rb2610";
                    req.direction     = Direction::BUY;
                    req.offset        = Offset::OPEN;
                    req.price_type    = PriceType::LIMIT;
                    req.limit_price   = 3800.0 + std::stoi(rid.substr(4));
                    req.volume        = 1;
                    auto ack          = co_await g.td->insert_order(req, rid);
                    got[rid]          = ack.order_id;
                    pending.fetch_sub(1);
                },
                "conc.insert");
        }
        // 等待全部完成
        while (pending.load() > 0) {
            co_await asio::steady_timer(co_await asio::this_coro::executor,
                                        std::chrono::milliseconds(5))
                .async_wait(asio::use_awaitable);
        }
    });

    test::wait_all(io, std::chrono::seconds(3), node_responder, client_task);
    node_responder.rethrow_if_error();
    client_task.rethrow_if_error();

    ASSERT_EQ(got.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        std::string rid = "rid_" + std::to_string(i);
        EXPECT_EQ(got[rid], rid) << "rid/ack mismatch for " << rid;
    }

    g.stop();
    node_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

} // namespace
} // namespace ipc
} // namespace tqsdk
