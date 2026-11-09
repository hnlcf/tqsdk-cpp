#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/data/data_manager.h"
#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/data/kline_store.h"
#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/trading/risk_manager.h"
#include "tqsdk/update_signal.h"

namespace tqsdk {

/// TqApi 是对用户暴露的唯一门面。
///
/// 生命周期语义（阶段 2 定死）：
///   - 构造：接管两个网关，启动后台 md_loop / td_loop，不停消费并合并 diff。
///   - wait_update()：挂起直到至少一次 diff 被合并；返回 true。进入下一轮时清位，
///     故本轮返回后 is_changing 仍可读到"自上次 wait 以来"的变化。
///   - get_quote/get_order/get_account/get_kline/is_changing：纯本地内存读。
///   - insert_order()：异步发单，立即返回本地占位 Order*（非空），不原地等终态；
///     用户通过后续 wait_update() + get_order(rid) 观察状态推进。
///   - close()：关闭两网关通道并停止后台 loop；之后 wait_update 返回 false。
///
/// 线程模型：阶段 0~3 使用单线程 io_context，TqApi 内部状态不做同步保护。
///
/// 生命周期约束：销毁 TqApi（或回收其网关）前，调用方必须先 close() 并驱动
/// io_context 跑完后台协程；否则挂起在 Channel::async_receive 的协程会在
/// Channel 析构后触碰已释放的 waiter。单线程回测可用 close() + ctx.run() 收尾。
class TqApi {
public:
    TqApi(asio::any_io_executor              ex,
          std::unique_ptr<MarketDataGateway> md,
          std::unique_ptr<TradingGateway>    td,
          std::shared_ptr<KlineStore>        kline_store = nullptr);

    ~TqApi();

    TqApi(const TqApi&)            = delete;
    TqApi& operator=(const TqApi&) = delete;

    // ---- 生命周期 ----
    // 启动后台消费协程。必须在 io_context.run() 之前调用一次。
    void start();
    // 关闭网关通道，令后台协程退出。幂等。
    void close();

    // ---- 就绪/指标 ----
    bool           md_ready() const;
    bool           td_ready() const;
    bool           ready() const;
    /// 当前交易阶段；无交易网关时返回 Unknown（由调用方回退到本地交易日历）。
    TradingCalendar::Phase session_phase() const noexcept {
        return td_ ? td_->session_phase() : TradingCalendar::Phase::Unknown;
    }
    nlohmann::json readiness_json() const;
    nlohmann::json metrics_json() const;
    nlohmann::json kline_cache_status() const;

    // 注册一个在 close() 时同步执行的清理钩子（按注册顺序调用）。
    // 用于关闭网关之外、TqApi 不直接持有的资源（例如远程模式下共享的
    // IpcClient 连接）。幂等：close() 已执行后再注册会立即调用一次。
    void add_close_hook(std::function<void()> hook);

    // ---- 更新钩子（供 TargetPosTask / InsertOrderTask 驱动）----
    using UpdateHookId = std::uint64_t;
    UpdateHookId add_update_hook(std::function<void()> hook);
    void         remove_update_hook(UpdateHookId id);
    /// 唤醒当前/下一次 wait_update；任务 set_target_volume 后调用。
    void                  notify_update();
    asio::any_io_executor executor() const noexcept { return ex_; }

    // ---- 风控 ----
    RiskManager&       risk_manager() noexcept { return risk_; }
    const RiskManager& risk_manager() const noexcept { return risk_; }

    // ---- 等待事件 ----
    // 挂起直到有 diff 合并；返回 true 表示有更新，false 表示已关闭。
    // 进入下一轮时清空 changed_fields（"自上次 wait 以来"）。
    asio::awaitable<bool> wait_update();

    // ---- 本地数据读取（返回引用，生命周期至下一次 wait_update 进入清位或 close）----
    // 合约行情；不存在则创建一条空 Quote 占位并订阅。
    Quote& get_quote(std::string_view symbol);
    // 报单；按 rid 查，不存在返回 nullptr。
    Order* get_order(std::string_view order_id);
    // 账户快照。
    Account& get_account();
    const Account& get_account() const;
    /// 获取 tick 序列（由订阅到的 quote 快照固化）。首次调用会订阅该合约行情。
    TickSeries* get_tick_serial(std::string_view symbol, int view_width = 200);
    // K 线流；不存在返回 nullptr（需先由数据更新创建）。
    KlineStream* get_kline(std::string_view symbol, int64_t duration_ns);

