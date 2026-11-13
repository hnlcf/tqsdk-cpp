#include "tqsdk/pch.h"

#include "tqsdk/ipc/protocol.h"
#include "tqsdk/ipc/tcp_channel.h"
#include "tqsdk/ipc/uds_channel.h"

#include "support/test_helpers.h"

namespace tqsdk {
namespace ipc {
namespace {

// 说明：协程内不能用 gtest 的 ASSERT_*（其展开为 return;，协程里非法），
// 故用 "if 失败 -> ADD_FAILURE + co_return" 的模式，与 uds_channel_test 一致。

// ============================================================================
// 协议帧在真实通道上的端到端往返：encode_body -> async_send -> async_recv ->
// decode_as。覆盖 UDS 与 TCP 两种传输、JSON 与 MessagePack 两种编码。
// ============================================================================

// ---------------- UDS + JSON：握手 + 订阅 + 命令 + ack 全链路 ----------------

TEST(ProtocolE2E, UdsJsonFullHandshakeAndCommand) {
    std::string path = "/tmp/tqsdk_ipc_proto_e2e_" + std::to_string(::getpid()) + ".sock";
    ::unlink(path.c_str());

    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path});

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

        // 1) 收 Hello
        auto f1 = (co_await ch.async_recv()).value();
        if (!f1.has_value()) {
            ADD_FAILURE() << "srv: no Hello";
            co_return;
        }
        auto hello = decode_as<FrameType::Hello>(*f1);
        EXPECT_EQ(hello.role, IpcRole::Gateway);
        EXPECT_EQ(hello.version, kIpcProtocolVersion);
        auto [enc1, p1] = encode_frame<FrameType::HelloAck>(HelloAckBody{true, ""});
        co_await ch.async_send(FrameType::HelloAck, enc1, ConstSlice{p1});

        // 2) 收 Subscribe
        auto f2 = (co_await ch.async_recv()).value();
        if (!f2.has_value()) {
            ADD_FAILURE() << "srv: no Subscribe";
            co_return;
        }
        auto sub = decode_as<FrameType::Subscribe>(*f2);
        EXPECT_EQ(sub.symbols.size(), 1u);
        EXPECT_EQ(sub.symbols[0], "SHFE.rb2610");
        auto [enc2, p2] = encode_frame<FrameType::SubscribeAck>(SubscribeAckBody{true, ""});
        co_await ch.async_send(FrameType::SubscribeAck, enc2, ConstSlice{p2});

        // 3) 推一条行情
        MarketDataPushBody md;
        md.data         = {{"quotes", {{"SHFE.rb2610", {{"last_price", 3800.0}}}}}};
        auto [enc3, p3] = encode_frame<FrameType::MarketDataPush>(md);
        co_await ch.async_send(FrameType::MarketDataPush, enc3, ConstSlice{p3});

        // 4) 收 Command（下单）
        auto f4 = (co_await ch.async_recv()).value();
        if (!f4.has_value()) {
            ADD_FAILURE() << "srv: no Command";
            co_return;
        }
        auto cmd = decode_as<FrameType::Command>(*f4);
        EXPECT_EQ(cmd.op, CommandOp::InsertOrder);
        EXPECT_EQ(cmd.rid, "rid_1");
        EXPECT_EQ(cmd.insert.instrument_id, "SHFE.rb2610");
        EXPECT_EQ(cmd.insert.volume, 3u);

        CommandAckBody ack;
        ack.ack.success  = true;
        ack.ack.order_id = cmd.rid;
        auto [enc4, p4]  = encode_frame<FrameType::CommandAck>(ack);
        co_await ch.async_send(FrameType::CommandAck, enc4, ConstSlice{p4});
        ch.close();
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path);

        auto [e1, b1] = encode_frame<FrameType::Hello>(HelloBody{1, IpcRole::Gateway, "gw"});
        co_await ch->async_send(FrameType::Hello, e1, ConstSlice{b1});
        auto r1 = (co_await ch->async_recv()).value();
        if (!r1.has_value()) {
            ADD_FAILURE() << "cli: no HelloAck";
            co_return;
        }
        EXPECT_TRUE(decode_as<FrameType::HelloAck>(*r1).ok);

        SubscribeBody sub;
        sub.symbols   = {"SHFE.rb2610"};
        auto [e2, b2] = encode_frame<FrameType::Subscribe>(sub);
        co_await ch->async_send(FrameType::Subscribe, e2, ConstSlice{b2});
        auto r2 = (co_await ch->async_recv()).value();
        if (!r2.has_value()) {
            ADD_FAILURE() << "cli: no SubscribeAck";
            co_return;
        }
        EXPECT_TRUE(decode_as<FrameType::SubscribeAck>(*r2).ok);

        auto r3 = (co_await ch->async_recv()).value();
        if (!r3.has_value()) {
            ADD_FAILURE() << "cli: no MarketDataPush";
            co_return;
        }
        auto md = decode_as<FrameType::MarketDataPush>(*r3);
        EXPECT_DOUBLE_EQ(md.data["quotes"]["SHFE.rb2610"]["last_price"].get<double>(), 3800.0);

