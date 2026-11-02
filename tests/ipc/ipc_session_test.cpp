#include "tqsdk/pch.h"

#include "tqsdk/ipc/ipc_session.h"
#include "tqsdk/ipc/uds_channel.h"

#include "support/test_helpers.h"

namespace tqsdk {
namespace ipc {
namespace {

// 协程内用 "if 失败 -> ADD_FAILURE + co_return"，禁用 ASSERT_*。
//
// 重要：IpcSession 启动后其后台协程（读/心跳/看门狗）会长期持有 pending 的
// timer 与 socket 读，使 io_context 永远不会自己排空。因此本文件里**绝不使用
// 无参 io.run() 去等待某操作完成**（会永远阻塞），而是统一用
// test::drive_until / test::wait_all 分片驱动。

std::string make_uds_path(const char* tag) {
    static std::atomic<int> c{0};
    return "/tmp/tqsdk_ipc_session_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(c.fetch_add(1)) + ".sock";
}

// 建立一对已连接的 UDS channel（server 端 + client 端）。
// 此阶段尚无 IpcSession 的后台协程，io.run() 可安全排空。
struct UdsPair {
    std::unique_ptr<IIpcChannel> srv;
    std::unique_ptr<IIpcChannel> cli;
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
    accept.get(); // 失败则抛
    return {std::move(srv), std::move(cli)};
}

// 默认两端 Options（Node/Gateway，长心跳，避免干扰）。
IpcSession::Options default_srv_opt() {
    IpcSession::Options o;
    o.local_role         = IpcRole::Node;
    o.local_name         = "node";
    o.handshake_timeout  = std::chrono::seconds(2);
    o.heartbeat_interval = std::chrono::seconds(60);
    o.heartbeat_timeout  = std::chrono::seconds(120);
    return o;
}
IpcSession::Options default_cli_opt() {
    IpcSession::Options o = default_srv_opt();
    o.local_role          = IpcRole::Gateway;
    o.local_name          = "gw";
    return o;
}

// ---------------------------------------------------------------------------
// 1) 握手成功：两端都进入 ready
// ---------------------------------------------------------------------------
TEST(IpcSessionTest, HandshakeReadyOnBothSides) {
    std::string      path = make_uds_path("hs");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession srv_sess(std::move(pair.srv), io.get_executor(), default_srv_opt());
    IpcSession cli_sess(std::move(pair.cli), io.get_executor(), default_cli_opt());

    auto srv_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await srv_sess.start(); });
    auto cli_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await cli_sess.start(); });

    test::wait_all(io, std::chrono::seconds(2), srv_start, cli_start);
    try {
        srv_start.get();
        cli_start.get();
    } catch (const std::exception& e) {
        FAIL() << "start: " << e.what();
    }

    EXPECT_TRUE(srv_sess.is_ready());
    EXPECT_TRUE(cli_sess.is_ready());
    EXPECT_TRUE(srv_sess.is_open());
    EXPECT_TRUE(cli_sess.is_open());

    srv_sess.close();
    cli_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 2) 业务帧投递：client -> server 一条 MarketDataPush
