#include "tqsdk/pch.h"

#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/sim/sim_td_gateway.h"
#include "tqsdk/ipc/gateway_server.h"
#include "tqsdk/ipc/ipc_client.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/remote_md_gateway.h"
#include "tqsdk/ipc/remote_td_gateway.h"
#include "tqsdk/ipc/tcp_channel.h"
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
// 2) 命令幂等：同一 op+rid 重复到达时复用首次 ack，不重复下单
// ---------------------------------------------------------------------------
TEST(GatewayIpcServerTest, DuplicateCommandReusesCachedAck) {
    const std::string path = make_uds_path("dedup");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    SimTdGateway sim(io.get_executor(), 1'000'000.0);
    sim.set_contract_spec("SHFE.rb2610", rb_spec());
    sim.on_tick("SHFE.rb2610", 3800.0);

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

    CommandBody body;
    body.op     = CommandOp::InsertOrder;
    body.rid    = "rid_dup";
    body.insert = market_buy("SHFE.rb2610", 1);

    std::vector<CommandAck> acks;
    auto                    reader = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        // 中间可能夹着 SimTd 的 TradePush，读到两个 CommandAck 为止。
        for (int i = 0; i < 10 && acks.size() < 2; ++i) {
            auto f = co_await node_sess->incoming().async_receive();
            if (!f) {
                co_return;
            }
            if (f->type == FrameType::CommandAck) {
                acks.push_back(decode_as<FrameType::CommandAck>(*f).ack);
            }
        }
    });
    auto                    sender = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await node_sess->send_body<FrameType::Command>(body);
        co_await node_sess->send_body<FrameType::Command>(body);
    });

    test::wait_all(io, std::chrono::seconds(3), sender, reader);
    sender.rethrow_if_error();
    reader.rethrow_if_error();

    const auto snap = sim.snapshot();

    // 先完成清理再断言：避免 ASSERT 失败提前返回导致后台协程仍引用栈上的 sim。
    node_sess->close();
    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(100));
    ::unlink(path.c_str());

    ASSERT_EQ(acks.size(), 2u);
    EXPECT_TRUE(acks[0].success);
    EXPECT_TRUE(acks[1].success);
    EXPECT_EQ(acks[0].order_id, "rid_dup");
    EXPECT_EQ(acks[1].order_id, "rid_dup");
    EXPECT_EQ(snap["trade"][""]["user"]["orders"].size(), 1u);
}

// ---------------------------------------------------------------------------
// 3) 撤单：下单挂住（限价未穿越）-> 撤单 -> ack success
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

    void inject(nlohmann::json diff) {
        MarketDataEvent ev{MarketDataEvent::Kind::Diff, std::move(diff), {}};
        co_spawn_logged(
            ex_,
            [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
                co_await ch_.async_send(std::move(e));
            },
            "RecordingMd.inject");
    }

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

