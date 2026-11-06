#pragma once

#include "tqsdk/pch.h"

class CtpFaultInjectionTest;

namespace tqsdk::ctp {

/// CTP 查询状态机。
///
/// 负责“一次查询窗口”的调度，不负责查询结果如何落入账户/订单模型：
///   - 单并发：同一时刻最多一个请求在途；
///   - 查询队列：account/position/order/trade + 按持仓补充 instrument；
///   - 限流退避：CTP -2/-3 时重新排队，绝不并发轰炸；
///   - watchdog：超时后丢弃迟到响应并推进队列；
///   - generation：断线/重启后旧 timer/响应不能污染新窗口；
///   - snapshot_ready：仅在本轮全部请求结束/失败后置 true；
///   - refresh debounce：查询完成后延迟启动下一轮。
///
/// 线程约束：除 `snapshot_ready()` 使用 atomic 外，所有方法只在 CTP 网关的
/// core strand 上调用。`Callbacks` 允许网关把结果映射回自己的状态。
class CtpQueryPipeline {
public:
    enum class Kind {
        Account,
        Position,
        Order,
        Trade,
        InstrumentSymbol, ///< 仅按持仓合约查询规格，避免全市场合约洪峰
    };

    struct Job {
        Kind        kind;
        std::string symbol;
        int         attempts   = 0;
        int         request_id = 0;
    };

    struct Callbacks {
        /// 发送一条 CTP 查询；返回 CTP API 的 ret（0 成功，-2/-3 限流）。
        std::function<int(Job&)> send;
        /// 一轮查询完成（initial=true 表示首次快照）。
        std::function<void(bool initial)> completed;
        /// 某条查询发送失败/超时（网关可在此补充 instrument 状态处理）。
        std::function<void(Kind)> failed;
        /// debounce 到期后请求网关启动下一轮。
        std::function<void()> restart;
    };

    CtpQueryPipeline(boost::asio::any_io_executor ex,
                     Callbacks                    callbacks,
                     std::chrono::milliseconds    query_timeout = std::chrono::seconds(10));
    ~CtpQueryPipeline();

    CtpQueryPipeline(const CtpQueryPipeline&)            = delete;
    CtpQueryPipeline& operator=(const CtpQueryPipeline&) = delete;

    /// 开始一轮查询。已在飞行中时：
    ///   - initial=false -> 只记下 refresh pending；
    ///   - initial=true  -> 忽略（已有窗口优先）。
    void start(bool initial, std::vector<Job> jobs);

    /// 把“缺少规格的持仓合约”追加到当前窗口。
    void enqueue_specs(const std::vector<std::string>& symbols);

    /// 当前请求正常结束（is_last 回调）。
    void finish();

    /// 当前请求发送失败/超时；会调用 failed 回调后推进队列。
    void fail(Kind kind);

    /// 断线/stop：取消 timer，丢弃旧 generation 的响应。
    void reset();

    /// 查询完成后延迟启动下一轮；运行中则挂起，等本轮 complete 后再排期。
    void schedule_restart(std::chrono::milliseconds delay = std::chrono::milliseconds(200));

    bool running() const noexcept { return running_; }
    bool has_active() const noexcept { return active_.has_value(); }
    bool active_matches(Kind kind, int request_id) const noexcept;
    bool snapshot_ready() const noexcept { return snapshot_ready_.load(std::memory_order_acquire); }
    uint64_t    generation() const noexcept { return generation_; }
    std::size_t pending() const noexcept { return pending_; }

    /// 仅供生命周期/测试显式覆盖快照 readiness。
    void force_snapshot_ready(bool ready) noexcept {
        snapshot_ready_.store(ready, std::memory_order_release);
    }
    void set_query_timeout(std::chrono::milliseconds timeout) noexcept { query_timeout_ = timeout; }

private:
    struct Active {
        Kind        kind;
        std::string symbol;
        int         request_id = 0;
    };

    void pump();
    void schedule_pump(std::chrono::milliseconds delay);
    void arm_watchdog();
    void complete();
    void cancel_timers();

    boost::asio::any_io_executor ex_;
    Callbacks                    callbacks_;
    std::chrono::milliseconds    query_timeout_;

    bool                  running_         = false;
    bool                  initial_         = false;
    bool                  waiting_         = false;
    bool                  restart_pending_ = false;
    uint64_t              generation_      = 0;
    int                   pending_         = 0;
    std::optional<Active> active_;
    std::deque<Job>       queue_;
    std::atomic<bool>     snapshot_ready_{false};

    friend class ::CtpFaultInjectionTest;

    boost::asio::steady_timer pump_timer_;
    boost::asio::steady_timer restart_timer_;
    boost::asio::steady_timer watchdog_;
};

} // namespace tqsdk::ctp
