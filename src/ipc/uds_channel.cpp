#include "tqsdk/ipc/uds_channel.h"

namespace tqsdk {
namespace ipc {

std::unique_ptr<UdsChannel> UdsChannel::connect(asio::io_context& io, const std::string& path) {
    Socket s(io);
    s.connect(asio::local::stream_protocol::endpoint(path));
    return std::make_unique<UdsChannel>(std::move(s));
}

asio::awaitable<void>
UdsChannel::async_send(FrameType type, FrameEncoding encoding, ConstSlice payload) {
    if (payload.size() > kMaxPayload) {
        throw IpcError("payload exceeds kMaxPayload");
    }

    // 帧头：4B BE length + 1B type + 1B encoding
    // length 语义：type + encoding + payload 三段的字节数 = 2 + payload.size()
    std::array<uint8_t, kLenHdr + kMeta> hdr{};
    uint32_t len_be = boost::endian::native_to_big(static_cast<uint32_t>(kMeta + payload.size()));
    memcpy(hdr.data(), &len_be, kLenHdr);
    hdr[kLenHdr + 0] = static_cast<uint8_t>(type);
    hdr[kLenHdr + 1] = static_cast<uint8_t>(encoding);

    // scatter/gather：头一帧、payload 一帧，零拷贝（除头组装）
    std::array<asio::const_buffer, 2> bufs{asio::buffer(hdr),
                                           asio::buffer(payload.data(), payload.size())};

    co_await asio::async_write(sock_, bufs, asio::use_awaitable);
}

asio::awaitable<std::optional<ReceivedFrame>> UdsChannel::async_recv() {
    // ---- 1) 读 4 字节长度头 ----
    std::array<uint8_t, kLenHdr> len_buf{};
    boost::system::error_code    ec;
    co_await asio::async_read(
        sock_, asio::buffer(len_buf), asio::redirect_error(asio::use_awaitable, ec));

    if (ec == asio::error::eof || ec == asio::error::connection_reset) {
        co_return std::nullopt;
    }
    if (ec) {
        throw IpcError(ec.message());
    }

    uint32_t len_be = 0;
    memcpy(&len_be, len_buf.data(), kLenHdr);
    uint32_t length = boost::endian::big_to_native(len_be);

    // ---- 2) 校验 length ----
    if (length < kMeta) {
        throw IpcError("frame length < 2 (malformed)");
    }
    if (length - kMeta > kMaxPayload) {
        throw IpcError("frame payload exceeds kMaxPayload");
    }

    // ---- 3) 一次 async_read 读 [type | encoding | payload] ----
    //  payload 直接读进 ReceivedFrame，无二次拷贝
    ReceivedFrame frame;
    frame.payload.resize(length - kMeta);

    std::array<uint8_t, kMeta>          meta{};
    std::array<asio::mutable_buffer, 2> bufs{asio::buffer(meta), asio::buffer(frame.payload)};

    co_await asio::async_read(sock_, bufs, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        throw IpcError(ec.message());
    }

    frame.type     = static_cast<FrameType>(meta[0]);
    frame.encoding = static_cast<FrameEncoding>(meta[1]);
    co_return frame;
}

void UdsChannel::close() noexcept {
    boost::system::error_code ec;
    auto                      _ = sock_.close(ec);
}

} // namespace ipc
} // namespace tqsdk