// md push 序号连续性 + Resume 从指定 seq replay。
TEST(GatewayIpcServerTest, SequenceNumbersAndResumeReplay) {
    const std::string path = make_uds_path("seq_resume");
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

    bool        got_ack      = false;
    bool        got_push1    = false;
    bool        got_push2    = false;
    bool        got_resume   = false;
    bool        got_replayed = false;
    uint64_t    seq1 = 0, seq2 = 0, replayed_seq = 0;
    std::string epoch1, epoch2, replayed_epoch;

    auto task = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        SubscribeBody sub;
        sub.symbols = {"SHFE.rb2610"};
        co_await node_sess->send_body<FrameType::Subscribe>(sub);

        auto ack_frame = co_await node_sess->incoming().async_receive();
        if (ack_frame.has_value()) {
            auto ack = decode_as<FrameType::SubscribeAck>(*ack_frame);
            got_ack  = ack.ok;
        }

        md.inject(nlohmann::json{{"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0}}}}}});
        md.inject(nlohmann::json{{"quotes", {{"SHFE.rb2610", {{"volume", 42}}}}}});

        auto f1 = co_await node_sess->incoming().async_receive();
        if (f1.has_value()) {
            auto b    = decode_as<FrameType::MarketDataPush>(*f1);
            got_push1 = true;
            seq1      = b.meta.seq;
            epoch1    = b.meta.epoch;
        }
        auto f2 = co_await node_sess->incoming().async_receive();
        if (f2.has_value()) {
            auto b    = decode_as<FrameType::MarketDataPush>(*f2);
            got_push2 = true;
            seq2      = b.meta.seq;
            epoch2    = b.meta.epoch;
        }

        ResumeBody resume;
        resume.epoch       = epoch1;
        resume.md_next_seq = 2;
        resume.td_next_seq = 0;
        co_await node_sess->send_body<FrameType::Resume>(resume);

        auto ra = co_await node_sess->incoming().async_receive();
        if (ra.has_value()) {
            auto ack   = decode_as<FrameType::ResumeAck>(*ra);
            got_resume = ack.md_replay && ack.epoch == epoch1;
        }

        auto rf = co_await node_sess->incoming().async_receive();
        if (rf.has_value() && rf->type == FrameType::MarketDataPush) {
            auto b         = decode_as<FrameType::MarketDataPush>(*rf);
            got_replayed   = true;
            replayed_seq   = b.meta.seq;
            replayed_epoch = b.meta.epoch;
        }
    });
    test::wait_all(io, std::chrono::seconds(3), task);
    task.rethrow_if_error();

    EXPECT_TRUE(got_ack);
    ASSERT_TRUE(got_push1);
    ASSERT_TRUE(got_push2);
    EXPECT_EQ(seq1, 1u);
    EXPECT_EQ(seq2, 2u);
    EXPECT_EQ(epoch1, epoch2);
    EXPECT_FALSE(epoch1.empty());

    EXPECT_TRUE(got_resume);
    EXPECT_TRUE(got_replayed);
    EXPECT_EQ(replayed_seq, 2u);
    EXPECT_EQ(replayed_epoch, epoch1);

    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(50));
    ::unlink(path.c_str());
}

// IpcClient 对 md seq 跳变做 gap 检测（1 -> 3），并给上层 resync 回调。
TEST(IpcClientTest, DetectsMdSequenceGap) {
    const std::string path = make_uds_path("client_gap");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    auto server_sess = std::make_unique<IpcSession>(
        std::move(pair.srv), io.get_executor(), opt(IpcRole::Gateway, "gw"));
    auto server_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await server_sess->start(); });

    auto client_sess = std::make_unique<IpcSession>(
        std::move(pair.cli), io.get_executor(), opt(IpcRole::Node, "node"));
    auto client_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await client_sess->start(); });

    test::wait_all(io, std::chrono::seconds(2), server_start, client_start);
    server_start.get();
    client_start.get();

    IpcClient client(std::move(client_sess), io.get_executor());
    int       md_frames   = 0;
    bool      got_gap     = false;
    uint64_t  gap_missing = 0;
    client.set_md_handler([&](ReceivedFrame&&) { ++md_frames; });
    client.set_md_gap_handler([&](const StreamGap& gap) {
        got_gap     = true;
        gap_missing = gap.missing;
    });
    client.start();

    auto sender = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        MarketDataPushBody p1;
        p1.meta = StreamMeta{"epoch-test", 1};
        p1.data = nlohmann::json{{"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0}}}}}};
        co_await server_sess->send_body<FrameType::MarketDataPush>(p1);

        MarketDataPushBody p3;
        p3.meta = StreamMeta{"epoch-test", 3};
        p3.data = nlohmann::json{{"quotes", {{"SHFE.rb2610", {{"volume", 42}}}}}};
        co_await server_sess->send_body<FrameType::MarketDataPush>(p3);
    });
    test::wait_all(io, std::chrono::seconds(2), sender);
    sender.rethrow_if_error();
    test::drive_until(
        io, [&]() { return got_gap && md_frames >= 2; }, std::chrono::milliseconds(500));

    EXPECT_TRUE(got_gap);
    EXPECT_EQ(gap_missing, 1u);
    EXPECT_EQ(md_frames, 2);
    EXPECT_EQ(client.metrics().md_gaps, 1u);

    client.stop();
    server_sess->close();
    test::drive_for(io, std::chrono::milliseconds(50));
    ::unlink(path.c_str());
}

