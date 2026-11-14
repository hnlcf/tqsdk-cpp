#include "tqsdk/pch.h"

#include <fstream>

#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/ipc/uds_channel.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;
using json = nlohmann::json;

namespace {

// ============================ 配置解析 ============================

TEST(TqApiConfigTest, ParseInProcessSim) {
    auto cfg = parse_tqapi_config(R"(
        [ipc]
        transport = "uds"
        encoding = "msgpack"

        [md]
        kind = "tq"
        host = "example.com"
        port = "8443"

        [td]
        kind = "sim"
        initial_balance = 500000
    )")
                   .value();

    EXPECT_EQ(cfg.ipc.transport, IpcTransport::Uds);
    EXPECT_EQ(cfg.ipc.encoding, ipc::FrameEncoding::MessagePack);
    EXPECT_EQ(cfg.md.kind, MdKind::Tq);
    EXPECT_EQ(cfg.md.host, "example.com");
    EXPECT_EQ(cfg.md.port, "8443");
    EXPECT_EQ(cfg.td.kind, TdKind::Sim);
    EXPECT_DOUBLE_EQ(cfg.td.initial_balance, 500000.0);
}

TEST(TqApiConfigTest, ParseRemoteBoth) {
    auto cfg = parse_tqapi_config(R"(
        [ipc]
        transport = "uds"
        uds_dir = "/tmp"

        [md]
        kind = "remote"
        socket = "/tmp/gw.sock"
        name = "gw-md"

        [td]
        kind = "remote"
        socket = "/tmp/gw.sock"
        name = "gw-td"
        ack_timeout_ms = 3000
    )")
                   .value();
    EXPECT_EQ(cfg.md.kind, MdKind::Remote);
    EXPECT_EQ(cfg.md.socket, "/tmp/gw.sock");
    EXPECT_EQ(cfg.td.kind, TdKind::Remote);
    EXPECT_EQ(cfg.td.socket, "/tmp/gw.sock");
    EXPECT_EQ(cfg.td.ack_timeout, std::chrono::milliseconds(3000));
}

TEST(TqApiConfigTest, ParseTcpTransport) {
    auto cfg = parse_tqapi_config(R"(
        [ipc]
        transport = "tcp"

        [md]
        kind = "remote"
        tcp_host = "127.0.0.1"
        tcp_port = 9000
    )")
                   .value();
    EXPECT_EQ(cfg.ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg.md.tcp_host, "127.0.0.1");
    EXPECT_EQ(cfg.md.tcp_port, 9000);
}

TEST(TqApiConfigTest, ParseDefaultsToNone) {
    auto cfg = parse_tqapi_config("").value();
    EXPECT_EQ(cfg.md.kind, MdKind::None);
    EXPECT_EQ(cfg.td.kind, TdKind::None);
    EXPECT_EQ(cfg.ipc.transport, IpcTransport::Uds);
    EXPECT_EQ(cfg.ipc.encoding, ipc::FrameEncoding::Json);
}

TEST(TqApiConfigTest, ParseErrorsOnBadKind) {
    EXPECT_FALSE(parse_tqapi_config("[md]\nkind = \"bogus\"\n"));
    EXPECT_FALSE(parse_tqapi_config("[td]\nkind = \"bogus\"\n"));
    EXPECT_EQ(parse_tqapi_config("[td]\nkind = \"ctp\"\n").value().td.kind, TdKind::Ctp);
    EXPECT_FALSE(parse_tqapi_config("[ipc]\ntransport = \"smoke\"\n"));
    EXPECT_FALSE(parse_tqapi_config("this is not toml === "));
}

// ============================ 进程内构造 ============================

TEST(TqApiFactoryTest, BuildSimTdOnly) {
    asio::io_context ctx;

    TqApiConfig cfg;
    cfg.td.kind            = TdKind::Sim;
    cfg.td.initial_balance = 1'000'000.0;

    std::unique_ptr<TqApi> api;
    auto                   mk = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await make_tqapi(ctx, cfg);
        if (!result) {
            throw std::runtime_error(result.error().detail);
        }
        api = std::move(*result);
    });
    test::wait_all(ctx, std::chrono::seconds(2), mk);
    mk.rethrow_if_error();

    ASSERT_NE(api, nullptr);

    // 用 sim 下一单，验证整条链路可用
    bool placed = false;
    auto body   = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        InsertOrderReq req;
        req.instrument_id = "SHFE.rb2610";
        req.direction     = Direction::BUY;
        req.offset        = Offset::OPEN;
        req.price_type    = PriceType::LIMIT;
        req.limit_price   = 3800.0;
        req.volume        = 1;
        Order* o          = co_await api->insert_order(req);
        placed            = (o != nullptr);
    });
    test::wait_all(ctx, std::chrono::seconds(2), body);
    body.rethrow_if_error();
    EXPECT_TRUE(placed);

    api->close();
    test::drive_for(ctx, std::chrono::milliseconds(30));
}

