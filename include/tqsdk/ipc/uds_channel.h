#pragma once

#include "tqsdk/pch.h"

#include "ipc_channle.h"

namespace tqsdk {
namespace ipc {

class UdsChannel final : public IIpcChannel {
public:
    using Socket = asio::local::stream_protocol::socket;

    /// 客户端：连接 path；失败抛 asio::system_error
    static std::unique_ptr<UdsChannel> connect(asio::io_context& io, const std::string& path);

    /// 服务端：acceptor 接受后构造
    explicit UdsChannel(Socket sock)
        : sock_(std::move(sock)) {}

    asio::awaitable<void>
    async_send(FrameType type, FrameEncoding encoding, ConstSlice payload) override;

    asio::awaitable<std::optional<ReceivedFrame>> async_recv() override;

    void close() noexcept override;

private:
    Socket sock_;

    // 2MB payload 上限，可按业务调；消息是行情/指令，不是文件块
    static constexpr uint32_t kMaxPayload = 2u << 20;
    static constexpr size_t   kLenHdr     = 4;
    static constexpr size_t   kMeta       = 2; // type + encoding
};

} // namespace ipc
} // namespace tqsdk
