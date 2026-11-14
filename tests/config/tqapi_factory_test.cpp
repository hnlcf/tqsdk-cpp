#include "tqsdk/pch.h"

#include <fstream>

#include "tqsdk/config/tqapi_factory.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/tqapi.h"

#include "support/test_helpers.h"

using namespace tqsdk;

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

TEST(TqApiConfigTest, ParsesGatewayNestedConfig) {
    const auto cfg = parse_gw_config(R"(
        [gateway]
        ipc = "gw-a"

        [ipc.gw-a]
        name = "gw-a"
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
        encoding = "msgpack"
        heartbeat_interval_ms = 6000

        [md]
        kind = "tq"
        [md.tq]
        host = "example.com"
        port = "8443"
        path = "/md"
        auto_url = true

        [td]
        kind = "sim"
        auto_url = true
        [td.sim]
        initial_balance = 500000
        [td.sim.specs."SHFE.rb2701"]
        volume_multiple = 10.0
        price_tick = 1.0
        margin_rate = 0.12
        [td.sim.last_prices]
        "SHFE.rb2701" = 3037.0

        [runtime]
        threads = 2
    )");
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->engine.ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg->engine.ipc.tcp_port, 19001);
    EXPECT_EQ(cfg->engine.ipc.encoding, ipc::FrameEncoding::MessagePack);
    EXPECT_EQ(cfg->engine.ipc.heartbeat_interval, std::chrono::milliseconds(6000));
    EXPECT_EQ(cfg->engine.md.kind, MdKind::Tq);
    EXPECT_EQ(cfg->engine.md.tq.host, "example.com");
    EXPECT_EQ(cfg->engine.md.tq.port, "8443");
    EXPECT_TRUE(cfg->engine.md.tq.auto_url);
    EXPECT_EQ(cfg->engine.td.kind, TdKind::Sim);
    EXPECT_TRUE(cfg->engine.td.auto_url);
    EXPECT_DOUBLE_EQ(cfg->engine.td.sim.initial_balance, 500000.0);
    ASSERT_EQ(cfg->engine.td.sim.specs.size(), 1u);
    EXPECT_EQ(cfg->engine.td.sim.specs[0].symbol, "SHFE.rb2701");
    EXPECT_DOUBLE_EQ(cfg->engine.td.sim.last_prices.at("SHFE.rb2701"), 3037.0);
    EXPECT_EQ(cfg->runtime.threads, 2);
}

TEST(TqApiConfigTest, ParsesNodeServeCacheAndAccounts) {
    const auto cfg = parse_node_config(R"(
        primary_account = "default"

        [[accounts]]
        id = "default"
        ipc = "gw-a"

        [[accounts]]
        id = "b"
        ipc = "gw-b"

        [ipc.gw-a]
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001

        [ipc.gw-b]
        transport = "uds"
        socket = "/tmp/gw-b.sock"

        [serve]
        bind = "0.0.0.0"
        port = 9000
        symbols = ["SHFE.rb2701"]
        [serve.auth]
        enabled = true
        token_env = "MY_TOKEN"
        allow_query_token = false
        [serve.ws]
        path = "/ws"
        heartbeat_interval_ms = 1500
        max_clients = 7
        max_queue = 9
        coalesce_ms = 12

        [cache]
        file = "/tmp/market.sqlite3"
        [cache.instruments]
        enabled = true
        file = "symbol.json"
        [cache.kline]
        enabled = true
        max_bars_per_series = 500
        [cache.tick]
        enabled = true
        max_ticks_per_series = 1234
        [cache.continuous]
        enabled = true
        file = "continuous.json"
    )");
    ASSERT_TRUE(cfg);
    ASSERT_EQ(cfg->accounts.size(), 2u);
    EXPECT_EQ(cfg->primary_account, "default");
    EXPECT_EQ(cfg->accounts[0].ipc.tcp_port, 19001);
    EXPECT_EQ(cfg->accounts[1].ipc.socket, "/tmp/gw-b.sock");
    EXPECT_EQ(cfg->serve.bind, "0.0.0.0");
    EXPECT_EQ(cfg->serve.port, 9000);
    ASSERT_EQ(cfg->serve.symbols.size(), 1u);
    EXPECT_TRUE(cfg->serve.auth.enabled);
    EXPECT_EQ(cfg->serve.auth.token_env, "MY_TOKEN");
    EXPECT_FALSE(cfg->serve.auth.allow_query_token);
    EXPECT_EQ(cfg->serve.ws.path, "/ws");
    EXPECT_EQ(cfg->serve.ws.heartbeat_interval, std::chrono::milliseconds(1500));
    EXPECT_EQ(cfg->serve.ws.max_clients, 7u);
    EXPECT_EQ(cfg->serve.ws.coalesce, std::chrono::milliseconds(12));
    EXPECT_EQ(cfg->cache.file, "/tmp/market.sqlite3");
    EXPECT_EQ(cfg->cache.kline.file, "/tmp/market.sqlite3");
    EXPECT_EQ(cfg->cache.tick.file, "/tmp/market.sqlite3");
    EXPECT_TRUE(cfg->cache.instruments.enabled);
    EXPECT_EQ(cfg->cache.kline.max_bars_per_series, 500);
    EXPECT_EQ(cfg->cache.tick.max_ticks_per_series, 1234u);
    EXPECT_EQ(cfg->cache.continuous.file, "continuous.json");
}

