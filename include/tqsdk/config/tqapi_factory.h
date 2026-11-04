#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/data/kline_store.h"
#include "tqsdk/gateway/ctp/ctp_config.h"
#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/tqapi.h"
#include "tqsdk/util/runtime.h"

namespace tqsdk {

/// 行情网关种类。
enum class MdKind {
    None,   ///< 不接行情
    Tq,     ///< 天勤行情（进程内直连）
    Ctp,    ///< CTP 行情前置
    Remote, ///< 通过 IPC 代理到 gw 进程
    Mock,   ///< 脚本化 mock（测试用，需外部注入）
};

/// 交易网关种类。
enum class TdKind {
    None,   ///< 不接交易
    Sim,    ///< 本地撮合模拟器
    Ctp,    ///< CTP 交易前置
    Remote, ///< 通过 IPC 代理到 gw 进程
    Mock,   ///< 脚本化 mock（测试用）
};

/// IPC 传输方式。
enum class IpcTransport {
    Uds,
    Tcp
};

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
        /// 本端在握手 Hello 中声明的角色。连接 gw（作为数据消费端）时用 Node；
        /// 策略端连接 node.serve 时用 Gateway。两端角色必须不同（否则握手被拒）。
        ipc::IpcRole role = ipc::IpcRole::Gateway;
        /// 本端实例名（Hello.name，仅日志/辨识）。
        std::string local_name = "tqsdk";
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
        /// 远程网关（通常为 CTP）是否启用 SHFE 市价/对价限价转换。
        bool shfe_market_as_limit = true;
    } td;

    /// [ctp] 柜台参数（md.kind/td.kind == ctp 时使用）。
    CtpConfig ctp;
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
/// core_ex 为空时使用 ctx 的 executor；多线程运行时调用方应传入 core strand，
/// TqApi/DataManager/网关事件循环全部绑定到该 executor。
asio::awaitable<std::unique_ptr<TqApi>>
make_tqapi(asio::io_context&           ctx,
           const TqApiConfig&          cfg,
           asio::any_io_executor       core_ex     = {},
           std::shared_ptr<KlineStore> kline_store = nullptr);

// ============================================================================
// 两进程（tqsdk-gw / tqsdk-node）的配置
// ============================================================================

/// tqsdk-gw（接入进程）配置。
///
/// gw 进程监听一个 UDS/TCP 端点，等待 node 连接；连接上来后，用**真实网关**
/// （md/td）为其提供数据与交易能力。真正接哪个网关由 [md].kind/[td].kind 决定。
struct GwConfig {
    struct Listen {
        IpcTransport transport = IpcTransport::Uds;
        std::string  socket    = "/tmp/tqsdk-gw.sock"; // transport==Uds
        std::string  tcp_host  = "127.0.0.1";
        uint16_t     tcp_port  = 0;
    } listen;

    ipc::FrameEncoding        encoding = ipc::FrameEncoding::Json;
    std::chrono::milliseconds handshake_timeout{std::chrono::seconds(5)};
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(5)};
    std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds(15)};
    std::string               local_name = "tqsdk-gw";

    // 真实网关选择（复用 TqApiConfig 的 Md/Td 段语义，但只允许直连种类）。
    TqApiConfig::Md md;
    TqApiConfig::Td td;

    /// SimTdGateway 的合约规格种子（[td.specs."SYMBOL"]）。仅在 td.kind == Sim 时使用。
    /// 每个条目对应一次 set_contract_spec。
    struct SpecSeed {
        std::string symbol;
        double      volume_multiple    = 1.0;
        double      price_tick         = 0.0;
        double      margin_rate        = 0.1;
        double      commission_rate    = 0.0;
        double      commission_per_lot = 0.0;
    };
    std::vector<SpecSeed> specs;

    /// SimTdGateway 的初始撮合价（[td.last_prices]）。用于让市价单/限价单在启动即有
    /// 可撮合价格；对应 on_tick(symbol, price)。
    std::map<std::string, double> last_prices;

    /// [ctp] 柜台参数（md.kind/td.kind == ctp 时使用）。
    CtpConfig ctp;

    /// [runtime] worker 线程数（io_context 线程；数据面仍由 core strand 串行）。
    RuntimeConfig runtime;
};

/// tqsdk-node（数据服务进程）配置。
///
/// node 从 gw 拉数据（作为 IpcRole::Node 连接 gw 端点），在进程内合并为权威截面，
/// 再通过 HTTP serve 层对外暴露。gw 连接参数复用 TqApiConfig（md/td kind 应设为
/// Remote，指向 gw 的监听端点）。
struct NodeConfig {
    TqApiConfig               upstream; ///< 到 gw 的连接（md/td.kind = Remote）
    std::string               serve_bind = "127.0.0.1";
    uint16_t                  serve_port = 8080;
    std::vector<InstrumentId> symbols;     ///< 启动即预订阅的合约（便于直接 curl 到数据）
    InstrumentCatalog::Config instruments; ///< [instruments] 标准合约目录/刷新
    KlineStore::Config        kline_cache; ///< [kline_cache] 本地 K 线历史缓存

    /// [serve.ws]：同一 HTTP 端口上的 WebSocket 实时推送。
    struct Ws {
        std::string               path = "/stream";
        std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(30)};
        std::size_t               max_clients = 256;
        std::size_t               max_queue   = 1024;
    } ws;

    /// [runtime] worker 线程数（io_context 线程；数据面仍由 core strand 串行）。
    RuntimeConfig runtime;
};

/// 解析 gw 配置（TOML 文本）。
GwConfig parse_gw_config(std::string_view toml_text);

/// 解析 node 配置（TOML 文本）。
NodeConfig parse_node_config(std::string_view toml_text);

/// 从文件加载。
GwConfig   load_gw_config(const std::string& path);
NodeConfig load_node_config(const std::string& path);

} // namespace tqsdk
