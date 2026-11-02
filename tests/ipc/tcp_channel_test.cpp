#include "tqsdk/pch.h"

#include "tqsdk/ipc/tcp_channel.h"

#include "support/test_helpers.h"

namespace tqsdk {
namespace ipc {
namespace {

using tcp = asio::ip::tcp;

// 绑定到本地回环的临时端口（port=0 由内核分配）。
uint16_t bind_ephemeral(tcp::acceptor& acc, asio::io_context& io) {
    tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    acc.open(ep.protocol());
    acc.set_option(tcp::acceptor::reuse_address(true));
    acc.bind(ep);
    acc.listen();
    return acc.local_endpoint().port();
}

class TcpChannelTest : public ::testing::Test {
protected:
    asio::io_context io_;
};

// 1) 空 payload 往返
TEST_F(TcpChannelTest, RoundTripEmptyPayload) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        auto f = co_await ch.async_recv();
        if (!f.has_value()) {
            ADD_FAILURE() << "srv: no Hello";
            co_return;
        }
        EXPECT_EQ(f->type, FrameType::Hello);
        EXPECT_EQ(f->encoding, FrameEncoding::Json);
        EXPECT_TRUE(f->payload.empty());

        co_await ch.async_send(FrameType::HelloAck, FrameEncoding::Json, {});
        ch.close();
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        co_await ch->async_send(FrameType::Hello, FrameEncoding::Json, {});
        auto f = co_await ch->async_recv();
        if (!f.has_value()) {
            ADD_FAILURE() << "cli: no HelloAck";
            co_return;
        }
        EXPECT_EQ(f->type, FrameType::HelloAck);
        EXPECT_EQ(f->encoding, FrameEncoding::Json);
        EXPECT_TRUE(f->payload.empty());
        ch->close();
    });

    io_.run();
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

// 2) 带内容往返
TEST_F(TcpChannelTest, RoundTripWithPayload) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    const std::vector<uint8_t> req = {'h', 'e', 'l', 'l', 'o'};
    const std::vector<uint8_t> rsp = {'w', 'o', 'r', 'l', 'd', '!', '!', '!'};

    auto srv = test::spawn_joined(io_, [&, req, rsp]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        auto f = co_await ch.async_recv();
        if (!f.has_value()) {
            ADD_FAILURE() << "srv: no frame";
            co_return;
        }
        EXPECT_EQ(f->type, FrameType::Command);
        EXPECT_EQ(f->payload, req);

        co_await ch.async_send(FrameType::CommandAck, f->encoding, ConstSlice{rsp});
        ch.close();
    });

    auto cli = test::spawn_joined(io_, [&, req, rsp]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        co_await ch->async_send(FrameType::Command, FrameEncoding::MessagePack, ConstSlice{req});
        auto f = co_await ch->async_recv();
        if (!f.has_value()) {
            ADD_FAILURE() << "cli: no ack";
            co_return;
        }
        EXPECT_EQ(f->type, FrameType::CommandAck);
        EXPECT_EQ(f->encoding, FrameEncoding::MessagePack);
        EXPECT_EQ(f->payload, rsp);
        ch->close();
    });

    io_.run();
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

// 3) 大 payload（1MB）
TEST_F(TcpChannelTest, LargePayloadRoundTrip) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    constexpr std::size_t kBig = 1u << 20;
    std::vector<uint8_t>  big(kBig);
    for (std::size_t i = 0; i < kBig; ++i)
        big[i] = static_cast<uint8_t>(i & 0xff);

    auto srv = test::spawn_joined(io_, [&, big]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        auto f = co_await ch.async_recv();
        if (!f.has_value()) {
            ADD_FAILURE() << "srv: no frame";
            co_return;
        }
        if (f->payload.size() != kBig) {
            ADD_FAILURE() << "srv: size mismatch";
            co_return;
        }
        EXPECT_EQ(f->payload, big);

        co_await ch.async_send(FrameType::Diff, f->encoding, ConstSlice{big});
        ch.close();
    });

    auto cli = test::spawn_joined(io_, [&, big]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        co_await ch->async_send(FrameType::Diff, FrameEncoding::MessagePack, ConstSlice{big});
        auto f = co_await ch->async_recv();
        if (!f.has_value()) {
            ADD_FAILURE() << "cli: no frame";
            co_return;
        }
        if (f->payload.size() != kBig) {
            ADD_FAILURE() << "cli: size mismatch";
            co_return;
        }
        EXPECT_EQ(f->payload, big);
        ch->close();
    });

    io_.run();
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

// 4) 对端关闭 → async_recv 返回 nullopt
TEST_F(TcpChannelTest, RecvReturnsNulloptOnPeerClose) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto                      sock = co_await acc.async_accept(asio::use_awaitable);
        boost::system::error_code ec;
        sock.close(ec);
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        auto f  = co_await ch->async_recv();
        EXPECT_FALSE(f.has_value());
    });

    io_.run();
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

// 5) 超限发送 → 抛 IpcError
TEST_F(TcpChannelTest, SendTooLargePayloadThrows) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto                      sock = co_await acc.async_accept(asio::use_awaitable);
        std::array<uint8_t, 16>   buf{};
        boost::system::error_code ec;
        co_await asio::async_read(
            sock, asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto                  ch      = TcpChannel::connect(io_, "127.0.0.1", port);
        constexpr std::size_t kTooBig = 4u << 20;
        std::vector<uint8_t>  huge(kTooBig);

        bool threw = false;
        try {
            co_await ch->async_send(FrameType::Diff, FrameEncoding::MessagePack, ConstSlice{huge});
        } catch (const IpcError&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        ch->close();
    });

    io_.run();
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