TEST(TqApiConfigTest, CacheCanUseSeparateFiles) {
    const auto cfg = parse_node_config(R"(
        [[accounts]]
        id = "default"
        ipc = "gw-a"
        [ipc.gw-a]
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
        [cache]
        file = "/tmp/shared.sqlite3"
        [cache.kline]
        file = "/tmp/kline.sqlite3"
        [cache.tick]
        file = "/tmp/tick.sqlite3"
    )");
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->cache.kline.file, "/tmp/kline.sqlite3");
    EXPECT_EQ(cfg->cache.tick.file, "/tmp/tick.sqlite3");
}

TEST(TqApiConfigTest, MissingGatewayIpcIsRejected) {
    const auto cfg = parse_gw_config(R"(
        [md]
        kind = "none"
        [td]
        kind = "none"
    )");
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_NE(cfg.error().detail.find("gateway"), std::string::npos);
}

TEST(TqApiConfigTest, MissingNodeAccountsIsRejected) {
    const auto cfg = parse_node_config("[serve]\nport = 8080\n");
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_NE(cfg.error().detail.find("accounts"), std::string::npos);
}

TEST(TqApiConfigTest, TqTdRequiresBrokerName) {
    const auto cfg = parse_gw_config(R"(
        [gateway]
        ipc = "gw-a"
        [ipc.gw-a]
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
        [td]
        kind = "tq"
        [td.tq]
        bank_account_id = "typo"
    )");
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_NE(cfg.error().detail.find("broker_name"), std::string::npos);
}

TEST(TqApiConfigTest, LoadsGatewayGroupFileWithIncludes) {
    TempConfigBundle bundle;
    bundle.write("ipc/gw-a.toml", R"(
        [ipc]
        name = "bundle-gw"
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
        heartbeat_interval_ms = 6000
    )");
    bundle.write("shared/tq.toml", R"(
        [tq_auth]
        enabled = true
        user_name = "bundle-user"
        password = "bundle-password"
        [td.tq]
        broker_name = "from-fragment"
        account_id = "bundle-account"
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
        [td.tq]
        broker_name = "from-entry"
    )");

    const auto cfg = load_gw_config(bundle.root.string());
    ASSERT_TRUE(cfg);
    EXPECT_EQ(cfg->engine.ipc.name, "bundle-gw");
    EXPECT_EQ(cfg->engine.ipc.tcp_port, 19001);
    EXPECT_EQ(cfg->engine.ipc.heartbeat_interval, std::chrono::milliseconds(6000));
    EXPECT_EQ(cfg->engine.td.tq.broker_name, "from-entry");
    EXPECT_EQ(cfg->engine.td.tq.account_id, "bundle-account");
    EXPECT_TRUE(cfg->tq_auth.enabled);
}