TEST(TqApiFactoryTest, BuildNoneBoth) {
    asio::io_context ctx;
    TqApiConfig      cfg; // 全 none

    std::unique_ptr<TqApi> api;
    auto                   mk = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await make_tqapi(ctx, cfg);
        if (!result) {
            throw std::runtime_error(result.error().detail);
        }
        api = std::move(*result);
    });
    test::wait_all(ctx, std::chrono::seconds(2), mk);
    mk.rethrow_if_error();
    ASSERT_NE(api, nullptr);

    api->close();
    test::drive_for(ctx, std::chrono::milliseconds(20));
}

// ============================ 远程端到端 ============================
//
// 结构：先在 main 线程创建 acceptor（构造即 bind+listen），再让 make_tqapi
// 同步 connect（因 acceptor 已 listen，连接进 backlog，不会阻塞）；之后同一条
// io_context 驱动 node 侧的 accept 协程与 gw 侧握手，两条协程并发推进。
// 注意 node 侧角色必须是 Node（与 gw 的 Gateway 角色不同），否则握手失败。

std::string tmp_sock(const char* tag) {
    static std::atomic<int> c{0};
    return "/tmp/tqsdk_factory_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(c.fetch_add(1)) + ".sock";
}

// node（假 gw 后端）应答器：收到 Subscribe 回 SubscribeAck；
// 收到 Command 回 CommandAck；另可主动推一条行情。
struct FakeNode {
    std::unique_ptr<ipc::IpcSession>  sess;
    std::optional<ipc::SubscribeBody> got_sub;
    std::optional<ipc::CommandBody>   got_cmd;
};

