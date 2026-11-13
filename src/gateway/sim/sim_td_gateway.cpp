#include "tqsdk/gateway/sim/sim_td_gateway.h"

#include "tqsdk/domain/enum_util.h"
#include "tqsdk/domain/sentinel.h"

namespace tqsdk {

namespace {

// 撮合价与限价的穿越判定容差（考虑 price_tick 浮点误差）。
constexpr double kPriceEps = 1e-9;

bool is_buy(Direction d) noexcept {
    return d == Direction::BUY;
}

bool is_close(Offset o) noexcept {
    return o == Offset::CLOSE || o == Offset::CLOSETODAY || o == Offset::CLOSEYESTERDAY;
}

} // namespace

SimTdGateway::SimTdGateway(asio::any_io_executor ex, double initial_balance)
    : ex_(ex),
      ch_(ex) {
    account_.pre_balance = initial_balance;
    account_.balance     = initial_balance;
    account_.available   = initial_balance;
}

// ---------- 生命周期 ----------
asio::awaitable<void> SimTdGateway::start() {
    co_return;
}

asio::awaitable<void> SimTdGateway::stop() {
    ch_.close();
    co_return;
}

// ---------- 规格配置 ----------
void SimTdGateway::set_contract_spec(const InstrumentId& symbol, const ContractSpec& spec) {
    specs_[symbol] = spec;
}

bool SimTdGateway::has_contract_spec(const InstrumentId& symbol) const {
    return specs_.count(symbol) > 0;
}

// ---------- 报单 ----------
asio::awaitable<CommandAck> SimTdGateway::insert_order(const InsertOrderReq& req,
                                                       const OrderId&        rid) {
    const InstrumentId& symbol = req.instrument_id;

    // 1) 参数校验
    if (req.volume == 0) {
        emit_reject(rid, symbol, "invalid volume");
        co_return CommandAck{.success = false, .order_id = rid, .error_msg = "invalid volume"};
    }
    if (specs_.count(symbol) == 0) {
        emit_reject(rid, symbol, "unknown instrument");
        co_return CommandAck{.success = false, .order_id = rid, .error_msg = "unknown instrument"};
    }
    if (req.price_type == PriceType::LIMIT && !std::isfinite(req.limit_price)) {
        emit_reject(rid, symbol, "limit order requires finite price");
        co_return CommandAck{
            .success = false, .order_id = rid, .error_msg = "limit order requires finite price"};
    }

    // 2) 造挂单并尝试立即撮合
    LiveOrder o;
    o.rid           = rid;
    o.instrument_id = symbol;
    o.direction     = req.direction;
    o.offset        = req.offset;
    o.price_type    = req.price_type;
    o.limit_price   = (req.price_type == PriceType::LIMIT) ? req.limit_price : kUnknownDouble;
    o.time_cond     = req.time_cond;
    o.volume_cond   = req.volume_cond;
    o.volume_orign  = req.volume;
    o.volume_left   = req.volume;
    o.insert_time   = ++clock_ns_;

    live_orders_[rid] = o;

    FillBatch batch;
    // 市价单(ANY/BEST/FIVELEVEL)需要已有报价；限价单可无报价先挂，等 on_tick。
    if (req.price_type != PriceType::LIMIT) {
        auto lp = last_price_.find(symbol);
        if (lp == last_price_.end() || !std::isfinite(lp->second)) {
            live_orders_.erase(rid);
            emit_reject(rid, symbol, "no market price for market order");
            co_return CommandAck{.success = false, .order_id = rid, .error_msg = "no market price"};
        }
        try_match_one(live_orders_[rid], lp->second, batch);
    } else {
        // 限价单：若已有报价且已穿越，立即撮合
        auto lp = last_price_.find(symbol);
        if (lp != last_price_.end() && std::isfinite(lp->second)) {
            try_match_one(live_orders_[rid], lp->second, batch);
        }
    }

    // 3) 若已全部成交，移出挂单簿；否则留在簿上，产一条 alive 回执。
    auto it = live_orders_.find(rid);
    if (it != live_orders_.end() && it->second.volume_left == 0) {
        live_orders_.erase(it);
    } else if (it != live_orders_.end() && it->second.time_cond == TimeCondition::IOC) {
        // IOC/FAK/FOK 在当前快照下没有成交：立即终态，不留在模拟挂单簿。
        const LiveOrder& lo = it->second;
        batch.orders[rid] = nlohmann::json{{"order_id", rid},
                                           {"instrument_id", symbol},
                                           {"direction", magic_enum::enum_name(lo.direction)},
                                           {"offset", magic_enum::enum_name(lo.offset)},
                                           {"volume_orign", lo.volume_orign},
                                           {"volume_left", lo.volume_left},
                                           {"status", magic_enum::enum_name(OrderStatus::FINISHED)},
                                           {"is_dead", true},
                                           {"last_msg", "IOC/FAK未成交"}};
        remember_order(rid, batch.orders[rid]);
        live_orders_.erase(it);
        batch.account_dirty = true;
    } else if (it != live_orders_.end()) {
        // 挂单未成交：产一条"已受理(alive)"订单快照，让上层看到 volume_left。
        const LiveOrder& lo = it->second;
        batch.orders[rid]   = nlohmann::json{{"order_id", rid},
                                             {"instrument_id", symbol},
                                             {"direction", magic_enum::enum_name(lo.direction)},
                                             {"offset", magic_enum::enum_name(lo.offset)},
                                             {"volume_orign", lo.volume_orign},
                                             {"volume_left", lo.volume_left},
                                             {"status", magic_enum::enum_name(OrderStatus::ALIVE)},
                                             {"is_dead", false},
                                             {"last_msg", "已报"}};
        remember_order(rid, batch.orders[rid]);
        batch.account_dirty = true; // 冻结保证金变化
    }

    recompute_available();
    emit(batch);

    co_return CommandAck{.success = true, .order_id = rid, .error_msg = {}};
}

asio::awaitable<CommandAck> SimTdGateway::cancel_order(const CancelOrderReq& req) {
    auto it = live_orders_.find(req.order_id);
    if (it == live_orders_.end()) {
        // 已成交或不存在：无操作，但仍回 ack（幂等撤单语义）
        co_return CommandAck{.success = true, .order_id = req.order_id, .error_msg = {}};
    }

    const LiveOrder order = it->second;
    live_orders_.erase(it);

    // 产"已撤"终态 diff
    FillBatch      batch;
    nlohmann::json oj{{"order_id", order.rid},
                      {"instrument_id", order.instrument_id},
                      {"direction", magic_enum::enum_name(order.direction)},
                      {"offset", magic_enum::enum_name(order.offset)},
                      {"volume_orign", order.volume_orign},
                      {"volume_left", order.volume_left},
                      {"status", magic_enum::enum_name(OrderStatus::FINISHED)},
                      {"is_dead", true},
                      {"last_msg", "已撤销"}};
    batch.orders[order.rid] = oj;
    remember_order(order.rid, batch.orders[order.rid]);
    batch.account_dirty = true; // 冻结保证金释放

    recompute_available();
    emit(batch);

    co_return CommandAck{.success = true, .order_id = req.order_id, .error_msg = {}};
}

// ---------- 行情驱动撮合 ----------
void SimTdGateway::on_tick(const InstrumentId& symbol, double last_price) {
    if (!std::isfinite(last_price)) {
        return;
    }
    last_price_[symbol] = last_price;

    FillBatch batch;
    match_symbol(symbol, batch);
    if (!batch.empty()) {
        recompute_available();
        emit(batch);
    }
}

void SimTdGateway::match_symbol(const InstrumentId& symbol, FillBatch& batch) {
    double price = last_price_[symbol];
    // 收集该合约的挂单 id（避免遍历时迭代器失效）
    std::vector<OrderId> ids;
    ids.reserve(live_orders_.size());
    for (auto& [rid, order] : live_orders_) {
        if (order.instrument_id == symbol && order.volume_left > 0) {
            ids.push_back(rid);
        }
    }
    for (auto& rid : ids) {
        auto it = live_orders_.find(rid);
        if (it == live_orders_.end()) {
            continue; // 可能在前一轮已被撤/移除
        }
        try_match_one(it->second, price, batch);
        if (it->second.volume_left == 0) {
            live_orders_.erase(it);
        }
    }
}

void SimTdGateway::try_match_one(LiveOrder& order, double last_price, FillBatch& batch) {
    if (order.volume_left == 0) {
        return;
    }

    // 判定是否可成交：
    //   市价单 -> 总是成交
    //   限价买 -> last_price <= limit 才成交
    //   限价卖 -> last_price >= limit 才成交
    bool   can_fill   = false;
    double fill_price = last_price;
    if (order.price_type != PriceType::LIMIT) {
        can_fill = true;
    } else if (is_buy(order.direction)) {
        can_fill   = last_price <= order.limit_price + kPriceEps;
        fill_price = std::min(last_price, order.limit_price);
    } else {
        can_fill   = last_price >= order.limit_price - kPriceEps;
        fill_price = std::max(last_price, order.limit_price);
    }

    if (!can_fill) {
        return;
    }

    // 阶段 3：不模拟对手盘量限制，可全量成交；部分成交通过多次 on_tick 模拟。
    uint64_t fill_volume = order.volume_left;
    apply_fill(order, fill_volume, fill_price, batch);
}

void SimTdGateway::apply_fill(LiveOrder& order,
                              uint64_t   volume,
                              double     fill_price,
                              FillBatch& batch) {
    order.volume_left -= volume;

    const ContractSpec& spec   = specs_.at(order.instrument_id);
    double              amount = fill_price * spec.volume_multiple * static_cast<double>(volume);
    double              fee    = amount * spec.commission_rate + spec.commission_per_lot * volume;

    // ---- 手续费从权益扣除 ----
    account_.commission += fee;
    account_.balance = account_.pre_balance + account_.close_profit - account_.commission;

    // ---- 持仓 + 平仓盈亏 ----
    update_position(order, volume, fill_price, batch);

    // ---- 成交明细 trade ----
    TradeId tid = "trade_" + std::to_string(trade_seq_.fetch_add(1, std::memory_order_relaxed));
    nlohmann::json tj{{"order_id", order.rid},
                      {"trade_id", tid},
                      {"instrument_id", order.instrument_id},
                      {"direction", magic_enum::enum_name(order.direction)},
                      {"offset", magic_enum::enum_name(order.offset)},
                      {"volume", volume},
                      {"price", fill_price},
                      {"commission", fee},
                      {"trade_date_time", order.insert_time}};
    batch.trades[tid]   = tj;
    trade_history_[tid] = batch.trades[tid];

    // ---- 订单状态 ----
    OrderStatus    status = order.volume_left == 0 ? OrderStatus::FINISHED : OrderStatus::ALIVE;
    nlohmann::json oj{{"order_id", order.rid},
                      {"instrument_id", order.instrument_id},
                      {"direction", magic_enum::enum_name(order.direction)},
                      {"offset", magic_enum::enum_name(order.offset)},
                      {"volume_orign", order.volume_orign},
                      {"volume_left", order.volume_left},
                      {"trade_price", fill_price},
                      {"status", magic_enum::enum_name(status)},
                      {"is_dead", order.volume_left == 0},
                      {"last_msg", order.volume_left == 0 ? "全部成交" : "部分成交"}};
    batch.orders[order.rid] = oj;
    remember_order(order.rid, batch.orders[order.rid]);
}

void SimTdGateway::update_position(const LiveOrder& order,
                                   uint64_t         volume,
                                   double           price,
                                   FillBatch&       batch) {
    SimPosition& pos = positions_[order.instrument_id];

    if (is_close(order.offset)) {
        // 平仓：减少对应方向持仓，结算平仓盈亏（用持仓均价近似）。
        if (is_buy(order.direction)) {
            // 买平 -> 减少空头
            if (pos.volume_short > 0) {
                double   avg = pos.volume_short ? pos.open_cost_short / pos.volume_short : 0.0;
                uint64_t q   = std::min(volume, pos.volume_short);
                account_.close_profit +=
                    (avg - price) * q * specs_.at(order.instrument_id).volume_multiple;
                pos.open_cost_short -= avg * q;
                pos.volume_short -= q;
            }
        } else {
            // 卖平 -> 减少多头
            if (pos.volume_long > 0) {
                double   avg = pos.volume_long ? pos.open_cost_long / pos.volume_long : 0.0;
                uint64_t q   = std::min(volume, pos.volume_long);
                account_.close_profit +=
                    (price - avg) * q * specs_.at(order.instrument_id).volume_multiple;
                pos.open_cost_long -= avg * q;
                pos.volume_long -= q;
            }
        }
    } else {
        // 开仓：增加对应方向持仓，累积成本。
        if (is_buy(order.direction)) {
            pos.open_cost_long += price * volume;
            pos.volume_long += volume;
        } else {
            pos.open_cost_short += price * volume;
            pos.volume_short += volume;
        }
    }

    batch.positions[order.instrument_id] = position_json(order.instrument_id);
    batch.account_dirty                  = true;
}

nlohmann::json SimTdGateway::position_json(const InstrumentId& symbol) const {
    auto               pit = positions_.find(symbol);
    const SimPosition  empty;
    const SimPosition& p = pit == positions_.end() ? empty : pit->second;

    const ContractSpec& spec = specs_.at(symbol);
    double              m    = spec.volume_multiple;

    double position_price_long = p.volume_long ? p.open_cost_long / p.volume_long : kUnknownDouble;
    double position_price_short =
        p.volume_short ? p.open_cost_short / p.volume_short : kUnknownDouble;

    double last = kUnknownDouble;
    if (auto lp = last_price_.find(symbol); lp != last_price_.end()) {
        last = lp->second;
    }

    double float_profit_long = 0.0, float_profit_short = 0.0;
    if (std::isfinite(last)) {
        if (p.volume_long) {
            float_profit_long = (last - position_price_long) * p.volume_long * m;
        }
        if (p.volume_short) {
            float_profit_short = (position_price_short - last) * p.volume_short * m;
        }
    }

    double margin_long =
        p.volume_long ? position_price_long * p.volume_long * m * spec.margin_rate : 0.0;
    double margin_short =
        p.volume_short ? position_price_short * p.volume_short * m * spec.margin_rate : 0.0;

    return nlohmann::json{
        {"instrument_id", symbol},
        {"volume_long", p.volume_long},
        {"volume_short", p.volume_short},
        {"volume_long_today", p.volume_long},
        {"volume_short_today", p.volume_short},
        {"open_price_long", position_price_long},
        {"open_price_short", position_price_short},
        {"position_price_long", position_price_long},
        {"position_price_short", position_price_short},
        {"last_price", last},
        {"float_profit_long", float_profit_long},
        {"float_profit_short", float_profit_short},
        {"float_profit", float_profit_long + float_profit_short},
        {"margin_long", margin_long},
        {"margin_short", margin_short},
        {"margin", margin_long + margin_short},
    };
}

// ---------- 资金重算 ----------
void SimTdGateway::recompute_available() {
    double margin        = 0.0;
    double frozen_margin = 0.0;

    for (auto& [symbol, p] : positions_) {
        auto sit = specs_.find(symbol);
        if (sit == specs_.end()) {
            continue;
        }
        const ContractSpec& spec = sit->second;
        double              m    = spec.volume_multiple;
        double              pl   = p.volume_long ? p.open_cost_long / p.volume_long : 0.0;
        double              ps   = p.volume_short ? p.open_cost_short / p.volume_short : 0.0;
        margin += (pl * p.volume_long + ps * p.volume_short) * m * spec.margin_rate;
    }

    // 挂单冻结：按每笔挂单未成交手数的限价（市价用最新价）预估保证金。
    for (auto& [rid, order] : live_orders_) {
        auto sit = specs_.find(order.instrument_id);
        if (sit == specs_.end()) {
            continue;
        }
        const ContractSpec& spec = sit->second;
        double              px   = order.limit_price;
        if (!std::isfinite(px)) {
            auto lp = last_price_.find(order.instrument_id);
            px      = lp == last_price_.end() ? 0.0 : lp->second;
        }
        frozen_margin += px * spec.volume_multiple * order.volume_left * spec.margin_rate;
    }

    account_.margin        = margin;
    account_.frozen_margin = frozen_margin;
    account_.balance       = account_.pre_balance + account_.close_profit - account_.commission;
    account_.available     = account_.balance - account_.margin - account_.frozen_margin;
    account_.risk_ratio = account_.balance > 0 ? 1.0 - account_.available / account_.balance : 0.0;
}

nlohmann::json SimTdGateway::account_json() const {
    return nlohmann::json{{"balance", account_.balance},
                          {"pre_balance", account_.pre_balance},
                          {"close_profit", account_.close_profit},
                          {"commission", account_.commission},
                          {"margin", account_.margin},
                          {"frozen_margin", account_.frozen_margin},
                          {"available", account_.available},
                          {"risk_ratio", account_.risk_ratio}};
}

void SimTdGateway::remember_order(const OrderId& rid, nlohmann::json json) {
    if (!json.is_object()) {
        return;
    }
    order_history_[rid] = std::move(json);
}

nlohmann::json SimTdGateway::snapshot() const {
    nlohmann::json user = nlohmann::json::object();

    user["orders"] = nlohmann::json::object();
    for (const auto& [rid, o] : order_history_) {
        user["orders"][rid] = o;
    }
    user["trades"] = nlohmann::json::object();
    for (const auto& [tid, t] : trade_history_) {
        user["trades"][tid] = t;
    }
    user["positions"] = nlohmann::json::object();
    for (const auto& [symbol, p] : positions_) {
        (void)p;
        user["positions"][symbol] = position_json(symbol);
    }
    user["accounts"] = nlohmann::json{{"CNY", account_json()}};

    return nlohmann::json{{"trade", {{"", {{"user", std::move(user)}}}}}};
}

// ---------- 投递 ----------
void SimTdGateway::emit(const FillBatch& batch) {
    if (batch.empty()) {
        return;
    }

    nlohmann::json user = nlohmann::json::object();
    if (!batch.orders.empty()) {
        user["orders"] = batch.orders;
    }
    if (!batch.trades.empty()) {
        user["trades"] = batch.trades;
    }
    if (!batch.positions.empty()) {
        user["positions"] = batch.positions;
    }
    if (batch.account_dirty) {
        user["accounts"] = {{"CNY", account_json()}};
    }

    nlohmann::json diff{{"trade", {{"", {{"user", user}}}}}};

    TradeEvent ev{TradeEvent::Kind::Diff, std::move(diff), {}, {}};
    co_spawn_logged(
        ex_,
        [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
            co_await ch_.async_send(std::move(e));
        },
        "SimTd.emit");
}

void SimTdGateway::emit_reject(const OrderId&      rid,
                               const InstrumentId& symbol,
                               const std::string&  msg) {
    nlohmann::json oj{{"order_id", rid},
                      {"instrument_id", symbol},
                      {"status", magic_enum::enum_name(OrderStatus::FINISHED)},
                      {"is_error", true},
                      {"is_dead", true},
                      {"last_msg", msg}};
    remember_order(rid, oj);
    nlohmann::json diff{{"trade", {{"", {{"user", {{"orders", {{rid, oj}}}}}}}}}};

    TradeEvent ev{TradeEvent::Kind::Diff, std::move(diff), {}, {}};
    co_spawn_logged(
        ex_,
        [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
            co_await ch_.async_send(std::move(e));
        },
        "SimTd.emit_reject");
}

} // namespace tqsdk