TEST(TqApiConfigTest, LoadsNodeGroupFileWithIncludes) {
    TempConfigBundle bundle;
    bundle.write("ipc/gw-a.toml", R"(
        [ipc]
        name = "gw-a"
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
    )");
    bundle.write("node.toml", R"(
        [include]
        files = []
        primary_account = "default"
        [[accounts]]
        id = "default"
        ipc = "gw-a"
        [serve]
        port = 19002
    )");

    const auto cfg = load_node_config(bundle.root.string());
    ASSERT_TRUE(cfg);
    ASSERT_EQ(cfg->accounts.size(), 1u);
    EXPECT_EQ(cfg->accounts[0].ipc.tcp_port, 19001);
    EXPECT_EQ(cfg->serve.port, 19002);
}

TEST(TqApiConfigTest, MissingIncludeReturnsFileIoError) {
    TempConfigBundle bundle;
    bundle.write("gw.toml", R"(
        [include]
        files = ["shared/missing.toml"]
        [gateway]
        ipc = "gw-a"
        [ipc.gw-a]
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
    )");

    const auto cfg = load_gw_config(bundle.root.string());
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::file_io);
}

TEST(TqApiConfigTest, RejectsIncludeCycle) {
    TempConfigBundle bundle;
    bundle.write("a.toml", "[include]\nfiles = [\"b.toml\"]\n");
    bundle.write("b.toml", "[include]\nfiles = [\"a.toml\"]\n");
    bundle.write("gw.toml", R"(
        [include]
        files = ["a.toml"]
        [gateway]
        ipc = "gw-a"
        [ipc.gw-a]
        transport = "tcp"
        tcp_host = "127.0.0.1"
        tcp_port = 19001
    )");

    const auto cfg = load_gw_config(bundle.root.string());
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
    EXPECT_NE(cfg.error().detail.find("cycle"), std::string::npos);
}

TEST(TqApiConfigTest, RemoteEngineConfigUsesIpcGroup) {
    IpcConfig ipc;
    ipc.transport = IpcTransport::Tcp;
    ipc.tcp_host  = "10.0.0.8";
    ipc.tcp_port  = 14330;

    const auto cfg = make_remote_engine_config(ipc, "account-b");
    EXPECT_EQ(cfg.ipc.transport, IpcTransport::Tcp);
    EXPECT_EQ(cfg.ipc.tcp_host, "10.0.0.8");
    EXPECT_EQ(cfg.ipc.tcp_port, 14330);
    EXPECT_EQ(cfg.ipc.name, "node-account-b");
    EXPECT_EQ(cfg.md.kind, MdKind::Remote);
    EXPECT_EQ(cfg.td.kind, TdKind::Remote);
}

TEST(TqApiFactoryTest, BuildsSimEngine) {
    asio::io_context ctx;
    EngineConfig     cfg;
    cfg.td.kind                = TdKind::Sim;
    cfg.td.sim.initial_balance = 1'000'000.0;

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

    bool placed = false;
    auto body   = test::spawn_joined(ctx, [&]() -> asio::awaitable<void> {
        InsertOrderReq req;
        req.instrument_id = "SHFE.rb2701";
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

TEST(TqApiFactoryTest, InvalidTransportReported) {
    const auto cfg = parse_gw_config(R"(
        [gateway]
        ipc = "gw-a"
        [ipc.gw-a]
        transport = "smoke"
        [md]
        kind = "none"
        [td]
        kind = "none"
    )");
    ASSERT_FALSE(cfg);
    EXPECT_EQ(cfg.error().code, config::ConfigErrorCode::invalid_value);
}