// 可重连 IpcClient：第一条连接断开后自动建新连接并发送 Resume，
// 新连接上的 seq 继续递增，客户端 gap 检测不报错。
TEST(IpcClientTest, ReconnectsAndResumes) {
    asio::io_context        io;
    asio::ip::tcp::acceptor acc(io, {asio::ip::tcp::v4(), 0});
    const uint16_t          port = acc.local_endpoint().port();

    bool        server_done = false;
    std::string server_error;
    uint64_t    resume_md_seq = 0;
    std::string resume_epoch;

    // 保持 server 侧 session 活到测试收尾，避免后台 read_loop 使用已析构对象。
    std::shared_ptr<IpcSession> peer1;
    std::shared_ptr<IpcSession> peer2;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto s1_sock = co_await acc.async_accept(asio::use_awaitable);
                auto s1 =
                    std::make_shared<IpcSession>(std::make_unique<TcpChannel>(std::move(s1_sock)),
                                                 io.get_executor(),
                                                 opt(IpcRole::Gateway, "gw1"));
                peer1 = s1;
                co_await s1->start();

                for (uint64_t seq = 1; seq <= 2; ++seq) {
                    MarketDataPushBody p;
                    p.meta = StreamMeta{"epoch-r", seq};
                    p.data = nlohmann::json{
                        {"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0 + seq}}}}}};
                    co_await s1->send_body<FrameType::MarketDataPush>(p);
                }
                // 给客户端读循环一点时间消费 seq=1/2，避免 close 与 TCP 读取竞态。
                asio::steady_timer drain_timer(io);
                drain_timer.expires_after(std::chrono::milliseconds(100));
                co_await drain_timer.async_wait(asio::use_awaitable);
                s1->close(); // 模拟 gw 断线

                auto s2_sock = co_await acc.async_accept(asio::use_awaitable);
                auto s2 =
                    std::make_shared<IpcSession>(std::make_unique<TcpChannel>(std::move(s2_sock)),
                                                 io.get_executor(),
                                                 opt(IpcRole::Gateway, "gw2"));
                peer2 = s2;
                co_await s2->start();

                auto f = co_await s2->incoming().async_receive();
                if (f.has_value() && f->type == FrameType::Resume) {
                    auto rb       = decode_as<FrameType::Resume>(*f);
                    resume_md_seq = rb.md_next_seq;
                    resume_epoch  = rb.epoch;
                }

                ResumeAckBody ack;
                ack.epoch       = resume_epoch;
                ack.md_replay   = true;
                ack.md_from_seq = resume_md_seq;
                co_await s2->send_body<FrameType::ResumeAck>(ack);

                MarketDataPushBody p3;
                p3.meta = StreamMeta{resume_epoch, 3};
                p3.data = nlohmann::json{{"quotes", {{"SHFE.rb2610", {{"last_price", 3803.0}}}}}};
                co_await s2->send_body<FrameType::MarketDataPush>(p3);
                s2->close();
            } catch (const std::exception& e) {
                server_error = e.what();
            }
            server_done = true;
        },
        asio::detached);

    IpcClient::SessionFactory factory =
        [&io, port]() -> asio::awaitable<IpcResult<std::unique_ptr<IpcSession>>> {
        auto channel = TcpChannel::connect(io, "127.0.0.1", port);
        auto session = std::make_unique<IpcSession>(
            std::move(channel), io.get_executor(), opt(IpcRole::Node, "node"));
        auto started = co_await session->start();
        if (!started) {
            co_return std::unexpected(started.error());
        }
        co_return session;
    };

    IpcClient client(factory,
                     io.get_executor(),
                     IpcClient::ReconnectOptions{
                         true, std::chrono::milliseconds(20), std::chrono::milliseconds(100)});

    int      md_frames  = 0;
    uint64_t last_seq   = 0;
    int      resume_ack = 0;
    client.set_md_handler([&](ReceivedFrame&& f) {
        if (f.type != FrameType::MarketDataPush) {
            return;
        }
        auto body = decode_as<FrameType::MarketDataPush>(f);
        ++md_frames;
        last_seq = body.meta.seq;
    });
    client.set_resume_ack_handler([&](const ResumeAckBody&) { ++resume_ack; });

    test::run_coro(io, [&]() -> asio::awaitable<void> {
        auto connect_result = co_await client.connect();
        if (!connect_result) {
            ADD_FAILURE() << connect_result.error().detail;
            co_return;
        }
        client.start();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (md_frames >= 3 && resume_ack >= 1) {
                break;
            }
            asio::steady_timer t(co_await asio::this_coro::executor, std::chrono::milliseconds(10));
            co_await t.async_wait(asio::use_awaitable);
        }
        co_return;
    }());

    EXPECT_TRUE(server_error.empty()) << server_error;
    EXPECT_TRUE(server_done);
    EXPECT_EQ(md_frames, 3);
    EXPECT_EQ(last_seq, 3u);
    EXPECT_EQ(resume_ack, 1);
    EXPECT_EQ(resume_md_seq, 3u);
    EXPECT_EQ(resume_epoch, "epoch-r");

    client.stop();
    acc.close();
    test::drive_for(io, std::chrono::milliseconds(50));
}

