#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/ipc/protocol.h"
#include "tqsdk/tqapi.h"

namespace tqsdk {

/// 行情网关种类。
enum class MdKind {
    None,   ///< 不接行情
    Tq,     ///< 天勤行情（进程内直连）
    Remote, ///< 通过 IPC 代理到 gw 进程
    Mock,   ///< 脚本化 mock（测试用，需外部注入）
};

/// 交易网关种类。
enum class TdKind {
    None,   ///< 不接交易
    Sim,    ///< 本地撮合模拟器
    Remote, ///< 通过 IPC 代理到 gw 进程
    Mock,    ///< 脚本化 mock（测试用）
};

/// IPC 传输方式。
enum class IpcTransport { Uds, Tcp };

/// TqApi 配置（对应一份 TOML 文件）。
///
/// 设计：每个网关独立选择"进程内直连"或"远程代理"，由 [md].kind / [td].kind 决定；
/// 远程时的连接参数来自 [ipc] 段及各段自身的 socket/host/port。
struct TqApiConfig {
    struct Ipc {
        IpcTransport              transport = IpcTransport::Uds;
        ipc::FrameEncoding        encoding  = ipc::FrameEncoding::Json;
        std::string               uds_dir   = "/tmp";
        std::chrono::milliseconds handshake_timeout{std::chrono::seconds(5)};
        std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(5)};
        std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds(15)};
    } ipc;

    struct Md {
        MdKind kind = MdKind::None;

        // kind == Tq
        std::string host = "free-openmd.shinnytech.com";
        std::string port = "443";
        std::string path = "/t/md/front/mobile";

        // kind == Remote
        std::string socket; // uds 路径（transport==Uds 时用）
        std::string tcp_host;
        uint16_t    tcp_port = 0;

        // kind == Remote 时的实例名（Hello.name）
        std::string name = "gw-md";
    } md;

    struct Td {
        TdKind kind = TdKind::None;

        // kind == Sim
        double initial_balance = 1'000'000.0;

        // kind == Remote
        std::string               socket;
        std::string               tcp_host;
        uint16_t                  tcp_port = 0;
        std::string               name     = "gw-td";
        std::chrono::milliseconds ack_timeout{std::chrono::seconds(10)};
    } td;
};

/// 从 TOML 文件解析配置。文件不存在 / 语法错误 / 缺必填项 抛 std::runtime_error。
TqApiConfig load_tqapi_config(const std::string& path);

/// 从 TOML 字符串解析配置（便于测试，无需落盘）。
TqApiConfig parse_tqapi_config(std::string_view toml_text);

/// 依据配置构造一个 TqApi。
///
/// 对进程内网关（tq/sim）同步构造；对远程网关需先建立 IPC 连接并完成握手，
/// 因此本函数是协程。返回的 TqApi 已 start()，可直接驱动 io.run()。
///
/// 远程模式下，md/td 共享同一条 IpcSession（一个 gw 连接同时承载行情与交易），
/// 由 RemoteMdGateway / RemoteTdGateway 通过 shared_ptr<IpcClient> 共享持有。
///
/// 失败（建连/握手超时、文件缺失等）抛异常。
asio::awaitable<std::unique_ptr<TqApi>> make_tqapi(asio::io_context& ctx,
                                                   const TqApiConfig& cfg);

} // namespace tqsdk
