#pragma once

#include "tqsdk/pch.h"

#include "ipc_channle.h"
#include "stream_channel.h"

namespace tqsdk {
namespace ipc {

/// 基于 TCP 的 IPC 通道，用于跨机 / 分布式部署。
///
/// 与 UdsChannel 共用同一套帧协议实现（StreamChannel），行为一致，
/// 仅传输层不同。跨机时注意：应配合 TLS 或网络层鉴权（当前为明文 TCP，
/// 建议仅用于受信网络；后续可在 StreamChannel 之上叠加 ssl::stream）。
class TcpChannel final : public StreamChannel<asio::ip::tcp::socket> {
public:
    using Socket = asio::ip::tcp::socket;

    /// 客户端：同步解析并连接 host:port；失败抛 asio::system_error。
    /// 与 UdsChannel::connect 一样是同步建连（建议在非 IO 线程或启动阶段调用）。
    static std::unique_ptr<TcpChannel>
    connect(asio::io_context& io, const std::string& host, uint16_t port);
    /// 服务端：acceptor 接受后构造
    explicit TcpChannel(Socket sock)
        : StreamChannel(std::move(sock)) {}
};

} // namespace ipc
} // namespace tqsdk