TEST(TqApiFactoryTest, RemoteMdAndTdEndToEnd) {
    asio::io_context  ctx;
    const std::string path = tmp_sock("e2e");
    ::unlink(path.c_str());

    asio::local::stream_protocol::acceptor acc(ctx, {path});

    TqApiConfig cfg;
    cfg.ipc.transport  = IpcTransport::Uds;
    cfg.ipc.encoding   = ipc::FrameEncoding::Json;
    cfg.md.kind        = MdKind::Remote;
    cfg.md.socket      = path;
    cfg.md.name        = "gw-md";
    cfg.td.kind        = TdKind::Remote;
    cfg.td.socket      = path;
    cfg.td.name        = "gw-td";
    cfg.td.ack_timeout = std::chrono::milliseconds(500);

    // ---- node 侧：accept 后跑 IpcSession(Node) 并应答 ----
    FakeNode node;
    auto     node_task = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        auto sock = co_await acc.async_accept(asio::use_awaitable);
        auto ch   = std::make_unique<ipc::UdsChannel>(std::move(sock));

        ipc::IpcSession::Options o;
        o.local_role         = ipc::IpcRole::Node;
        o.local_name         = "node";
        o.handshake_timeout  = std::chrono::seconds(3);
        o.heartbeat_interval = std::chrono::seconds(60);
        o.heartbeat_timeout  = std::chrono::seconds(120);
        node.sess = std::make_unique<ipc::IpcSession>(std::move(ch), ctx.get_executor(), o);
        (void)co_await node.sess->start();

        // 应答循环：处理 Subscribe / Command 若干次后退出
        for (int i = 0; i < 8; ++i) {
            auto opt = co_await node.sess->incoming().async_receive();
            if (!opt) {
                break;
            }
            if (opt->type == ipc::FrameType::Subscribe) {
                node.got_sub = ipc::decode_as<ipc::FrameType::Subscribe>(*opt);
                SPDLOG_TRACE("[FakeNode] got Subscribe");
                (void)co_await node.sess->send_body<ipc::FrameType::SubscribeAck>(
                    ipc::SubscribeAckBody{true, ""});
            } else if (opt->type == ipc::FrameType::Command) {
                node.got_cmd = ipc::decode_as<ipc::FrameType::Command>(*opt);
                SPDLOG_TRACE("[FakeNode] got Command rid={}", node.got_cmd->rid);
                (void)co_await node.sess->send_body<ipc::FrameType::CommandAck>(
                    ipc::CommandAckBody{CommandAck{true, node.got_cmd->rid, ""}});
            }
        }
    });

    // ---- gw 侧：按配置造 TqApi（同步 connect + 后台握手）----
    std::unique_ptr<TqApi> api;
    auto                   mk = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        auto result = co_await make_tqapi(ctx, cfg);
        if (!result) {
            throw std::runtime_error(result.error().detail);
        }
        api = std::move(*result);
    });

    // 驱动：等待 TqApi 建好（握手完成）
    ASSERT_TRUE(test::drive_until(ctx, [&] { return mk.is_done(); }, std::chrono::seconds(3)));
    mk.rethrow_if_error();
    ASSERT_NE(api, nullptr);

    // 订阅 + 下单
    bool ordered     = false;
    auto client_task = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        // 触发 md 订阅：通过 get_quote 间接订阅（sim/tq 会订阅；remote 也会发 Subscribe）
        (void)api->get_quote("SHFE.rb2610");

        InsertOrderReq req;
        req.instrument_id = "SHFE.rb2610";
        req.direction     = Direction::BUY;
        req.offset        = Offset::OPEN;
        req.price_type    = PriceType::LIMIT;
        req.limit_price   = 3800.0;
        req.volume        = 1;
        CommandAck ack    = co_await api->cancel_order(CancelOrderReq{"rid_x", ""});
        SPDLOG_TRACE("[FakeNode] cancel_order returned success={} order_id={}",
                     (int)ack.success,
                     ack.order_id);
        (void)ack;
        ordered = true;
    });

    ASSERT_TRUE(
        test::drive_until(ctx, [&] { return client_task.is_done(); }, std::chrono::seconds(3)));
    client_task.rethrow_if_error();
    EXPECT_TRUE(ordered) << "cancel_order did not complete";

    // 继续驱动，让 node 处理完成若干帧
    test::drive_for(ctx, std::chrono::milliseconds(100));

    // node 应收到过 Command（撤单）
    EXPECT_TRUE(node.got_cmd.has_value()) << "node did not receive Command";
    if (node.got_cmd) {
        EXPECT_EQ(node.got_cmd->op, ipc::CommandOp::CancelOrder);
        EXPECT_EQ(node.got_cmd->cancel.order_id, "rid_x");
        // rid 作为关联键必须随帧携带
        EXPECT_EQ(node.got_cmd->rid, "rid_x");
    }

    SPDLOG_TRACE("[test] before api close, got_cmd={}", node.got_cmd.has_value());
    api->close();
    SPDLOG_TRACE("[test] after api close");
    if (node.sess) {
        node.sess->close();
    }
    // 必须先把后台协程（md_loop/td_loop/client/node 读循环）驱动到退出，
    // 再析构 api/node.sess；否则 Channel 析构时仍挂着 waiter（UAF）。
    test::drive_for(ctx, std::chrono::milliseconds(200));
    SPDLOG_TRACE("[test] after drive");
    api.reset();
    node.sess.reset();
    test::drive_for(ctx, std::chrono::milliseconds(100));
    SPDLOG_TRACE("[test] teardown done");
    ::unlink(path.c_str());
}

TEST(TqApiFactoryConfig, ParsesRuntimeThreads) {
    auto node = parse_node_config("[runtime]\nthreads = 4\n").value();
    EXPECT_EQ(node.runtime.threads, 4);

    auto gw = parse_gw_config("[runtime]\nthreads = 2\n").value();
    EXPECT_EQ(gw.runtime.threads, 2);

    // 0/负值收敛为 1，超大值封顶 64。
    EXPECT_EQ(parse_node_config("[runtime]\nthreads = 0\n").value().runtime.threads, 1);
    EXPECT_EQ(parse_gw_config("[runtime]\nthreads = -8\n").value().runtime.threads, 1);
    EXPECT_EQ(parse_node_config("[runtime]\nthreads = 1000\n").value().runtime.threads, 64);
}

