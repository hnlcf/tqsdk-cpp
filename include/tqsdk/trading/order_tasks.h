#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h"

namespace tqsdk {

class TqApi;
struct Quote;

enum class TargetPosPriceMode {
    ACTIVE,  ///< 主动价：买用卖一、卖用买一
    PASSIVE, ///< 被动价：买用买一、卖用卖一
    CUSTOM,  ///< 使用 price_provider
};

struct TargetPosOptions {
    /// 单笔最大手数；0 表示不拆分。
    std::uint64_t max_volume = 0;
    /// CUSTOM 模式价格回调；返回空表示行情不足，等待下一次更新。
    std::function<std::optional<double>(const Quote&, Direction)> price_provider;
};

/// 将账户净持仓调整到目标值。
///
/// 这是执行任务的第一版：
/// - target_volume 以“净持仓”计，正数多头、负数空头；
/// - 支持 ACTIVE/PASSIVE/CUSTOM 三种价格来源；
/// - 自动选择开仓/平仓，平仓使用 CLOSE 由网关按交易所规则拆分今昨；
/// - 同一任务同时最多一笔在途报单；
/// - 任务由 TqApi::wait_update() 的 update hook 驱动，因此必须在 wait_update
///   循环中运行；
/// - cancel() 会撤销本任务发出的活动委托，并注销 hook。
class TargetPosTask {
public:
    explicit TargetPosTask(TqApi&             api,
                           std::string        symbol,
                           TargetPosPriceMode mode    = TargetPosPriceMode::ACTIVE,
                           TargetPosOptions   options = {});
    ~TargetPosTask();

    TargetPosTask(const TargetPosTask&)            = delete;
    TargetPosTask& operator=(const TargetPosTask&) = delete;

    void         set_target_volume(std::int64_t volume);
    std::int64_t target_volume() const;
    void         cancel();
    bool         active() const;

private:
    struct State;
    static void            on_update(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};

/// 把一个 InsertOrderReq 发到成交；未完全成交时在后续更新中补单。
class InsertOrderTask {
public:
    explicit InsertOrderTask(TqApi& api, InsertOrderReq request, std::uint32_t max_retries = 20);
    ~InsertOrderTask();

    InsertOrderTask(const InsertOrderTask&)            = delete;
    InsertOrderTask& operator=(const InsertOrderTask&) = delete;

    void          start();
    void          cancel();
    bool          active() const;
    std::uint64_t filled_volume() const;

private:
    struct State;
    static void            on_update(const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};

/// 与 InsertOrderTask 语义一致，名字对齐官方 API；保留独立类型便于后续扩展。
class InsertOrderUntilAllTradedTask final : public InsertOrderTask {
public:
    using InsertOrderTask::InsertOrderTask;
};

} // namespace tqsdk
