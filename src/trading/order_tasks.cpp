#include "tqsdk/trading/order_tasks.h"

#include "tqsdk/tqapi.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

namespace tqsdk {

namespace asio = boost::asio;

namespace {

bool valid_price(double price) {
    return std::isfinite(price) && price > 0.0;
}

std::optional<double> quote_price(const Quote&            quote,
                                  Direction               direction,
                                  TargetPosPriceMode      mode,
                                  const TargetPosOptions& options) {
    if (mode == TargetPosPriceMode::CUSTOM && options.price_provider) {
        return options.price_provider(quote, direction);
    }
    if (mode == TargetPosPriceMode::PASSIVE) {
        return direction == Direction::BUY ? quote.bid_price1 : quote.ask_price1;
    }
    return direction == Direction::BUY ? quote.ask_price1 : quote.bid_price1;
}

} // namespace

struct TargetPosTask::State {
    TqApi*             api = nullptr;
    std::string        symbol;
    TargetPosPriceMode mode = TargetPosPriceMode::ACTIVE;
    TargetPosOptions   options;

    std::atomic<std::int64_t> target{0};
    std::atomic<bool>         pending{false};
    std::atomic<bool>         stopped{false};
    TqApi::UpdateHookId       hook_id = 0;

    std::mutex                  mutex;
    std::unordered_set<OrderId> active_orders;
    std::unordered_set<OrderId> cancelling_orders;
};

TargetPosTask::TargetPosTask(TqApi&             api,
                             std::string        symbol,
                             TargetPosPriceMode mode,
                             TargetPosOptions   options)
    : state_(std::make_shared<State>()) {
    state_->api     = &api;
    state_->symbol  = std::move(symbol);
    state_->mode    = mode;
    state_->options = std::move(options);
    const auto weak = std::weak_ptr<State>(state_);
    state_->hook_id = api.add_update_hook([weak] {
        if (auto state = weak.lock()) {
            on_update(state);
        }
    });
    if (state_->hook_id == 0) {
        state_->stopped = true;
    }
}

TargetPosTask::~TargetPosTask() {
    cancel();
}

void TargetPosTask::set_target_volume(std::int64_t volume) {
    if (state_->stopped) {
        return;
    }
    state_->target.store(volume, std::memory_order_relaxed);
    state_->api->notify_update();
}

std::int64_t TargetPosTask::target_volume() const {
    return state_->target.load(std::memory_order_relaxed);
}

bool TargetPosTask::active() const {
    return !state_->stopped.load(std::memory_order_relaxed);
}

void TargetPosTask::cancel() {
    if (state_->stopped.exchange(true)) {
        return;
    }
    if (state_->api) {
        state_->api->remove_update_hook(state_->hook_id);
    }
    std::vector<OrderId> ids;
    {
        std::lock_guard lock(state_->mutex);
        ids.assign(state_->active_orders.begin(), state_->active_orders.end());
        state_->active_orders.clear();
        state_->cancelling_orders.clear();
    }
    for (const auto& id : ids) {
        asio::co_spawn(
            state_->api->executor(),
            [api = state_->api, id]() -> asio::awaitable<void> {
                co_await api->cancel_order(CancelOrderReq{id, {}});
            },
            asio::detached);
    }
}

void TargetPosTask::on_update(const std::shared_ptr<State>& state) {
    if (!state) {
        return;
    }
    if (state->stopped.load(std::memory_order_relaxed) ||
        state->pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto&                api = *state->api;
    std::vector<OrderId> active;
    {
        std::lock_guard lock(state->mutex);
        for (auto it = state->active_orders.begin(); it != state->active_orders.end();) {
            const auto* order = api.get_order(*it);
            if (order == nullptr || order->is_dead) {
                state->cancelling_orders.erase(*it);
                it = state->active_orders.erase(it);
            } else {
                ++it;
            }
        }
        active.assign(state->active_orders.begin(), state->active_orders.end());
    }

    const auto* quote = api.data().quotes.find(state->symbol);
    if (quote == nullptr) {
        state->pending.store(false, std::memory_order_release);
        return;
    }

    // 行情变化时撤掉旧价单，下一轮更新再按新价重报。
    for (const auto& id : active) {
        const auto* order = api.get_order(id);
        if (order == nullptr || order->is_dead) {
            continue;
        }
        auto desired = quote_price(*quote, order->direction, state->mode, state->options);
        if (!desired.has_value() || !valid_price(*desired)) {
            continue;
        }
        const auto tick =
            std::isfinite(quote->price_tick) && quote->price_tick > 0 ? quote->price_tick : 1e-9;
        if (std::abs(order->limit_price - *desired) <= tick * 0.5) {
            continue;
        }
        bool need_cancel = false;
        {
            std::lock_guard lock(state->mutex);
            need_cancel = state->cancelling_orders.insert(id).second;
        }
        if (need_cancel) {
            asio::co_spawn(
                api.executor(),
                [state, id]() -> asio::awaitable<void> {
                    const auto ack = co_await state->api->cancel_order(CancelOrderReq{id, {}});
                    if (!ack.success) {
                        std::lock_guard lock(state->mutex);
                        state->cancelling_orders.erase(id);
                    }
                },
                asio::detached);
        }
    }
    {
        std::lock_guard lock(state->mutex);
        if (!state->cancelling_orders.empty()) {
            state->pending.store(false, std::memory_order_release);
            return;
        }
    }

    const auto*  position     = api.data().positions.find(state->symbol);
    std::int64_t long_volume  = position ? static_cast<std::int64_t>(position->volume_long) : 0;
    std::int64_t short_volume = position ? static_cast<std::int64_t>(position->volume_short) : 0;

    // 将在途委托折算成 effective long/short，避免重复平仓或重复开仓。
    std::int64_t pending_close_long  = 0; // SELL CLOSE 消耗 long
    std::int64_t pending_close_short = 0; // BUY CLOSE 消耗 short
    std::int64_t pending_open_long   = 0;
    std::int64_t pending_open_short  = 0;
    for (const auto& id : active) {
        const auto* order = api.get_order(id);
        if (order == nullptr || order->is_dead) {
            continue;
        }
        const auto volume = static_cast<std::int64_t>(order->volume_left);
        if (order->direction == Direction::BUY && order->offset == Offset::OPEN) {
            pending_open_long += volume;
        } else if (order->direction == Direction::SELL && order->offset == Offset::OPEN) {
            pending_open_short += volume;
        } else if (order->direction == Direction::BUY) {
            pending_close_short += volume;
        } else {
            pending_close_long += volume;
        }
    }
    long_volume = std::max<std::int64_t>(0, long_volume - pending_close_long) + pending_open_long;
    short_volume =
        std::max<std::int64_t>(0, short_volume - pending_close_short) + pending_open_short;
    const auto effective_net = long_volume - short_volume;
    const auto target        = state->target.load(std::memory_order_relaxed);
    auto       delta         = target - effective_net;

    if (delta == 0) {
        state->pending.store(false, std::memory_order_release);
        return;
    }

    const auto direction = delta > 0 ? Direction::BUY : Direction::SELL;
    auto       price     = quote_price(*quote, direction, state->mode, state->options);
    if (!price.has_value() || !valid_price(*price)) {
        state->pending.store(false, std::memory_order_release);
        return;
    }

    InsertOrderReq request;
    request.instrument_id = state->symbol;
    request.exchange_id   = state->symbol.substr(0, state->symbol.find('.'));
    request.direction     = direction;
    request.price_type    = PriceType::LIMIT;
    request.limit_price   = *price;
    request.time_cond     = TimeCondition::GFD;

    std::int64_t qty = std::abs(delta);
    if (delta > 0 && short_volume > 0) {
        request.offset = Offset::CLOSE;
        qty            = std::min<std::int64_t>(qty, short_volume);
    } else if (delta < 0 && long_volume > 0) {
        request.offset = Offset::CLOSE;
        qty            = std::min<std::int64_t>(qty, long_volume);
    } else {
        request.offset = Offset::OPEN;
    }
    if (state->options.max_volume > 0) {
        qty = std::min<std::int64_t>(qty, static_cast<std::int64_t>(state->options.max_volume));
    }
    if (qty <= 0) {
        state->pending.store(false, std::memory_order_release);
        return;
    }
    request.volume = static_cast<std::uint64_t>(qty);
    const auto rid = state->api->next_order_id();

    asio::co_spawn(
        state->api->executor(),
        [state, request = std::move(request), rid]() mutable -> asio::awaitable<void> {
            try {
                Order* order = co_await state->api->insert_order(request, rid);
                if (order != nullptr && !state->stopped.load(std::memory_order_relaxed)) {
                    std::lock_guard lock(state->mutex);
                    state->active_orders.insert(order->order_id);
                }
            } catch (const std::exception& e) {
                SPDLOG_WARN(
                    "[TargetPosTask] insert failed symbol={} error={}", state->symbol, e.what());
            } catch (...) {
                SPDLOG_WARN("[TargetPosTask] insert failed symbol={} unknown error", state->symbol);
            }
            state->pending.store(false, std::memory_order_release);
        },
        asio::detached);
}

struct InsertOrderTask::State {
    TqApi*         api = nullptr;
    InsertOrderReq request;
    std::uint64_t  desired_volume = 0;
    std::uint32_t  max_retries    = 20;
    std::uint32_t  attempts       = 0;

    std::atomic<std::uint64_t> filled{0};
    std::atomic<bool>          pending{false};
    std::atomic<bool>          stopped{true};
    TqApi::UpdateHookId        hook_id = 0;

    std::mutex mutex;
    OrderId    active_order;
};

InsertOrderTask::InsertOrderTask(TqApi& api, InsertOrderReq request, std::uint32_t max_retries)
    : state_(std::make_shared<State>()) {
    state_->api            = &api;
    state_->request        = std::move(request);
    state_->desired_volume = state_->request.volume;
    state_->max_retries    = max_retries;
    start();
}

InsertOrderTask::~InsertOrderTask() {
    cancel();
}

void InsertOrderTask::start() {
    if (state_->hook_id != 0 || !state_->stopped.load(std::memory_order_relaxed)) {
        return;
    }
    state_->stopped.store(false, std::memory_order_relaxed);
    const auto weak = std::weak_ptr<State>(state_);
    state_->hook_id = state_->api->add_update_hook([weak] {
        if (auto state = weak.lock()) {
            on_update(state);
        }
    });
    if (state_->hook_id == 0) {
        state_->stopped = true;
        return;
    }
    state_->api->notify_update();
}

bool InsertOrderTask::active() const {
    return !state_->stopped.load(std::memory_order_relaxed);
}

std::uint64_t InsertOrderTask::filled_volume() const {
    return state_->filled.load(std::memory_order_relaxed);
}

void InsertOrderTask::cancel() {
    if (state_->stopped.exchange(true)) {
        return;
    }
    if (state_->api) {
        state_->api->remove_update_hook(state_->hook_id);
    }
    OrderId active;
    {
        std::lock_guard lock(state_->mutex);
        active = state_->active_order;
        state_->active_order.clear();
    }
    if (!active.empty()) {
        asio::co_spawn(
            state_->api->executor(),
            [api = state_->api, active]() -> asio::awaitable<void> {
                co_await api->cancel_order(CancelOrderReq{active, {}});
            },
            asio::detached);
    }
}

void InsertOrderTask::on_update(const std::shared_ptr<State>& state) {
    if (!state) {
        return;
    }
    if (state->stopped.load(std::memory_order_relaxed) ||
        state->pending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    auto&   api = *state->api;
    OrderId active;
    {
        std::lock_guard lock(state->mutex);
        active = state->active_order;
    }

    if (!active.empty()) {
        const auto* order = api.get_order(active);
        if (order != nullptr && !order->is_dead) {
            state->pending.store(false, std::memory_order_release);
            return;
        }
        if (order != nullptr) {
            const auto filled = order->volume_orign >= order->volume_left
                                    ? order->volume_orign - order->volume_left
                                    : 0;
            state->filled.fetch_add(filled, std::memory_order_relaxed);
        }
        {
            std::lock_guard lock(state->mutex);
            if (state->active_order == active) {
                state->active_order.clear();
            }
        }
        ++state->attempts;
        if (state->attempts > state->max_retries) {
            state->stopped = true;
            api.remove_update_hook(state->hook_id);
            state->pending.store(false, std::memory_order_release);
            return;
        }
    }

    const auto filled = state->filled.load(std::memory_order_relaxed);
    if (filled >= state->desired_volume) {
        state->stopped = true;
        api.remove_update_hook(state->hook_id);
        state->pending.store(false, std::memory_order_release);
        return;
    }

    auto request   = state->request;
    request.volume = state->desired_volume - filled;
    const auto rid = api.next_order_id();
    asio::co_spawn(
        state->api->executor(),
        [state, request = std::move(request), rid]() mutable -> asio::awaitable<void> {
            try {
                Order* order = co_await state->api->insert_order(request, rid);
                if (order != nullptr && !state->stopped.load(std::memory_order_relaxed)) {
                    std::lock_guard lock(state->mutex);
                    state->active_order = order->order_id;
                }
            } catch (const std::exception& e) {
                SPDLOG_WARN("[InsertOrderTask] insert failed: {}", e.what());
            } catch (...) {
                SPDLOG_WARN("[InsertOrderTask] insert failed: unknown error");
            }
            state->pending.store(false, std::memory_order_release);
        },
        asio::detached);
}

} // namespace tqsdk