TEST(TqApiFactoryConfig, ParsesKlineCache) {
    auto node = parse_node_config(R"(
[kline_cache]
enabled = true
file = "data/kline.sqlite3"
max_bars_per_series = 500
flush_interval_ms = 50
max_pending_bars = 1234
fresh_after_sec = 60
)")
                    .value();
    EXPECT_TRUE(node.kline_cache.enabled);
    EXPECT_EQ(node.kline_cache.file, "data/kline.sqlite3");
    EXPECT_EQ(node.kline_cache.max_bars_per_series, 500);
    EXPECT_EQ(node.kline_cache.flush_interval_ms, 50);
    EXPECT_EQ(node.kline_cache.max_pending_bars, 1234u);
    EXPECT_EQ(node.kline_cache.fresh_after_sec, 60);

    auto disabled = parse_node_config("[kline_cache]\nenabled = false\n").value();
    EXPECT_FALSE(disabled.kline_cache.enabled);
}

TEST(TqApiFactoryConfig, MarketCacheFileIsSharedUnlessExplicit) {
    auto cfg = parse_node_config(R"(
[market_cache]
file = "/tmp/market.sqlite3"
[kline_cache]
enabled = true
[tick_cache]
enabled = true
)")
                   .value();
    EXPECT_EQ(cfg.kline_cache.file, "/tmp/market.sqlite3");
    EXPECT_EQ(cfg.tick_cache.file, "/tmp/market.sqlite3");

    auto explicit_cfg = parse_node_config(R"(
[market_cache]
file = "/tmp/market.sqlite3"
[kline_cache]
file = "/tmp/kline-only.sqlite3"
[tick_cache]
file = "/tmp/tick-only.sqlite3"
)")
                            .value();
    EXPECT_EQ(explicit_cfg.kline_cache.file, "/tmp/kline-only.sqlite3");
    EXPECT_EQ(explicit_cfg.tick_cache.file, "/tmp/tick-only.sqlite3");
}

TEST(TqApiFactoryConfig, ParsesNodeTickCache) {
    auto cfg = parse_node_config(R"(
[tick_cache]
enabled = true
file = "/tmp/ticks.sqlite3"
max_ticks_per_series = 1234
flush_interval_ms = 20
max_pending_ticks = 99
)")
                   .value();
    EXPECT_TRUE(cfg.tick_cache.enabled);
    EXPECT_EQ(cfg.tick_cache.file, "/tmp/ticks.sqlite3");
    EXPECT_EQ(cfg.tick_cache.max_ticks_per_series, 1234u);
    EXPECT_EQ(cfg.tick_cache.flush_interval_ms, 20);
    EXPECT_EQ(cfg.tick_cache.max_pending_ticks, 99u);
}

TEST(TqApiFactoryConfig, ParsesNodeAccountIpcGroups) {
    auto cfg = parse_node_config(R"(
primary_account = "B"
[[accounts]]
id = "B"
ipc = "gw-b"
[[accounts]]
id = "C"
ipc = "gw-c"

[ipc.gw-b]
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = 14331

[ipc.gw-c]
transport = "tcp"
tcp_host = "10.0.0.2"
tcp_port = 14332
)")
                   .value();
    ASSERT_EQ(cfg.accounts.size(), 2u);
    EXPECT_EQ(cfg.primary_account, "B");
    EXPECT_EQ(cfg.accounts[0].id, "B");
    EXPECT_EQ(cfg.accounts[0].ipc.tcp_port, 14331);
    EXPECT_EQ(cfg.accounts[1].id, "C");
    EXPECT_EQ(cfg.accounts[1].ipc.tcp_host, "10.0.0.2");
}

TEST(TqApiFactoryConfig, ParsesAutoUrlFlags) {
    auto cfg =
        parse_tqapi_config("[md]\nkind = \"tq\"\nauto_url = true\n[td]\nauto_url = true\n").value();
    EXPECT_TRUE(cfg.md.auto_url);
    EXPECT_TRUE(cfg.td.auto_url);

    auto gw =
        parse_gw_config("[md]\nkind = \"tq\"\nauto_url = true\n[td]\nauto_url = true\n").value();
    EXPECT_TRUE(gw.md.auto_url);
    EXPECT_TRUE(gw.td.auto_url);
}

