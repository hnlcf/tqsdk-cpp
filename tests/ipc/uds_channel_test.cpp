#include "tqsdk/pch.h"

#include "tqsdk/ipc/uds_channel.h"

#include "support/test_helpers.h"

namespace tqsdk {
namespace ipc {
namespace {

std::string make_test_path() {
    static std::atomic<int> counter{0};
    return "/tmp/tqsdk_ipc_uds_test_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1)) + ".sock";
}

class UdsChannelTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = make_test_path();
        ::unlink(path_.c_str());
    }
    void        TearDown() override { ::unlink(path_.c_str()); }
    std::string path_;
};

// 说明：本文件统一用 test::spawn_joined 代替 asio::use_future。
// use_future 在 Boost 1.92 + clang + ASan 下处理 void(exception_ptr) 完成签名
// 会崩溃（promise_handler 生命周期缺陷），spawn_joined 用自持 promise 规避。

// ---------------- 1) 空 payload 往返 ----------------
TEST_F(UdsChannelTest, RoundTripEmptyPayload) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

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

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path_);
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

// ---------------- 2) 带内容往返 ----------------
TEST_F(UdsChannelTest, RoundTripWithPayload) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    const std::vector<uint8_t> req = {'h', 'e', 'l', 'l', 'o'};
    const std::vector<uint8_t> rsp = {'w', 'o', 'r', 'l', 'd', '!', '!', '!'};

    auto srv = test::spawn_joined(io, [&, req, rsp]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

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

    auto cli = test::spawn_joined(io, [&, req, rsp]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path_);
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

// ---------------- 3) 大 payload（1MB，落在 2MB 限制内） ----------------
TEST_F(UdsChannelTest, LargePayloadRoundTrip) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    constexpr std::size_t kBig = 1u << 20;
    std::vector<uint8_t>  big(kBig);
    for (std::size_t i = 0; i < kBig; ++i) {
        big[i] = static_cast<uint8_t>(i & 0xff);
    }

    auto srv = test::spawn_joined(io, [&, big]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

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

    auto cli = test::spawn_joined(io, [&, big]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path_);
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

// ---------------- 4) 对端关闭 → async_recv 返回 nullopt ----------------
TEST_F(UdsChannelTest, RecvReturnsNulloptOnPeerClose) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto                      sock = co_await acc.async_accept(asio::use_awaitable);
        boost::system::error_code ec;
        sock.close(ec); // 只关，不发
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path_);
        auto f  = co_await ch->async_recv();
        EXPECT_FALSE(f.has_value());
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

// ---------------- 5) 超限发送 → 抛 IpcError ----------------
TEST_F(UdsChannelTest, SendTooLargePayloadThrows) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto sock = co_await acc.async_accept(asio::use_awaitable);
        // 客户端会抛异常后立即 close，服务端读个 eof 即可退出
        std::array<uint8_t, 16>   buf{};
        boost::system::error_code ec;
        co_await asio::async_read(
            sock, asio::buffer(buf), asio::redirect_error(asio::use_awaitable, ec));
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto                  ch      = UdsChannel::connect(io, path_);
        constexpr std::size_t kTooBig = 4u << 20; // 4MB > kMaxPayload(2MB)
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

// ---------------- 6) 非法帧：length < kMeta(2) ----------------
TEST_F(UdsChannelTest, MalformedFrameTooShortThrows) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

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

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        asio::local::stream_protocol::socket sock(io);
        sock.connect(asio::local::stream_protocol::endpoint(path_));
        // 直接写非法 length 头：BE 0x00000001
        std::array<uint8_t, 4> bad = {0x00, 0x00, 0x00, 0x01};
        co_await asio::async_write(sock, asio::buffer(bad), asio::use_awaitable);
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

// ---------------- 7) 非法帧：length 超过 kMaxPayload ----------------
TEST_F(UdsChannelTest, MalformedFrameTooLongThrows) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

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

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        asio::local::stream_protocol::socket sock(io);
        sock.connect(asio::local::stream_protocol::endpoint(path_));
        // length = 3MB，超过 2MB 上限
        std::uint32_t          length = 3u << 20;
        std::array<uint8_t, 4> buf    = {static_cast<uint8_t>((length >> 24) & 0xff),
                                         static_cast<uint8_t>((length >> 16) & 0xff),
                                         static_cast<uint8_t>((length >> 8) & 0xff),
                                         static_cast<uint8_t>(length & 0xff)};
        co_await asio::async_write(sock, asio::buffer(buf), asio::use_awaitable);
        // 等服务端抛异常后关闭（async_read 会得到 eof）
        std::array<uint8_t, 1>    dummy{};
        boost::system::error_code ec;
        co_await asio::async_read(
            sock, asio::buffer(dummy), asio::redirect_error(asio::use_awaitable, ec));
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

// ---------------- 8) connect 到不存在的路径 ----------------
TEST_F(UdsChannelTest, ConnectToNonexistentPathThrows) {
    asio::io_context io;
    // 没有 acceptor，路径不存在
    EXPECT_THROW(UdsChannel::connect(io, path_), boost::system::system_error);
}

// ---------------- 9) 多次 close 幂等 ----------------
TEST_F(UdsChannelTest, CloseIsIdempotent) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    std::atomic<bool> srv_second_close_ok{false};
    std::atomic<bool> cli_second_close_ok{false};

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));
        ch.close();
        try {
            ch.close(); // 第二次
            srv_second_close_ok = true;
        } catch (...) {
            srv_second_close_ok = false;
        }
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        // 先让出一次，避免"全程无挂起"的协程在 co_spawn 内同步跑完
        // 触发 Boost.Asio 1.92 的 eager-completion 断言。
        co_await asio::post(io, asio::use_awaitable);
        auto ch = UdsChannel::connect(io, path_);
        ch->close();
        try {
            ch->close();
            cli_second_close_ok = true;
        } catch (...) {
            cli_second_close_ok = false;
        }
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

    EXPECT_TRUE(srv_second_close_ok);
    EXPECT_TRUE(cli_second_close_ok);
}

// ---------------- 10) 关闭后 recv 应抛 IpcError ----------------
TEST_F(UdsChannelTest, RecvAfterCloseThrows) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});
    // 不用 accept：connect 会进 listen queue，随后 close 即可

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path_);
        ch->close();
        bool threw = false;
        try {
            co_await ch->async_recv();
        } catch (const IpcError&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    });

    io.run();
    try {
        cli.get();
    } catch (const std::exception& e) {
        FAIL() << "cli: " << e.what();
    }
}

// ---------------- 11) 双向并发，验证顺序 + 内容 ----------------
TEST_F(UdsChannelTest, BidirectionalConcurrent) {
    asio::io_context                       io;
    asio::local::stream_protocol::acceptor acc(io, {path_});

    constexpr int kCount = 100;

    auto srv = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto       sock = co_await acc.async_accept(asio::use_awaitable);
        UdsChannel ch(std::move(sock));

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

        // 等 client 关闭
        auto eof = co_await ch.async_recv();
        EXPECT_FALSE(eof.has_value());
        ch.close();
    });

    auto cli = test::spawn_joined(io, [&]() -> asio::awaitable<void> {
        auto ch = UdsChannel::connect(io, path_);
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

} // namespace
} // namespace ipc
} // namespace tqsdk