// ---------------------------------------------------------------------------
TEST(IpcSessionTest, BusinessFrameDelivered) {
    std::string      path = make_uds_path("biz");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession srv_sess(std::move(pair.srv), io.get_executor(), default_srv_opt());
    IpcSession cli_sess(std::move(pair.cli), io.get_executor(), default_cli_opt());

    auto srv_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await srv_sess.start(); });
    auto cli_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await cli_sess.start(); });
    test::wait_all(io, std::chrono::seconds(2), srv_start, cli_start);
    try {
        srv_start.get();
        cli_start.get();
    } catch (const std::exception& e) {
        FAIL() << "start: " << e.what();
    }
    ASSERT_TRUE(srv_sess.is_ready());

    MarketDataPushBody md;
    md.data = {{"quotes", {{"SHFE.rb2610", {{"last_price", 3811.0}}}}}};

    std::optional<ReceivedFrame> got;
    auto                         sender   = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await cli_sess.send_body<FrameType::MarketDataPush>(md);
    });
    auto                         receiver = test::spawn_joined(
        io, [&]() -> asio::awaitable<void> { got = co_await srv_sess.incoming().async_receive(); });

    test::wait_all(io, std::chrono::seconds(2), sender, receiver);
    try {
        sender.get();
        receiver.get();
    } catch (const std::exception& e) {
        FAIL() << "io: " << e.what();
    }
    ASSERT_TRUE(got.has_value());
    auto decoded = decode_as<FrameType::MarketDataPush>(*got);
    EXPECT_DOUBLE_EQ(decoded.data["quotes"]["SHFE.rb2610"]["last_price"].get<double>(), 3811.0);

    srv_sess.close();
    cli_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 3) 角色冲突：两端相同角色 -> 握手失败
// ---------------------------------------------------------------------------
TEST(IpcSessionTest, RoleConflictFailsHandshake) {
    std::string      path = make_uds_path("role");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession::Options a;
    a.local_role          = IpcRole::Gateway; // 两端都 Gateway -> 冲突
    a.local_name          = "g1";
    a.handshake_timeout   = std::chrono::milliseconds(500);
    a.heartbeat_interval  = std::chrono::seconds(60);
    a.heartbeat_timeout   = std::chrono::seconds(120);
    IpcSession::Options b = a;
    b.local_name          = "g2";

    IpcSession srv_sess(std::move(pair.srv), io.get_executor(), a);
    IpcSession cli_sess(std::move(pair.cli), io.get_executor(), b);

    bool srv_failed = false;
    bool cli_failed = false;
    auto srv_start  = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        try {
            co_await srv_sess.start();
        } catch (const IpcError&) {
            srv_failed = true;
        }
    });
    auto cli_start  = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        try {
            co_await cli_sess.start();
        } catch (const IpcError&) {
            cli_failed = true;
        }
    });
    test::wait_all(io, std::chrono::seconds(2), srv_start, cli_start);
    srv_start.rethrow_if_error();
    cli_start.rethrow_if_error();

    EXPECT_TRUE(srv_failed || cli_failed);
    EXPECT_FALSE(srv_sess.is_ready() && cli_sess.is_ready());

    srv_sess.close();
    cli_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 4) 心跳保活：短 interval、长 timeout 不应断线，tx/rx 计数增长
// ---------------------------------------------------------------------------
TEST(IpcSessionTest, HeartbeatKeepsAlive) {
    std::string      path = make_uds_path("hb");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession::Options opt;
    opt.local_role           = IpcRole::Node;
    opt.local_name           = "node";
    opt.handshake_timeout    = std::chrono::seconds(1);
    opt.heartbeat_interval   = std::chrono::milliseconds(30);
    opt.heartbeat_timeout    = std::chrono::milliseconds(300);
    IpcSession::Options opt2 = opt;
    opt2.local_role          = IpcRole::Gateway;
    opt2.local_name          = "gw";

    IpcSession srv_sess(std::move(pair.srv), io.get_executor(), opt);
    IpcSession cli_sess(std::move(pair.cli), io.get_executor(), opt2);

    auto srv_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await srv_sess.start(); });
    auto cli_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await cli_sess.start(); });
    test::wait_all(io, std::chrono::seconds(1), srv_start, cli_start);
    try {
        srv_start.get();
        cli_start.get();
    } catch (const std::exception& e) {
        FAIL() << "start: " << e.what();
    }

    // 让心跳往返若干轮（> 数个 interval）
    test::drive_for(io, std::chrono::milliseconds(200));

    EXPECT_TRUE(srv_sess.is_open()); // 心跳保活，未因超时关闭
    EXPECT_TRUE(cli_sess.is_open());
    EXPECT_GT(srv_sess.rx_frames(), 0u);
    EXPECT_GT(cli_sess.tx_frames(), 0u);

    srv_sess.close();
    cli_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 5) 对端 Goodbye -> 本端关闭