TEST(TqApiFactoryConfig, ParsesSimSpecsAndSeedPrices) {
    auto cfg = parse_tqapi_config(R"(
[td]
kind = "sim"
initial_balance = 200000.0

[td.specs."SHFE.rb2701"]
volume_multiple = 10.0
price_tick = 1.0
margin_rate = 0.12
commission_per_lot = 2.5

[td.last_prices]
"SHFE.rb2701" = 3037.0
)")
                   .value();
    EXPECT_EQ(cfg.td.kind, TdKind::Sim);
    EXPECT_DOUBLE_EQ(cfg.td.initial_balance, 200000.0);
    ASSERT_EQ(cfg.td.specs.size(), 1u);
    EXPECT_EQ(cfg.td.specs[0].symbol, "SHFE.rb2701");
    EXPECT_DOUBLE_EQ(cfg.td.specs[0].volume_multiple, 10.0);
    EXPECT_DOUBLE_EQ(cfg.td.specs[0].margin_rate, 0.12);
    EXPECT_DOUBLE_EQ(cfg.td.specs[0].commission_per_lot, 2.5);
    EXPECT_DOUBLE_EQ(cfg.td.last_prices.at("SHFE.rb2701"), 3037.0);
}

TEST(TqApiFactoryConfig, ParsesServeAuthSection) {
    const char* toml = R"(
[md]
kind = "none"
[td]
kind = "none"
[serve.auth]
enabled = true
token_env = "MY_TQ_TOKEN"
allow_query_token = false
exempt_healthz = false
)";
    auto        cfg  = parse_node_config(toml).value();
    EXPECT_TRUE(cfg.auth.enabled);
    EXPECT_EQ(cfg.auth.token_env, "MY_TQ_TOKEN");
    EXPECT_FALSE(cfg.auth.allow_query_token);
    EXPECT_FALSE(cfg.auth.exempt_healthz);
}

TEST(TqApiFactoryConfig, ParsesAuditSection) {
    const char* toml = R"(
[audit]
enabled = true
file = "/tmp/audit.log"
max_bytes = 2048
max_files = 2
)";
    auto        cfg  = parse_node_config(toml).value();
    EXPECT_TRUE(cfg.audit.enabled);
    EXPECT_EQ(cfg.audit.file, "/tmp/audit.log");
    EXPECT_EQ(cfg.audit.max_bytes, 2048u);
    EXPECT_EQ(cfg.audit.max_files, 2u);
}

TEST(TqApiFactoryConfig, ParsesCtpSection) {
    const char* toml = R"(
[md]
kind = "ctp"
[td]
kind = "ctp"
[ctp]
broker_id = "9999"
user_id = "u1"
password = "p1"
auth_code = "a1"
app_id = "app1"
td_front = "tcp://127.0.0.1:41205"
md_front = "tcp://127.0.0.1:41213"
flow_path = "/tmp/ctp-flow"
order_interval_ms = 37
front_timeout_ms = 12345
)";
    auto        cfg  = parse_tqapi_config(toml).value();
    EXPECT_EQ(cfg.md.kind, MdKind::Ctp);
    EXPECT_EQ(cfg.td.kind, TdKind::Ctp);
    EXPECT_EQ(cfg.ctp.broker_id, "9999");
    EXPECT_EQ(cfg.ctp.user_id, "u1");
    EXPECT_EQ(cfg.ctp.password, "p1");
    EXPECT_EQ(cfg.ctp.auth_code, "a1");
    EXPECT_EQ(cfg.ctp.app_id, "app1");
    EXPECT_EQ(cfg.ctp.td_front, "tcp://127.0.0.1:41205");
    EXPECT_EQ(cfg.ctp.md_front, "tcp://127.0.0.1:41213");
    EXPECT_EQ(cfg.ctp.flow_path, "/tmp/ctp-flow");
    EXPECT_EQ(cfg.ctp.order_interval.count(), 37);
    EXPECT_EQ(cfg.ctp.front_timeout.count(), 12345);

    auto gw = parse_gw_config(toml).value();
    EXPECT_EQ(gw.ctp.broker_id, "9999");
}

} // namespace

TEST(ConfigErrors, TryParseReturnsConfigError) {
    const auto result = try_parse_tqapi_config("[md]\nkind = \"bogus\"\n");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_FALSE(result.error().detail.empty());
}

