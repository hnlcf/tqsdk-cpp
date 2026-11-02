#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/data/data_manager.h"
#include "tqsdk/data/diff_merge_engine.h"
#include "tqsdk/gateway/market_data_gateway.h"
#include "tqsdk/gateway/trading_gateway.h"
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
          std::unique_ptr<TradingGateway>    td);

    ~TqApi();

    TqApi(const TqApi&)            = delete;
    TqApi& operator=(const TqApi&) = delete;

    // ---- 生命周期 ----
    // 启动后台消费协程。必须在 io_context.run() 之前调用一次。
    void start();
    // 关闭网关通道，令后台协程退出。幂等。
    void close();

    // 注册一个在 close() 时同步执行的清理钩子（按注册顺序调用）。
    // 用于关闭网关之外、TqApi 不直接持有的资源（例如远程模式下共享的
    // IpcClient 连接）。幂等：close() 已执行后再注册会立即调用一次。
    void add_close_hook(std::function<void()> hook);

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
    // K 线流；不存在返回 nullptr（需先由数据更新创建）。
    KlineStream* get_kline(std::string_view symbol, int64_t duration_ns);

    // 获取（并按需创建+订阅）K 线序列，语义对齐 TqSdk 的 get_kline_serial：
    //   - 若该 (symbol, duration) 序列尚不存在，则在本地创建空流并发起订阅；
    //   - 返回序列指针（非空），后续通过 wait_update() + 本指针读取 bars。
    // view_width 为期望保留的 bar 根数（同时决定订阅请求的宽度）。
    KlineStream*
    get_kline_serial(std::string_view symbol, int64_t duration_ns, int view_width = 200);

    // 判断某实体本轮的某字段是否变化。
    bool is_changing(const Quote& q, uint64_t field_bits) const noexcept;
    bool is_changing(const Order& o, uint64_t field_bits) const noexcept;
    bool is_changing(const Account& a, uint64_t field_bits) const noexcept;

    // ---- 交易 ----
    // 异步发单：本地造占位单，命令发给网关后立即返回其指针（保证非空），不等终态。
    // 注意：Asio 的 awaitable 不支持引用类型，故返回 Order* 而非 Order&。
    asio::awaitable<Order*>     insert_order(const InsertOrderReq& req);
    asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req);

    // ---- 内部数据访问（测试/调试用）----
    DataManager& data() noexcept { return data_; }
    std::string  next_order_id();

private:
    asio::awaitable<void> md_loop();
    asio::awaitable<void> td_loop();

    asio::any_io_executor              ex_;
    std::unique_ptr<MarketDataGateway> md_;
    std::unique_ptr<TradingGateway>    td_;

    DataManager     data_;
    DiffMergeEngine merger_;
    UpdateSignal    update_signal_;

    bool                  started_      = false;
    bool                  closed_       = false;
    bool                  cleared_once_ = false; // 首轮 wait 前无需清位
    std::atomic<uint64_t> order_seq_{0};

    std::vector<std::function<void()>> close_hooks_;
};

} // namespace tqsdk