// ---------------------------------------------------------------------------
TEST(IpcSessionTest, GoodbyeClosesPeer) {
    std::string      path = make_uds_path("bye");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession srv_sess(std::move(pair.srv), io.get_executor(), default_srv_opt());
    IpcSession cli_sess(std::move(pair.cli), io.get_executor(), default_cli_opt());

    auto srv_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await srv_sess.start(); });
    auto cli_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await cli_sess.start(); });
    test::wait_all(io, std::chrono::seconds(2), srv_start, cli_start);
    try {
        srv_start.get();
        cli_start.get();
    } catch (const std::exception& e) {
        FAIL() << "start: " << e.what();
    }

    auto sender = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        co_await cli_sess.send_body<FrameType::Goodbye>(GoodbyeBody{"done"});
    });
    test::wait_all(io, std::chrono::seconds(2), sender);
    sender.rethrow_if_error();

    // 驱动直到 server 端检测到关闭（读循环收到 Goodbye）。
    test::drive_until(io, [&]() { return !srv_sess.is_open(); }, std::chrono::seconds(1));
    EXPECT_FALSE(srv_sess.is_open()); // server 收到 Goodbye 后应关闭

    srv_sess.close();
    cli_sess.close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// 6) 心跳超时：对端长时间无入站 -> 看门狗判超时关闭
//
// 注意：两个真实 IpcSession 互相连接时，一方发 Heartbeat 另一方会自动回
// HeartbeatAck，看门狗被持续刷新，不会超时（这是正确行为）。因此本测试的
// 对端用一个"握手后装死"的裸 channel（不再读也不回应），以模拟对端静默。
// ---------------------------------------------------------------------------
TEST(IpcSessionTest, HeartbeatTimeoutCloses) {
    std::string      path = make_uds_path("to");
    asio::io_context io;
    auto             pair = make_uds_pair(io, path);

    IpcSession::Options srv_opt;
    srv_opt.local_role         = IpcRole::Node;
    srv_opt.local_name         = "node";
    srv_opt.handshake_timeout  = std::chrono::seconds(1);
    srv_opt.heartbeat_interval = std::chrono::milliseconds(20);
    srv_opt.heartbeat_timeout  = std::chrono::milliseconds(120);

    IpcSession                   srv_sess(std::move(pair.srv), io.get_executor(), srv_opt);
    std::unique_ptr<IIpcChannel> peer = std::move(pair.cli);

    // 对端：发 Hello 完成握手，读到 HelloAck 后彻底装死（不再读、不再发）。
    auto peer_hello = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto hello_bytes = encode_body(HelloBody{kIpcProtocolVersion, IpcRole::Gateway, "silent"},
                                       srv_opt.encoding);
        co_await peer->async_send(FrameType::Hello, srv_opt.encoding, ConstSlice{hello_bytes});
        auto ack = co_await peer->async_recv(); // 读掉 HelloAck
        (void)ack;
    });

    auto srv_start =
        test::spawn_joined(io, [&]() -> asio::awaitable<void> { co_await srv_sess.start(); });

    test::wait_all(io, std::chrono::seconds(2), srv_start, peer_hello);
    srv_start.get();
    peer_hello.rethrow_if_error();
    ASSERT_TRUE(srv_sess.is_ready());

    // 对端不再产生任何入站帧，驱动直到 server 看门狗判定超时关闭。
    test::drive_until(io, [&]() { return !srv_sess.is_open(); }, std::chrono::seconds(1));
    EXPECT_FALSE(srv_sess.is_open());

    srv_sess.close();
    peer->close();
    test::drive_for(io, std::chrono::milliseconds(30));
    ::unlink(path.c_str());
}

} // namespace
} // namespace ipc
} // namespace tqsdk