// gw 重启后 epoch 变化：Resume 不再 replay，直接要求完整 td 快照。
TEST(GatewayIpcServerTest, ResumeEpochMismatchTriggersTradeSnapshot) {
    const std::string path = make_uds_path("epoch_snapshot");
    asio::io_context  io;
    auto              pair = make_uds_pair(io, path);

    RecordingMdGateway md(io.get_executor());
    SimTdGateway       td(io.get_executor(), 1'000'000.0);

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
        std::make_shared<GatewayIpcServer>(std::move(gw_sess), &md, &td, io.get_executor());
    gw_server->start();

    ResumeAckBody ack;
    bool          got_snapshot = false;
    bool          decode_ok    = false;
    auto          task         = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        ResumeBody resume;
        resume.epoch       = "old-epoch";
        resume.md_next_seq = 7;
        resume.td_next_seq = 5;
        co_await node_sess->send_body<FrameType::Resume>(resume);

        auto ack_frame = co_await node_sess->incoming().async_receive();
        if (ack_frame.has_value()) {
            ack = decode_as<FrameType::ResumeAck>(*ack_frame);
        }
        auto snap_frame = co_await node_sess->incoming().async_receive();
        if (snap_frame.has_value() && snap_frame->type == FrameType::TradeSnapshot) {
            auto body    = decode_as<FrameType::TradeSnapshot>(*snap_frame);
            got_snapshot = body.data.contains("trade");
            decode_ok    = true;
        }
    });
    test::wait_all(io, std::chrono::seconds(2), task);
    task.rethrow_if_error();

    EXPECT_EQ(ack.error, "epoch changed");
    EXPECT_FALSE(ack.md_replay);
    EXPECT_FALSE(ack.td_replay);
    EXPECT_TRUE(ack.td_snapshot_required);
    EXPECT_TRUE(got_snapshot);
    EXPECT_TRUE(decode_ok);

    gw_server->close();
    test::drive_for(io, std::chrono::milliseconds(100));
    ::unlink(path.c_str());
}

