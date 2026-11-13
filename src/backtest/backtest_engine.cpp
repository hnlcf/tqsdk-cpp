#include "tqsdk/backtest/backtest_engine.h"

#include <algorithm>

namespace tqsdk {

namespace {

double bar_price(const BacktestConfig& cfg, const BacktestBar& bar) {
    if (cfg.execute_on_bar_close && std::isfinite(bar.close) && bar.close > 0) {
        return bar.close;
    }
    if (std::isfinite(bar.open) && bar.open > 0) {
        return bar.open;
    }
    return bar.close;
}

} // namespace

double BacktestEngine::execution_price(const BacktestBar& bar) const {
    return bar_price(config_, bar);
}

double BacktestEngine::equity() const noexcept {
    const double unrealized =
        (last_price_ - avg_long_) * static_cast<double>(long_volume_) * config_.volume_multiple +
        (avg_short_ - last_price_) * static_cast<double>(short_volume_) * config_.volume_multiple;
    return config_.initial_balance + realized_pnl_ - commission_ +
           (long_volume_ > 0 || short_volume_ > 0 ? unrealized : 0.0);
}

void BacktestEngine::mark(const BacktestBar& bar) {
    const double price = execution_price(bar);
    if (std::isfinite(price) && price > 0) {
        last_price_ = price;
    }
    current_datetime_    = bar.datetime;
    const double current = equity();
    if (current > peak_equity_) {
        peak_equity_ = current;
    }
    const double drawdown = peak_equity_ - current;
    if (drawdown > max_drawdown_) {
        max_drawdown_     = drawdown;
        max_drawdown_pct_ = peak_equity_ > 0 ? drawdown / peak_equity_ * 100.0 : 0.0;
    }
}

void BacktestEngine::mark_tick(const Tick& tick) {
    if (std::isfinite(tick.last_price) && tick.last_price > 0) {
        last_price_ = tick.last_price;
    }
    // Tick.datetime 是字符串；tick 回测暂不伪造 TimeStamp，成交 datetime 保持 0。
    current_datetime_    = 0;
    const double current = equity();
    if (current > peak_equity_) {
        peak_equity_ = current;
    }
    const double drawdown = peak_equity_ - current;
    if (drawdown > max_drawdown_) {
        max_drawdown_     = drawdown;
        max_drawdown_pct_ = peak_equity_ > 0 ? drawdown / peak_equity_ * 100.0 : 0.0;
    }
}

void BacktestEngine::record_fill(Direction direction,
                                 Offset    offset,
                                 uint64_t  volume,
                                 double    price,
                                 double    commission,
                                 double    realized) {
    BacktestFill fill;
    fill.datetime     = current_datetime_;
    fill.direction    = direction;
    fill.offset       = offset;
    fill.price        = price;
    fill.volume       = volume;
    fill.commission   = commission;
    fill.realized_pnl = realized;
    fills_.push_back(fill);
    commission_ += commission;
    realized_pnl_ += realized;
}

bool BacktestEngine::close_short(uint64_t volume, double price) {
    const auto qty = std::min<uint64_t>(volume, short_volume_);
    if (qty == 0) {
        return false;
    }
    const double pnl = (avg_short_ - price) * static_cast<double>(qty) * config_.volume_multiple;
    const double commission =
        config_.commission_per_lot * static_cast<double>(qty) +
        config_.commission_rate * price * config_.volume_multiple * static_cast<double>(qty);
    short_volume_ -= qty;
    if (short_volume_ == 0) {
        avg_short_ = 0.0;
    }
    record_fill(Direction::BUY, Offset::CLOSE, qty, price, commission, pnl);
    return true;
}

bool BacktestEngine::close_long(uint64_t volume, double price) {
    const auto qty = std::min<uint64_t>(volume, long_volume_);
    if (qty == 0) {
        return false;
    }
    const double pnl = (price - avg_long_) * static_cast<double>(qty) * config_.volume_multiple;
    const double commission =
        config_.commission_per_lot * static_cast<double>(qty) +
        config_.commission_rate * price * config_.volume_multiple * static_cast<double>(qty);
    long_volume_ -= qty;
    if (long_volume_ == 0) {
        avg_long_ = 0.0;
    }
    record_fill(Direction::SELL, Offset::CLOSE, qty, price, commission, pnl);
    return true;
}

void BacktestEngine::open_long(uint64_t volume, double price) {
    if (volume == 0) {
        return;
    }
    const auto total = static_cast<double>(long_volume_ + volume);
    avg_long_        = (avg_long_ * static_cast<double>(long_volume_) + price * volume) / total;
    long_volume_ += volume;
    const double commission =
        config_.commission_per_lot * static_cast<double>(volume) +
        config_.commission_rate * price * config_.volume_multiple * static_cast<double>(volume);
    record_fill(Direction::BUY, Offset::OPEN, volume, price, commission, 0.0);
}

void BacktestEngine::open_short(uint64_t volume, double price) {
    if (volume == 0) {
        return;
    }
    const auto total = static_cast<double>(short_volume_ + volume);
    avg_short_       = (avg_short_ * static_cast<double>(short_volume_) + price * volume) / total;
    short_volume_ += volume;
    const double commission =
        config_.commission_per_lot * static_cast<double>(volume) +
        config_.commission_rate * price * config_.volume_multiple * static_cast<double>(volume);
    record_fill(Direction::SELL, Offset::OPEN, volume, price, commission, 0.0);
}

bool BacktestEngine::execute(Direction direction, Offset offset, uint64_t volume) {
    if (volume == 0 || last_price_ <= 0 || !std::isfinite(last_price_)) {
        return false;
    }
    const double price     = last_price_;
    uint64_t     remaining = volume;

    if (direction == Direction::BUY) {
        if (offset == Offset::CLOSE) {
            return close_short(volume, price);
        }
        if (short_volume_ > 0) {
            const auto qty = std::min<uint64_t>(remaining, short_volume_);
            close_short(qty, price);
            remaining -= qty;
        }
        open_long(remaining, price);
    } else {
        if (offset == Offset::CLOSE) {
            return close_long(volume, price);
        }
        if (long_volume_ > 0) {
            const auto qty = std::min<uint64_t>(remaining, long_volume_);
            close_long(qty, price);
            remaining -= qty;
        }
        open_short(remaining, price);
    }
    return true;
}

BacktestStats BacktestEngine::run(const std::vector<BacktestBar>& bars) {
    return run(bars, {});
}

BacktestStats BacktestEngine::run(const std::vector<BacktestBar>& bars, Strategy strategy) {
    fills_.clear();
    realized_pnl_     = 0.0;
    commission_       = 0.0;
    peak_equity_      = config_.initial_balance;
    max_drawdown_     = 0.0;
    max_drawdown_pct_ = 0.0;
    last_price_       = 0.0;
    current_datetime_ = 0;
    long_volume_      = 0;
    short_volume_     = 0;
    avg_long_         = 0.0;
    avg_short_        = 0.0;

    for (const auto& bar : bars) {
        mark(bar);
        if (strategy) {
            BacktestContext context(*this, bar);
            strategy(context);
        }
    }
    if (!bars.empty()) {
        mark(bars.back());
    }

    BacktestStats stats;
    stats.bars             = bars.size();
    stats.fills            = fills_.size();
    stats.win_fills        = static_cast<std::size_t>(std::count_if(
        fills_.begin(), fills_.end(), [](const auto& fill) { return fill.realized_pnl > 0.0; }));
    stats.initial_balance  = config_.initial_balance;
    stats.final_equity     = equity();
    stats.total_return     = stats.final_equity - stats.initial_balance;
    stats.realized_pnl     = realized_pnl_;
    stats.commission       = commission_;
    stats.max_drawdown     = max_drawdown_;
    stats.max_drawdown_pct = max_drawdown_pct_;
    stats.final_position =
        static_cast<std::int64_t>(long_volume_) - static_cast<std::int64_t>(short_volume_);
    return stats;
}

BacktestStats BacktestEngine::run_ticks(const std::vector<Tick>& ticks, TickStrategy strategy) {
    fills_.clear();
    realized_pnl_     = 0.0;
    commission_       = 0.0;
    peak_equity_      = config_.initial_balance;
    max_drawdown_     = 0.0;
    max_drawdown_pct_ = 0.0;
    last_price_       = 0.0;
    current_datetime_ = 0;
    long_volume_      = 0;
    short_volume_     = 0;
    avg_long_         = 0.0;
    avg_short_        = 0.0;

    for (const auto& tick : ticks) {
        mark_tick(tick);
        if (strategy) {
            BacktestTickContext context(*this, tick);
            strategy(context);
        }
    }
    if (!ticks.empty()) {
        mark_tick(ticks.back());
    }

    BacktestStats stats;
    stats.bars             = ticks.size();
    stats.fills            = fills_.size();
    stats.win_fills        = static_cast<std::size_t>(std::count_if(
        fills_.begin(), fills_.end(), [](const auto& fill) { return fill.realized_pnl > 0.0; }));
    stats.initial_balance  = config_.initial_balance;
    stats.final_equity     = equity();
    stats.total_return     = stats.final_equity - stats.initial_balance;
    stats.realized_pnl     = realized_pnl_;
    stats.commission       = commission_;
    stats.max_drawdown     = max_drawdown_;
    stats.max_drawdown_pct = max_drawdown_pct_;
    stats.final_position =
        static_cast<std::int64_t>(long_volume_) - static_cast<std::int64_t>(short_volume_);
    return stats;
}

std::int64_t BacktestContext::position() const noexcept {
    return engine_->position();
}

double BacktestContext::equity() const noexcept {
    return engine_->equity();
}

bool BacktestContext::buy_open(uint64_t volume) {
    return engine_->execute(Direction::BUY, Offset::OPEN, volume);
}

bool BacktestContext::sell_open(uint64_t volume) {
    return engine_->execute(Direction::SELL, Offset::OPEN, volume);
}

bool BacktestContext::buy_close(uint64_t volume) {
    return engine_->execute(Direction::BUY, Offset::CLOSE, volume);
}

bool BacktestContext::sell_close(uint64_t volume) {
    return engine_->execute(Direction::SELL, Offset::CLOSE, volume);
}

bool BacktestContext::target_position(std::int64_t target) {
    const auto current = engine_->position();
    if (target == current) {
        return true;
    }
    if (target > current) {
        return engine_->execute(
            Direction::BUY, Offset::OPEN, static_cast<uint64_t>(target - current));
    }
    return engine_->execute(Direction::SELL, Offset::OPEN, static_cast<uint64_t>(current - target));
}

std::int64_t BacktestTickContext::position() const noexcept {
    return engine_->position();
}

double BacktestTickContext::equity() const noexcept {
    return engine_->equity();
}

bool BacktestTickContext::buy_open(uint64_t volume) {
    return engine_->execute(Direction::BUY, Offset::OPEN, volume);
}

bool BacktestTickContext::sell_open(uint64_t volume) {
    return engine_->execute(Direction::SELL, Offset::OPEN, volume);
}

bool BacktestTickContext::buy_close(uint64_t volume) {
    return engine_->execute(Direction::BUY, Offset::CLOSE, volume);
}

bool BacktestTickContext::sell_close(uint64_t volume) {
    return engine_->execute(Direction::SELL, Offset::CLOSE, volume);
}

bool BacktestTickContext::target_position(std::int64_t target) {
    const auto current = engine_->position();
    if (target == current) {
        return true;
    }
    if (target > current) {
        return engine_->execute(
            Direction::BUY, Offset::OPEN, static_cast<uint64_t>(target - current));
    }
    return engine_->execute(Direction::SELL, Offset::OPEN, static_cast<uint64_t>(current - target));
}

} // namespace tqsdk
