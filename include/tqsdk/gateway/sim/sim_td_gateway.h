#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

/// 合约静态规格：撮合与资金计算所需的最小信息集。
/// 阶段 3 由 set_contract_spec() 显式配置；阶段 5 将从真实标的表
/// (symbols/latest.json) 拉取后填入，sim 与真实网关共用同一套字段。
struct ContractSpec {
    double volume_multiple    = 1.0; // 合约乘数
    double price_tick         = 0.0; // 最小变动价位（撮合价穿越判定用）
    double margin_rate        = 0.1; // 保证金率
    double commission_rate    = 0.0; // 手续费率（按成交额）
    double commission_per_lot = 0.0; // 每手固定手续费（与 rate 叠加）
};

/// 本地撮合模拟网关（TradingGateway 的模拟实现）。
///
/// 设计要点：
///   - 与真实网关完全同构：所有状态变更一律翻译成 trade diff 推入 event_output()，
///     由 TqApi 的 td_loop -> DiffMergeEngine 合并，模拟器本身不碰 DataManager。
///   - 一次撮合产生的 order/trade/position/account 变更放进同一个 diff（事务性），
///     td_loop 一次 receive 即完成一次原子推进。
///   - 撮合价格来自内部 last_price_ 表，由 on_tick() 显式驱动（确定性回测）。
///     on_tick 扮演"交易所撮合回调"的角色，由回测 driver 在喂行情时同步调用；
///     它不属于 TradingGateway 抽象，TqApi 对它无感知。
///
/// 撮合规则（阶段 3 简化模型）：
///   - 市价单：按 last_price_ 立即成交；无报价则拒单。
///   - 限价单：到价即成交，否则挂入簿；on_tick 时按最新价穿越撮合。
///   - 支持部分成交（成交量受剩余手数限制）。
///   - 撤单：已完全成交则忽略，否则产终态 diff。
///
/// 资金模型（简化自洽，不做今昨仓/结算/交割/多币种）：
///   - 开仓冻结保证金，成交后转持仓占用；平仓释放对应保证金。
///   - 手续费 = 成交额 * commission_rate + 手数 * commission_per_lot。
///   - balance/available/risk_ratio 等按 TqSdk 同构公式重算。
class SimTdGateway : public TradingGateway {
public:
    explicit SimTdGateway(asio::any_io_executor ex, double initial_balance = 1'000'000.0);

    // ---- TradingGateway ----
    asio::awaitable<void>       start() override;
    asio::awaitable<void>       stop() override;
    asio::awaitable<CommandAck> insert_order(const InsertOrderReq& req,
                                             const OrderId&        rid) override;
    asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req) override;
    Channel<TradeEvent>&        event_output() override { return ch_; }

    // ---- 撮合驱动（供回测 driver 调用，非 TradingGateway 抽象的一部分）----
    // 更新某合约最新价并触发挂单簿撮合。返回本次产生的 diff 数量（便于测试断言）。
    void on_tick(const InstrumentId& symbol, double last_price);

    // ---- 规格配置（阶段 5 由真实标的表填充）----
    void set_contract_spec(const InstrumentId& symbol, const ContractSpec& spec);
    bool has_contract_spec(const InstrumentId& symbol) const;

private:
    // 挂单簿内的活动订单状态
    struct LiveOrder {
        OrderId      rid;
        InstrumentId instrument_id;
        Direction    direction;
        Offset       offset;
        PriceType    price_type;
        double       limit_price;  // 限价；市价单为 NaN
        uint64_t     volume_orign; // 原始手数
        uint64_t     volume_left;  // 未成交手数
        TimeStamp    insert_time;
    };

    // 持仓聚合（long/short 总量）
    struct SimPosition {
        uint64_t volume_long     = 0;
        uint64_t volume_short    = 0;
        double   open_cost_long  = 0.0; // 多头累计开仓成本
        double   open_cost_short = 0.0; // 空头累计开仓成本
    };

    // 资金账户
    struct SimAccount {
        double pre_balance   = 0.0; // 初始权益
        double balance       = 0.0; // 动态权益 = pre_balance + 平仓盈亏 - 手续费
        double close_profit  = 0.0; // 平仓盈亏累计
        double commission    = 0.0; // 手续费累计
        double margin        = 0.0; // 持仓占用保证金
        double frozen_margin = 0.0; // 挂单冻结保证金
        double available     = 0.0; // 可用资金
        double risk_ratio    = 0.0; // 风险度
    };

    // 进行一次撮合尝试；返回是否发生了成交。
    // 成交时会把 order/trade/position/account 变更写入 acc（累积到单个 diff）。
    struct FillBatch {
        nlohmann::json orders        = nlohmann::json::object();
        nlohmann::json trades        = nlohmann::json::object();
        nlohmann::json positions     = nlohmann::json::object();
        bool           account_dirty = false;
        bool           empty() const {
            return orders.empty() && trades.empty() && positions.empty() && !account_dirty;
        }
    };

    // 遍历某合约挂单簿，按 last_price_ 撮合，结果累积进 batch。
    void match_symbol(const InstrumentId& symbol, FillBatch& batch);

    // 对单笔挂单做撮合（可能部分成交），成交结果写入 batch。
    void try_match_one(LiveOrder& order, double last_price, FillBatch& batch);

    // 实际成交 volume 手，更新本地状态并写 batch。
    void apply_fill(LiveOrder& order, uint64_t volume, double fill_price, FillBatch& batch);

    // 把 batch 转成 trade diff 并投递到通道。
    void emit(const FillBatch& batch);

    // 重算可用资金 / 风险度（在账户字段变化后调用）。
    void recompute_available();

    // 构造一条"报单被拒/错误终态"的 diff 并投递。
    void emit_reject(const OrderId& rid, const InstrumentId& symbol, const std::string& msg);

    // 依据方向/开平更新持仓（供 apply_fill 调用）。
    void update_position(const LiveOrder& order, uint64_t volume, double price, FillBatch& batch);
    nlohmann::json position_json(const InstrumentId& symbol) const;

    asio::any_io_executor                          ex_;
    Channel<TradeEvent>                            ch_;
    std::unordered_map<OrderId, LiveOrder>         live_orders_;
    std::unordered_map<InstrumentId, double>       last_price_;
    std::unordered_map<InstrumentId, ContractSpec> specs_;
    std::unordered_map<InstrumentId, SimPosition>  positions_;
    SimAccount                                     account_;
    std::atomic<uint64_t>                          trade_seq_{0};
    TimeStamp                                      clock_ns_ = 0;
};

} // namespace tqsdk