// 两个 node 同时连接共享同一 md 网关：EventBus 必须把同一事件广播给两条连接。
TEST(GatewayIpcServerTest, EventBusBroadcastsToAllConnections) {
    const std::string path1 = make_uds_path("bus1");
    const std::string path2 = make_uds_path("bus2");
    asio::io_context  io;
    auto              pair1 = make_uds_pair(io, path1);
    auto              pair2 = make_uds_pair(io, path2);

    RecordingMdGateway md(io.get_executor());

    // 两个 gw server 各自一条 strand；gateway/EventBus 仍在 io core executor。
    auto srv1_ex = asio::make_strand(io);
    auto srv2_ex = asio::make_strand(io);

    auto gw1 =
        std::make_unique<IpcSession>(std::move(pair1.srv), srv1_ex, opt(IpcRole::Gateway, "gw1"));
    auto gw2 =
        std::make_unique<IpcSession>(std::move(pair2.srv), srv2_ex, opt(IpcRole::Gateway, "gw2"));
    auto node1 = std::make_unique<IpcSession>(
        std::move(pair1.cli), io.get_executor(), opt(IpcRole::Node, "node1"));
    auto node2 = std::make_unique<IpcSession>(
        std::move(pair2.cli), io.get_executor(), opt(IpcRole::Node, "node2"));

    auto t1 = test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw1->start(); });
    auto t2 = test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await gw2->start(); });
    auto t3 = test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node1->start(); });
    auto t4 = test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await node2->start(); });
    test::wait_all(io, std::chrono::seconds(2), t1, t2, t3, t4);
    t1.get();
    t2.get();
    t3.get();
    t4.get();

    auto bus  = std::make_shared<EventBus<MarketDataEvent>>(md.output(), io.get_executor());
    auto srv1 = std::make_shared<GatewayIpcServer>(
        std::move(gw1), &md, nullptr, srv1_ex, io.get_executor(), GatewayIpcServer::Options{});
    auto srv2 = std::make_shared<GatewayIpcServer>(
        std::move(gw2), &md, nullptr, srv2_ex, io.get_executor(), GatewayIpcServer::Options{});
    srv1->attach_buses(bus.get(), nullptr);
    srv2->attach_buses(bus.get(), nullptr);
    bus->start();
    srv1->start();
    srv2->start();

    bool        acked1 = false, acked2 = false;
    bool        push1 = false, push2 = false;
    uint64_t    seq1 = 0, seq2 = 0;
    std::string epoch1, epoch2;
    json        data1, data2;

    auto task = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        SubscribeBody sub1;
        sub1.symbols = {"SHFE.rb2610"};
        co_await node1->send_body<FrameType::Subscribe>(sub1);
        SubscribeBody sub2;
        sub2.symbols = {"DCE.i2601"};
        co_await node2->send_body<FrameType::Subscribe>(sub2);

        auto a1 = co_await node1->incoming().async_receive();
        if (a1.has_value()) {
            acked1 = decode_as<FrameType::SubscribeAck>(*a1).ok;
        }
        auto a2 = co_await node2->incoming().async_receive();
        if (a2.has_value()) {
            acked2 = decode_as<FrameType::SubscribeAck>(*a2).ok;
        }

        md.inject(nlohmann::json{
            {"quotes",
             {{"SHFE.rb2610", {{"last_price", 3800.0}}}, {"DCE.i2601", {{"last_price", 800.0}}}}}});

        auto p1 = co_await node1->incoming().async_receive();
        if (p1.has_value()) {
            auto b = decode_as<FrameType::MarketDataPush>(*p1);
            push1  = true;
            seq1   = b.meta.seq;
            epoch1 = b.meta.epoch;
            data1  = std::move(b.data);
        }
        auto p2 = co_await node2->incoming().async_receive();
        if (p2.has_value()) {
            auto b = decode_as<FrameType::MarketDataPush>(*p2);
            push2  = true;
            seq2   = b.meta.seq;
            epoch2 = b.meta.epoch;
            data2  = std::move(b.data);
        }
    });
    test::wait_all(io, std::chrono::seconds(3), task);
    task.rethrow_if_error();

    EXPECT_TRUE(acked1);
    EXPECT_TRUE(acked2);
    EXPECT_TRUE(push1);
    EXPECT_TRUE(push2);
    EXPECT_EQ(seq1, 1u);
    EXPECT_EQ(seq2, 1u);
    // 每个 GatewayIpcServer 是独立连接/独立流，epoch 各自生成。
    EXPECT_FALSE(epoch1.empty());
    EXPECT_FALSE(epoch2.empty());

    // 同一广播事件按连接订阅裁剪，互不泄漏。
    ASSERT_TRUE(data1.contains("quotes"));
    EXPECT_TRUE(data1["quotes"].contains("SHFE.rb2610"));
    EXPECT_FALSE(data1["quotes"].contains("DCE.i2601"));
    ASSERT_TRUE(data2.contains("quotes"));
    EXPECT_TRUE(data2["quotes"].contains("DCE.i2601"));
    EXPECT_FALSE(data2["quotes"].contains("SHFE.rb2610"));

    bus->stop();
    srv1->close();
    srv2->close();
    node1->close();
    node2->close();
    test::drive_for(io, std::chrono::milliseconds(100));
    ::unlink(path1.c_str());
    ::unlink(path2.c_str());
}

} // namespace
} // namespace ipc
} // namespace tqsdk