TEST(ConfigErrors, LoadMissingFileReturnsError) {
    const auto result = load_node_config("/tmp/tqsdk-missing-config-for-test.toml");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, config::ConfigErrorCode::file_io);
    EXPECT_FALSE(result.error().detail.empty());
}

TEST(TqApiConfigTest, ParseTqTdOptions) {
    const auto cfg = parse_tqapi_config(R"(
        [td]
        kind = "tq"
        auto_url = true
        [td.tq]
        broker_name = "simnow"
        account_id = "user_a"
        password = "secret"
        broker_id = "9999"
        front_url = "tcp://127.0.0.1:1"
        account_key = "A"
    )");
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->td.kind, TdKind::Tq);
    EXPECT_TRUE(cfg->td.auto_url);
    EXPECT_EQ(cfg->td.tq.broker_name, "simnow");
    EXPECT_EQ(cfg->td.tq.account_id, "user_a");
    EXPECT_EQ(cfg->td.tq.account_key, "A");
    EXPECT_EQ(cfg->td.tq.broker_id, "9999");
}

TEST(TqApiConfigTest, ParseGatewayTqTdOptions) {
    const auto cfg = parse_gw_config(R"(
        [td]
        kind = "tq"
        auto_url = true
        [td.tq]
        broker_name = "simnow"
        account_id = "user_b"
        password = "secret"
    )");
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->td.kind, TdKind::Tq);
    EXPECT_TRUE(cfg->td.auto_url);
    EXPECT_EQ(cfg->td.tq.broker_name, "simnow");
    EXPECT_EQ(cfg->td.tq.account_id, "user_b");
}

TEST(TqApiConfigTest, TqTdRequiresBrokerName) {
    const auto cfg = parse_gw_config(R"(
        [td]
        kind = "tq"
        [td.tq]
        bank_account_id = "typo"
    )");
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_NE(cfg.error().detail.find("broker_name"), std::string::npos);
}

namespace {

struct TempConfigBundle {
    std::filesystem::path root;

    TempConfigBundle() {
        root = std::filesystem::temp_directory_path() /
               fmt::format("tqsdk-config-bundle-{}",
                           std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root / "shared");
    }

    ~TempConfigBundle() { std::filesystem::remove_all(root); }

    void write(std::string_view name, std::string_view body) const {
        const auto path = root / std::string(name);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << body;
    }
};

} // namespace

TEST(TqApiConfigTest, LoadsGatewayConfigDirectoryWithIncludes) {
    TempConfigBundle bundle;
    bundle.write("ipc/gw-a.toml", R"(
        [ipc]
        name = "bundle-gw"
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
        encoding = "json"
        heartbeat_interval_ms = 6000
        heartbeat_timeout_ms = 18000
    )");
    bundle.write("shared/tq.toml", R"(
        [tq_auth]
        enabled = true
        user_name = "bundle-user"
        password = "bundle-password"
        [td.tq]
        broker_name = "from-fragment"
        account_id = "bundle-account"
        password = "account-password"
    )");
    bundle.write("gw.toml", R"(
        [include]
        files = ["shared/tq.toml"]
        [gateway]
        ipc = "gw-a"
        [md]
        kind = "none"
        [td]
        kind = "tq"
        auto_url = true
        [td.tq]
        broker_name = "from-entry"
    )");

    const auto cfg = load_gw_config(bundle.root.string());
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->ipc.tcp_port, 19001);
    EXPECT_EQ(cfg->ipc.name, "bundle-gw");
    EXPECT_EQ(cfg->ipc.heartbeat_interval, std::chrono::milliseconds(6000));
    EXPECT_EQ(cfg->td.tq.broker_name, "from-entry");
    EXPECT_EQ(cfg->td.tq.account_id, "bundle-account");
    EXPECT_TRUE(cfg->tq_auth.enabled);
    EXPECT_EQ(cfg->tq_auth.user_name, "bundle-user");
}

