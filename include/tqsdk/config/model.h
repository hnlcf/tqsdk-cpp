#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/auth/tq_auth.h"
#include "tqsdk/constants.h"
#include "tqsdk/data/continuous_table.h"
#include "tqsdk/data/kline_store.h"
#include "tqsdk/data/tick_store.h"
#include "tqsdk/gateway/ctp/ctp_config.h"
#include "tqsdk/instrument/instrument_catalog.h"
#include "tqsdk/ipc/protocol.h"
#include "tqsdk/util/audit.h"
#include "tqsdk/util/runtime.h"

namespace tqsdk {

enum class MdKind {
    None,
    Tq,
    Ctp,
    Remote,
    Mock,
};

enum class TdKind {
    None,
    Sim,
    Ctp,
    Tq,
    Remote,
    Mock,
};

enum class IpcTransport {
    Uds,
    Tcp
};

/// 一组 gw/node IPC 交互的完整配置。
struct IpcConfig {
    std::string        name      = "gw";
    IpcTransport       transport = IpcTransport::Uds;
    std::string        socket    = "/tmp/tqsdk-gw.sock";
    std::string        tcp_host  = "127.0.0.1";
    uint16_t           tcp_port  = 0;
    ipc::FrameEncoding encoding  = ipc::FrameEncoding::Json;

    std::chrono::milliseconds handshake_timeout{std::chrono::seconds(5)};
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(5)};
    std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds(15)};
};

struct TqMdConfig {
    std::string host     = "free-openmd.shinnytech.com";
    std::string port     = "443";
    std::string path     = "/t/md/front/mobile";
    bool        auto_url = false;
};

struct MdConfig {
    MdKind     kind = MdKind::None;
    TqMdConfig tq;
};

struct SimSpec {
    std::string symbol;
    double      volume_multiple    = 1.0;
    double      price_tick         = 0.0;
    double      margin_rate        = 0.1;
    double      commission_rate    = 0.0;
    double      commission_per_lot = 0.0;
};

struct SimConfig {
    double                        initial_balance = 1'000'000.0;
    std::vector<SimSpec>          specs;
    std::map<std::string, double> last_prices;
};

struct TqTdConfig {
    std::string broker_name;
    std::string account_id;
    std::string password;
    std::string broker_id;
    std::string front_url;
    std::string td_url;
    std::string account_key;
};

struct TdConfig {
    TdKind                    kind = TdKind::None;
    SimConfig                 sim;
    TqTdConfig                tq;
    bool                      auto_url = false;
    std::chrono::milliseconds ack_timeout{std::chrono::seconds(10)};
    bool                      shfe_market_as_limit = true;
};

/// TqApi 运行时配置。gw 用它描述真实网关；node 只通过 IPC 构造 Remote 版本。
struct EngineConfig {
    IpcConfig ipc;
    MdConfig  md;
    TdConfig  td;
    CtpConfig ctp;
};

struct ServeAuth {
    bool        enabled = false;
    std::string token;
    std::string token_env         = "TQSDK_SERVE_TOKEN";
    bool        allow_query_token = true;
    bool        exempt_healthz    = true;
};

struct ServeWs {
    std::string               path = "/stream";
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds(30)};
    std::size_t               max_clients = 256;
    std::size_t               max_queue   = 1024;
    std::chrono::milliseconds coalesce{0};
};

struct ServeConfig {
    std::string              bind = "127.0.0.1";
    uint16_t                 port = 8080;
    std::vector<std::string> symbols;
    ServeAuth                auth;
    ServeWs                  ws;
};

/// node 侧统一缓存配置；kline/tick 默认共用 cache.file。
struct CacheConfig {
    std::string               file = std::string(constants::kDefaultMarketCacheFile);
    InstrumentCatalog::Config instruments;
    KlineStore::Config        kline;
    TickStore::Config         tick;
    ContinuousTableConfig     continuous;
};

struct GatewayConfig {
    EngineConfig   engine;
    TqAuth::Config tq_auth;
    RuntimeConfig  runtime;
};

struct NodeConfig {
    struct AccountEndpoint {
        std::string id;
        IpcConfig   ipc;
    };
    std::vector<AccountEndpoint> accounts;
    std::string                  primary_account;

    ServeConfig    serve;
    CacheConfig    cache;
    TqAuth::Config tq_auth;
    audit::Options audit;
    RuntimeConfig  runtime;
};

} // namespace tqsdk
