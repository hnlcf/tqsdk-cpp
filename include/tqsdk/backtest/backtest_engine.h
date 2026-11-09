#pragma once

#include "tqsdk/pch.h"
#include "tqsdk/types.h"

namespace tqsdk {

struct BacktestBar {
    TimeStamp datetime = 0;
    double    open     = 0.0;
    double    high     = 0.0;
    double    low      = 0.0;
    double    close    = 0.0;
    uint64_t  volume   = 0;
};

struct BacktestConfig {
    double initial_balance      = 1'000'000.0;
    double volume_multiple      = 1.0;
    double margin_rate          = 0.1;
    double commission_rate      = 0.0;
    double commission_per_lot   = 0.0;
    bool   execute_on_bar_close = true;
};

struct BacktestFill {
    TimeStamp datetime     = 0;
    Direction direction    = Direction::BUY;
    Offset    offset       = Offset::OPEN;
    double    price        = 0.0;
    uint64_t  volume       = 0;
    double    commission   = 0.0;
    double    realized_pnl = 0.0;
};

class BacktestEngine;

struct BacktestStats {
    std::size_t  bars             = 0;
    std::size_t  fills            = 0;
    std::size_t  win_fills        = 0;
    double       initial_balance  = 0.0;
    double       final_equity     = 0.0;
    double       total_return     = 0.0;
    double       realized_pnl     = 0.0;
    double       commission       = 0.0;
    double       max_drawdown     = 0.0;
    double       max_drawdown_pct = 0.0;
    std::int64_t final_position   = 0;
};

class BacktestContext {
public:
    const BacktestBar& bar() const noexcept { return *bar_; }
    std::int64_t       position() const noexcept;
    double             equity() const noexcept;

    bool buy_open(uint64_t volume);
    bool sell_open(uint64_t volume);
    bool buy_close(uint64_t volume);
    bool sell_close(uint64_t volume);
    bool target_position(std::int64_t target);

private:
    friend class BacktestEngine;
    BacktestContext(BacktestEngine& engine, const BacktestBar& bar)
        : engine_(&engine),
          bar_(&bar) {}
    BacktestEngine*    engine_;
    const BacktestBar* bar_;
};

class BacktestEngine {
public:
    using Strategy = std::function<void(BacktestContext&)>;

    explicit BacktestEngine(BacktestConfig config)
        : config_(config) {}

    BacktestStats run(const std::vector<BacktestBar>& bars);
    BacktestStats run(const std::vector<BacktestBar>& bars, Strategy strategy);

    const BacktestConfig& config() const noexcept { return config_; }
    std::int64_t          position() const noexcept { return long_volume_ - short_volume_; }
    double                equity() const noexcept;
    const std::vector<BacktestFill>& fills() const noexcept { return fills_; }

private:
    friend class BacktestContext;

    bool   execute(Direction direction, Offset offset, uint64_t volume);
    bool   close_short(uint64_t volume, double price);
    bool   close_long(uint64_t volume, double price);
    void   open_long(uint64_t volume, double price);
    void   open_short(uint64_t volume, double price);
    void   record_fill(Direction direction,
                       Offset    offset,
                       uint64_t  volume,
                       double    price,
                       double    commission,
                       double    realized);
    double execution_price(const BacktestBar& bar) const;
    void   mark(const BacktestBar& bar);

    BacktestConfig            config_;
    std::vector<BacktestFill> fills_;
    double                    realized_pnl_     = 0.0;
    double                    commission_       = 0.0;
    double                    peak_equity_      = 0.0;
    double                    max_drawdown_     = 0.0;
    double                    max_drawdown_pct_ = 0.0;
    double                    last_price_       = 0.0;
    TimeStamp                 current_datetime_ = 0;
    std::uint64_t             long_volume_      = 0;
    std::uint64_t             short_volume_     = 0;
    double                    avg_long_         = 0.0;
    double                    avg_short_        = 0.0;
};

} // namespace tqsdk
