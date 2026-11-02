#pragma once

#include "tqsdk/pch.h"

#include "ipc_channle.h"
#include "stream_channel.h"

namespace tqsdk {
namespace ipc {

class UdsChannel final : public StreamChannel<asio::local::stream_protocol::socket> {
public:
    using Socket = asio::local::stream_protocol::socket;

    /// 客户端：连接 path；失败抛 asio::system_error
    static std::unique_ptr<UdsChannel> connect(asio::io_context& io, const std::string& path);

    /// 服务端：acceptor 接受后构造
    explicit UdsChannel(Socket sock)
        : StreamChannel(std::move(sock)) {}
};

} // namespace ipc
} // namespace tqsdk