// 6) 非法帧：length < kMeta
TEST_F(TcpChannelTest, MalformedFrameTooShortThrows) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        bool threw = false;
        try {
            co_await ch.async_recv();
        } catch (const IpcError& e) {
            threw = true;
            EXPECT_NE(std::string(e.what()).find("length"), std::string::npos);
        }
        EXPECT_TRUE(threw);
        ch.close();
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        tcp::socket sock(io_);
        sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        std::array<uint8_t, 4> bad = {0x00, 0x00, 0x00, 0x01};
        co_await asio::async_write(sock, asio::buffer(bad), asio::use_awaitable);
    });

    io_.run();
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

// 7) 非法帧：length 超过 kMaxPayload
TEST_F(TcpChannelTest, MalformedFrameTooLongThrows) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        bool threw = false;
        try {
            co_await ch.async_recv();
        } catch (const IpcError& e) {
            threw = true;
            EXPECT_NE(std::string(e.what()).find("payload"), std::string::npos);
        }
        EXPECT_TRUE(threw);
        ch.close();
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        tcp::socket sock(io_);
        sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        std::uint32_t          length = 3u << 20;
        std::array<uint8_t, 4> buf    = {static_cast<uint8_t>((length >> 24) & 0xff),
                                         static_cast<uint8_t>((length >> 16) & 0xff),
                                         static_cast<uint8_t>((length >> 8) & 0xff),
                                         static_cast<uint8_t>(length & 0xff)};
        co_await asio::async_write(sock, asio::buffer(buf), asio::use_awaitable);
        std::array<uint8_t, 1>    dummy{};
        boost::system::error_code ec;
        co_await asio::async_read(
            sock, asio::buffer(dummy), asio::redirect_error(asio::use_awaitable, ec));
    });

    io_.run();
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

// 8) 连接到未监听端口 → 抛异常
TEST_F(TcpChannelTest, ConnectToClosedPortThrows) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);
    acc.close(); // 关闭监听，端口不再有人 accept

    EXPECT_THROW(TcpChannel::connect(io_, "127.0.0.1", port), boost::system::system_error);
}

// 9) 多次 close 幂等
TEST_F(TcpChannelTest, CloseIsIdempotent) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    std::atomic<bool> srv_ok{false};
    std::atomic<bool> cli_ok{false};

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));
        ch.close();
        try {
            ch.close();
            srv_ok = true;
        } catch (...) {
            srv_ok = false;
        }
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        co_await asio::post(io_, asio::use_awaitable);
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        ch->close();
        try {
            ch->close();
            cli_ok = true;
        } catch (...) {
            cli_ok = false;
        }
    });

    io_.run();
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
    EXPECT_TRUE(srv_ok);
    EXPECT_TRUE(cli_ok);
}

// 10) 关闭后 recv 抛 IpcError
TEST_F(TcpChannelTest, RecvAfterCloseThrows) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        ch->close();
        bool threw = false;
        try {
            co_await ch->async_recv();
        } catch (const IpcError&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    });

    io_.run();
    try {
        cli.get();
    } catch (const std::exception& e) {
        FAIL() << "cli: " << e.what();
    }
}

// 11) 双向并发，验证顺序 + 内容
TEST_F(TcpChannelTest, BidirectionalConcurrent) {
    tcp::acceptor acc(io_);
    uint16_t      port = bind_ephemeral(acc, io_);

    constexpr int kCount = 100;

    auto srv = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        TcpChannel ch(std::move(sock));

        for (int i = 0; i < kCount; ++i) {
            auto f = co_await ch.async_recv();
            if (!f.has_value()) {
                ADD_FAILURE() << "srv: eof at " << i;
                co_return;
            }
            if (f->payload.size() != sizeof(int)) {
                ADD_FAILURE() << "srv: bad size at " << i;
                co_return;
            }
            int v;
            std::memcpy(&v, f->payload.data(), sizeof(int));
            EXPECT_EQ(v, i);
            co_await ch.async_send(f->type, f->encoding, ConstSlice{f->payload});
        }

        auto eof = co_await ch.async_recv();
        EXPECT_FALSE(eof.has_value());
        ch.close();
    });

    auto cli = test::spawn_joined(io_, [&]() -> asio::awaitable<void> {
        auto ch = TcpChannel::connect(io_, "127.0.0.1", port);
        for (int i = 0; i < kCount; ++i) {
            std::array<uint8_t, sizeof(int)> buf{};
            std::memcpy(buf.data(), &i, sizeof(int));
            co_await ch->async_send(FrameType::Command, FrameEncoding::Json, ConstSlice{buf});
            auto f = co_await ch->async_recv();
            if (!f.has_value()) {
                ADD_FAILURE() << "cli: eof at " << i;
                co_return;
            }
            if (f->payload.size() != sizeof(int)) {
                ADD_FAILURE() << "cli: bad size at " << i;
                co_return;
            }
            int v;
            std::memcpy(&v, f->payload.data(), sizeof(int));
            EXPECT_EQ(v, i);
        }
        ch->close();
    });

    io_.run();
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

} // namespace
} // namespace ipc
} // namespace tqsdk
