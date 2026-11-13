#pragma once

#include "tqsdk/pch.h"

#include "ipc_channle.h"

namespace tqsdk {
namespace ipc {

/// 流式 IPC 通道的公共实现（帧编解码），供 UDS / TCP 复用。
///
/// 把"帧格式 + 上限校验 + scatter/gather 读写"这一份协议逻辑集中在此，
/// 具体传输层（Unix domain socket / TCP）只需提供异步读写流并继承本类，
/// 从而保证两种传输下的帧语义完全一致（单一协议来源）。
///
/// Socket：满足 AsyncReadStream + AsyncWriteStream 的类型，例如
///         asio::local::stream_protocol::socket、asio::ip::tcp::socket。
template <typename Socket>
class StreamChannel : public IIpcChannel {
public:
    explicit StreamChannel(Socket sock)
        : sock_(std::move(sock)) {}

    asio::awaitable<IpcResult<void>>
    async_send(FrameType type, FrameEncoding encoding, ConstSlice payload) override {
        if (payload.size() > kMaxPayload) {
            co_return std::unexpected(
                IpcFailure{IpcErrorCode::protocol, "payload exceeds kMaxPayload"});
        }

        // 帧头：4B BE length + 1B type + 1B encoding
        // length 语义：type + encoding + payload 三段的字节数 = 2 + payload.size()
        std::array<uint8_t, kLenHdr + kMeta> hdr{};
        uint32_t                             len_be =
            boost::endian::native_to_big(static_cast<uint32_t>(kMeta + payload.size()));
        memcpy(hdr.data(), &len_be, kLenHdr);
        hdr[kLenHdr + 0] = static_cast<uint8_t>(type);
        hdr[kLenHdr + 1] = static_cast<uint8_t>(encoding);

        // scatter/gather：头一帧、payload 一帧，零拷贝（除头组装）
        std::array<asio::const_buffer, 2> bufs{asio::buffer(hdr),
                                               asio::buffer(payload.data(), payload.size())};

        boost::system::error_code ec;
        co_await asio::async_write(sock_, bufs, asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            co_return std::unexpected(IpcFailure{IpcErrorCode::closed, ec.message()});
        }
        co_return IpcResult<void>{};
    }

    asio::awaitable<IpcResult<std::optional<ReceivedFrame>>> async_recv() override {
        // ---- 1) 读 4 字节长度头 ----
        std::array<uint8_t, kLenHdr> len_buf{};
        boost::system::error_code    ec;
        co_await asio::async_read(
            sock_, asio::buffer(len_buf), asio::redirect_error(asio::use_awaitable, ec));

        if (ec == asio::error::eof || ec == asio::error::connection_reset) {
            co_return std::optional<ReceivedFrame>{};
        }
        if (ec) {
            co_return std::unexpected(IpcFailure{IpcErrorCode::closed, ec.message()});
        }

        uint32_t len_be = 0;
        memcpy(&len_be, len_buf.data(), kLenHdr);
        uint32_t length = boost::endian::big_to_native(len_be);

        // ---- 2) 校验 length ----
        if (length < kMeta) {
            co_return std::unexpected(
                IpcFailure{IpcErrorCode::protocol, "frame length < 2 (malformed)"});
        }
        if (length - kMeta > kMaxPayload) {
            co_return std::unexpected(
                IpcFailure{IpcErrorCode::protocol, "frame payload exceeds kMaxPayload"});
        }

        // ---- 3) 一次 async_read 读 [type | encoding | payload] ----
        //  payload 直接读进 ReceivedFrame，无二次拷贝
        ReceivedFrame frame;
        frame.payload.resize(length - kMeta);

        std::array<uint8_t, kMeta>          meta{};
        std::array<asio::mutable_buffer, 2> bufs{asio::buffer(meta), asio::buffer(frame.payload)};

        co_await asio::async_read(sock_, bufs, asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            co_return std::unexpected(IpcFailure{IpcErrorCode::closed, ec.message()});
        }

        frame.type     = static_cast<FrameType>(meta[0]);
        frame.encoding = static_cast<FrameEncoding>(meta[1]);
        co_return std::optional<ReceivedFrame>{std::move(frame)};
    }

    void close() noexcept override {
        boost::system::error_code ec;
        auto                      _ = sock_.close(ec);
    }

    Socket& socket() noexcept { return sock_; }

protected:
    // 2MB payload 上限，可按业务调；消息是行情/指令，不是文件块
    static constexpr uint32_t kMaxPayload = 2u << 20;
    static constexpr size_t   kLenHdr     = 4;
    static constexpr size_t   kMeta       = 2; // type + encoding

    Socket sock_;
};

} // namespace ipc
} // namespace tqsdk