TEST(TqApiConfigTest, LoadsNodeConfigDirectoryWithIncludes) {
    TempConfigBundle bundle;
    bundle.write("ipc/gw-a.toml", R"(
        [ipc]
        name = "gw-a"
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
        encoding = "json"
        handshake_timeout_ms = 6000
        heartbeat_interval_ms = 7000
        heartbeat_timeout_ms = 21000
    )");
    bundle.write("node.toml", R"(
        [include]
        files = []
        primary_account = "default"
        [[accounts]]
        id = "default"
        ipc = "gw-a"
        [serve]
        bind = "127.0.0.1"
        port = 19002
    )");

    const auto cfg = load_node_config(bundle.root.string());
    ASSERT_TRUE(cfg);
    ASSERT_EQ(cfg->accounts.size(), 1u);
    EXPECT_EQ(cfg->primary_account, "default");
    EXPECT_EQ(cfg->accounts[0].ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg->accounts[0].ipc.tcp_port, 19001);
    EXPECT_EQ(cfg->accounts[0].ipc.handshake_timeout, std::chrono::milliseconds(6000));
    EXPECT_EQ(cfg->serve_port, 19002);
}

TEST(TqApiConfigTest, MissingIncludeReturnsFileIoError) {
    TempConfigBundle bundle;
    bundle.write("gw.toml", R"(
        [include]
        files = ["shared/missing.toml"]
        [td]
        kind = "none"
    )");

    const auto cfg = load_gw_config(bundle.root.string());
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::file_io);
    EXPECT_NE(cfg.error().detail.find("missing.toml"), std::string::npos);
}

TEST(TqApiConfigTest, RejectsIncludeCycle) {
    TempConfigBundle bundle;
    bundle.write("a.toml", R"(
        [include]
        files = ["b.toml"]
        [td]
        kind = "none"
    )");
    bundle.write("b.toml", R"(
        [include]
        files = ["a.toml"]
    )");
    bundle.write("gw.toml", R"(
        [include]
        files = ["a.toml"]
    )");

    const auto cfg = load_gw_config(bundle.root.string());
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_NE(cfg.error().detail.find("cycle"), std::string::npos);
}

TEST(TqApiFactoryConfig, RemoteTqApiConfigUsesIpcGroup) {
    IpcConfig ipc;
    ipc.transport = IpcTransport::Tcp;
    ipc.tcp_host  = "10.0.0.8";
    ipc.tcp_port  = 14330;
    ipc.encoding  = ipc::FrameEncoding::Json;

    const auto cfg = make_remote_tqapi_config(ipc, "account-b");
    EXPECT_EQ(cfg.ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg.ipc.role, ipc::IpcRole::Node);
    EXPECT_EQ(cfg.ipc.local_name, "node-account-b");
    EXPECT_EQ(cfg.md.kind, MdKind::Remote);
    EXPECT_EQ(cfg.td.kind, TdKind::Remote);
    EXPECT_EQ(cfg.md.tcp_host, "10.0.0.8");
    EXPECT_EQ(cfg.md.tcp_port, 14330);
    EXPECT_EQ(cfg.td.tcp_host, "10.0.0.8");
    EXPECT_EQ(cfg.td.tcp_port, 14330);
}

TEST(TqApiFactoryConfig, LegacyNodeRemoteConfigCreatesAccount) {
    const auto cfg = parse_node_config(R"(
[ipc]
transport = "tcp"
encoding = "json"
handshake_timeout_ms = 3000
[md]
kind = "remote"
tcp_host = "127.0.0.1"
tcp_port = 14330
[td]
kind = "remote"
tcp_host = "127.0.0.1"
tcp_port = 14330
)");
    ASSERT_TRUE(cfg);
    ASSERT_EQ(cfg->accounts.size(), 1u);
    EXPECT_EQ(cfg->primary_account, "default");
    EXPECT_EQ(cfg->accounts[0].ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg->accounts[0].ipc.tcp_port, 14330);
    EXPECT_EQ(cfg->accounts[0].ipc.handshake_timeout, std::chrono::milliseconds(3000));
}

TEST(TqApiFactoryConfig, LegacyGwConfigUsesListenAsIpcGroup) {
    const auto cfg = parse_gw_config(R"(
[listen]
transport = "tcp"
tcp_host = "127.0.0.1"
tcp_port = 14330
[ipc]
encoding = "msgpack"
heartbeat_interval_ms = 7000
[td]
kind = "none"
)");
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg->ipc.tcp_port, 14330);
    EXPECT_EQ(cfg->ipc.encoding, ipc::FrameEncoding::MessagePack);
    EXPECT_EQ(cfg->ipc.heartbeat_interval, std::chrono::milliseconds(7000));
}