    // 获取（并按需创建+订阅）K 线序列，语义对齐 TqSdk 的 get_kline_serial：
    //   - 若该 (symbol, duration) 序列尚不存在，则在本地创建空流并发起订阅；
    //   - 返回序列指针（非空），后续通过 wait_update() + 本指针读取 bars。
    // view_width 为期望保留的 bar 根数（同时决定订阅请求的宽度）。
    KlineStream*
    get_kline_serial(std::string_view symbol, int64_t duration_ns, int view_width = 200);

    // ---- 显式订阅管理（供 serve 层做多客户端引用计数聚合）----
    // 与 get_quote/get_kline_serial 的"首次创建才订阅"不同，这些接口每次调用都
    // 会把订阅意图交给网关；调用方（如 WebSocket hub）负责保证同一 symbol 只在
    // 全局引用计数 0->1 时调用一次、归零时 release 一次，从而与网关的引用计数对称。
    // 本地实体不存在时会先创建占位，保证后续 diff 能落库。
    asio::awaitable<void> subscribe_quotes(std::vector<std::string> symbols);
    asio::awaitable<void> subscribe_kline(std::string symbol, int64_t duration_ns, int view_width);
    asio::awaitable<void> release(std::vector<std::string>                     symbols,
                                  std::vector<std::pair<std::string, int64_t>> klines);

    // 判断某实体本轮的某字段是否变化。
    bool is_changing(const Quote& q, uint64_t field_bits) const noexcept;
    bool is_changing(const Order& o, uint64_t field_bits) const noexcept;
    bool is_changing(const Account& a, uint64_t field_bits) const noexcept;

    // ---- 交易 ----
    // 异步发单：本地造占位单，命令发给网关后立即返回其指针（保证非空），不等终态。
    // 注意：Asio 的 awaitable 不支持引用类型，故返回 Order* 而非 Order&。
    asio::awaitable<Order*>     insert_order(const InsertOrderReq&  req,
                                             std::optional<OrderId> order_id = std::nullopt);
    asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req);

    // ---- 内部数据访问（测试/调试用）----
    DataManager&       data() noexcept { return data_; }
    const DataManager& data() const noexcept { return data_; }
    std::string        next_order_id();

private:
    asio::awaitable<void> md_loop();
    asio::awaitable<void> td_loop();

    void hydrate_kline_cache(std::string_view symbol,
                             int64_t          duration_ns,
                             int              view_width,
                             KlineStream&     stream);
    int  remote_kline_width(std::string_view symbol, int64_t duration_ns, int requested) const;
    void persist_changed_klines();
    void run_update_hooks();
    void append_ticks_from_quotes();

    asio::any_io_executor              ex_;
    std::unique_ptr<MarketDataGateway> md_;
    std::unique_ptr<TradingGateway>    td_;

    DataManager     data_;
    DiffMergeEngine merger_;
    UpdateSignal    update_signal_;

    std::atomic<bool>     started_{false};
    std::atomic<bool>     closed_{false};
    bool                  cleared_once_ = false; // 首轮 wait 前无需清位
    std::atomic<uint64_t> order_seq_{0};
    std::atomic<bool>     md_ready_{false};
    std::atomic<bool>     td_ready_{false};
    std::atomic<bool>     md_connected_{false};
    std::atomic<bool>     td_connected_{false};

    std::shared_ptr<KlineStore>           kline_store_;
    std::unordered_map<std::string, int>  cache_loaded_width_;
    std::unordered_map<std::string, bool> cache_fresh_;

    std::vector<std::function<void()>> close_hooks_;

    std::uint64_t                                               next_update_hook_id_ = 0;
    std::vector<std::pair<UpdateHookId, std::function<void()>>> update_hooks_;
    RiskManager                                                 risk_;
};

} // namespace tqsdk