        CommandBody cmd;
        cmd.op                   = CommandOp::InsertOrder;
        cmd.rid                  = "rid_1";
        cmd.insert.instrument_id = "SHFE.rb2610";
        cmd.insert.direction     = Direction::BUY;
        cmd.insert.offset        = Offset::OPEN;
        cmd.insert.price_type    = PriceType::LIMIT;
        cmd.insert.limit_price   = 3799.0;
        cmd.insert.volume        = 3;
        auto [e4, b4]            = encode_frame<FrameType::Command>(cmd);
        co_await ch->async_send(FrameType::Command, e4, ConstSlice{b4});
        auto r4 = (co_await ch->async_recv()).value();
        if (!r4.has_value()) {
            ADD_FAILURE() << "cli: no CommandAck";
            co_return;
        }
        auto ack = decode_as<FrameType::CommandAck>(*r4);
        EXPECT_TRUE(ack.ack.success);
        EXPECT_EQ(ack.ack.order_id, "rid_1");
        ch->close();
    });

    io.run();
    try {
        srv.get();
    } catch (const std::exception& e) {
        FAIL() << "srv: " << e.what();
    }
    try {
        cli.get();
    } catch (const std::exception& e) {
        FAIL() << "cli: " << e.what();
    }
    ::unlink(path.c_str());
}

// ---------------- TCP + MessagePack：换传输 + 换编码都无障碍 ----------------

TEST(ProtocolE2E, TcpMsgpackHandshake) {
    asio::io_context        io;
    asio::ip::tcp::acceptor acc(io);
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    acc.open(ep.protocol());
    acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acc.bind(ep);
    acc.listen();
    uint16_t port = acc.local_endpoint().port();

    constexpr auto kEnc = FrameEncoding::MessagePack;

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        auto f = (co_await ch.async_recv()).value();
        if (!f.has_value()) {
            ADD_FAILURE() << "srv: no Hello";
            co_return;
        }
        EXPECT_EQ(f->encoding, kEnc);
        EXPECT_EQ(decode_as<FrameType::Hello>(*f).name, "tcp-gw");
        auto [e, b] = encode_frame<FrameType::HelloAck>(HelloAckBody{true, ""}, kEnc);
        co_await ch.async_send(FrameType::HelloAck, e, ConstSlice{b});

        auto f2 = (co_await ch.async_recv()).value();
        if (!f2.has_value()) {
            ADD_FAILURE() << "srv: no Subscribe";
            co_return;
        }
        auto sub = decode_as<FrameType::Subscribe>(*f2);
        EXPECT_EQ(sub.klines.size(), 1u);
        EXPECT_EQ(sub.klines[0].view_width, 100);
        auto [e2, b2] = encode_frame<FrameType::SubscribeAck>(SubscribeAckBody{true, ""}, kEnc);
        co_await ch.async_send(FrameType::SubscribeAck, e2, ConstSlice{b2});
        ch.close();
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io, "127.0.0.1", port);

        auto [e, b] =
            encode_frame<FrameType::Hello>(HelloBody{1, IpcRole::Gateway, "tcp-gw"}, kEnc);
        co_await ch->async_send(FrameType::Hello, e, ConstSlice{b});
        auto r = (co_await ch->async_recv()).value();
        if (!r.has_value()) {
            ADD_FAILURE() << "cli: no HelloAck";
            co_return;
        }
        EXPECT_TRUE(decode_as<FrameType::HelloAck>(*r).ok);

        SubscribeBody sub;
        sub.klines    = {{"DCE.i2601", 60'000'000'000LL, 100}};
        auto [e2, b2] = encode_frame<FrameType::Subscribe>(sub, kEnc);
        co_await ch->async_send(FrameType::Subscribe, e2, ConstSlice{b2});
        auto r2 = (co_await ch->async_recv()).value();
        if (!r2.has_value()) {
            ADD_FAILURE() << "cli: no SubscribeAck";
            co_return;
        }
        EXPECT_TRUE(decode_as<FrameType::SubscribeAck>(*r2).ok);
        ch->close();
    });

    io.run();
    try {
        srv.get();
    } catch (const std::exception& e) {
        FAIL() << "srv: " << e.what();
    }
    try {
        cli.get();
    } catch (const std::exception& e) {
        FAIL() << "cli: " << e.what();
    }
}

// ---------------- 大量帧连续往返，验证流式分帧正确 ----------------

TEST(ProtocolE2E, UdsManyFramesStream) {
    std::string path = "/tmp/tqsdk_ipc_proto_stream_" + std::to_string(::getpid()) + ".sock";
    ::unlink(path.c_str());

    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path});
    constexpr int                          kCount = 200;

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));
        for (int i = 0; i < kCount; ++i) {
            auto f = (co_await ch.async_recv()).value();
            if (!f.has_value()) {
                ADD_FAILURE() << "srv: eof at " << i;
                co_return;
            }
            auto md = decode_as<FrameType::MarketDataPush>(*f);
            EXPECT_EQ(md.data["i"].get<int>(), i);
        }
        ch.close();
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path);
        for (int i = 0; i < kCount; ++i) {
            MarketDataPushBody md;
            md.data     = {{"i", i}};
            auto [e, b] = encode_frame<FrameType::MarketDataPush>(md);
            co_await ch->async_send(FrameType::MarketDataPush, e, ConstSlice{b});
        }
        ch->close();
    });

    io.run();
    try {
        srv.get();
    } catch (const std::exception& e) {
        FAIL() << "srv: " << e.what();
    }
    try {
        cli.get();
    } catch (const std::exception& e) {
        FAIL() << "cli: " << e.what();
    }
    ::unlink(path.c_str());
}

} // namespace
} // namespace ipc
} // namespace tqsdk
