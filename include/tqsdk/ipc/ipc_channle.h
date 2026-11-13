#pragma once

#include "tqsdk/pch.h"

#include "frame.h"
#include "ipc_error.h"

namespace tqsdk {
namespace ipc {

namespace asio = boost::asio;

/// 统一的异步 IPC 通道接口
///
/// 约束：
///  - 所有方法必须在 socket 所属的 io_context 线程上调用（asio 标准约束）
///  - close() 后所有未完成操作以 operation_aborted 结束
///  - 帧格式严格遵循 Frame：`[len: u32 BE][type: u8][encoding: u8][payload]`
///    其中 len = 2 + payload.size()
class IIpcChannel {
public:
    virtual ~IIpcChannel() = default;

    /// 发送一帧；失败返回 IpcFailure。
    virtual asio::awaitable<IpcResult<void>>
    async_send(FrameType type, FrameEncoding encoding, ConstSlice payload) = 0;

    /// 接收一帧；对端关闭返回 nullopt，协议错误返回 IpcFailure。
    virtual asio::awaitable<IpcResult<std::optional<ReceivedFrame>>> async_recv() = 0;

    virtual void close() noexcept = 0;
};

} // namespace ipc
} // namespace tqsdk
