#include "tqsdk/backtest/backtest_c_api.h"

#include "tqsdk/backtest/backtest_engine.h"

#include <memory>
#include <string>
#include <vector>

using namespace tqsdk;

struct tq_bt_engine {
    explicit tq_bt_engine(const BacktestConfig& config)
        : engine(config) {}

    tqsdk::BacktestEngine engine;
    std::string           error;
};

namespace {

void fill_stats(const tqsdk::BacktestStats& source, tq_bt_stats* target) {
    target->bars             = source.bars;
    target->fills            = source.fills;
    target->win_fills        = source.win_fills;
    target->initial_balance  = source.initial_balance;
    target->final_equity     = source.final_equity;
    target->total_return     = source.total_return;
    target->realized_pnl     = source.realized_pnl;
    target->commission       = source.commission;
    target->max_drawdown     = source.max_drawdown;
    target->max_drawdown_pct = source.max_drawdown_pct;
    target->final_position   = source.final_position;
    target->status           = 0;
    target->error            = nullptr;
}

} // namespace

extern "C" {

tq_bt_engine* tq_bt_create(const tq_bt_config* config) {
    try {
        BacktestConfig cfg;
        if (config != nullptr) {
            cfg.initial_balance      = config->initial_balance;
            cfg.volume_multiple      = config->volume_multiple;
            cfg.margin_rate          = config->margin_rate;
            cfg.commission_rate      = config->commission_rate;
            cfg.commission_per_lot   = config->commission_per_lot;
            cfg.execute_on_bar_close = config->execute_on_bar_close != 0;
        }
        return new tq_bt_engine(cfg);
    } catch (...) {
        return nullptr;
    }
}

void tq_bt_destroy(tq_bt_engine* engine) {
    delete engine;
}

int tq_bt_run(tq_bt_engine*     engine,
              const tq_bt_bar*  bars,
              size_t            bars_count,
              tq_bt_strategy_fn strategy,
              void*             user,
              tq_bt_stats*      out_stats) {
    if (engine == nullptr || out_stats == nullptr || (bars == nullptr && bars_count != 0)) {
        return -1;
    }
    try {
        std::vector<BacktestBar> converted;
        converted.reserve(bars_count);
        for (size_t i = 0; i < bars_count; ++i) {
            converted.push_back(BacktestBar{static_cast<TimeStamp>(bars[i].datetime),
                                            bars[i].open,
                                            bars[i].high,
                                            bars[i].low,
                                            bars[i].close,
                                            bars[i].volume});
        }

        bool                     callback_failed = false;
        BacktestEngine::Strategy callback;
        if (strategy != nullptr) {
            callback = [&](BacktestContext& context) {
                tq_bt_bar cbar{static_cast<int64_t>(context.bar().datetime),
                               context.bar().open,
                               context.bar().high,
                               context.bar().low,
                               context.bar().close,
                               context.bar().volume};
                auto*     opaque = reinterpret_cast<tq_bt_context*>(&context);
                if (strategy(user, &cbar, opaque) != 0) {
                    callback_failed = true;
                }
            };
        }
        const auto stats = engine->engine.run(converted, std::move(callback));
        fill_stats(stats, out_stats);
        if (callback_failed) {
            engine->error     = "Python strategy callback returned an error";
            out_stats->status = 2;
            out_stats->error  = engine->error.c_str();
        } else {
            engine->error.clear();
        }
        return 0;
    } catch (const std::exception& e) {
        engine->error     = e.what();
        out_stats->status = 1;
        out_stats->error  = engine->error.c_str();
        return -1;
    } catch (...) {
        engine->error     = "unknown backtest exception";
        out_stats->status = 1;
        out_stats->error  = engine->error.c_str();
        return -1;
    }
}

int64_t tq_bt_position(tq_bt_engine* engine) {
    return engine == nullptr ? 0 : engine->engine.position();
}

double tq_bt_equity(tq_bt_engine* engine) {
    return engine == nullptr ? 0.0 : engine->engine.equity();
}

size_t tq_bt_fill_count(tq_bt_engine* engine) {
    return engine == nullptr ? 0 : engine->engine.fills().size();
}

int tq_bt_fill_at(tq_bt_engine* engine, size_t index, tq_bt_fill* out_fill) {
    if (engine == nullptr || out_fill == nullptr || index >= engine->engine.fills().size()) {
        return -1;
    }
    const auto& source     = engine->engine.fills()[index];
    out_fill->datetime     = source.datetime;
    out_fill->direction    = static_cast<int32_t>(source.direction);
    out_fill->offset       = static_cast<int32_t>(source.offset);
    out_fill->price        = source.price;
    out_fill->volume       = source.volume;
    out_fill->commission   = source.commission;
    out_fill->realized_pnl = source.realized_pnl;
    return 0;
}

const char* tq_bt_last_error(tq_bt_engine* engine) {
    return engine == nullptr ? "null engine" : engine->error.c_str();
}

int64_t tq_bt_context_position(tq_bt_context* context) {
    return context == nullptr ? 0 : reinterpret_cast<BacktestContext*>(context)->position();
}

double tq_bt_context_equity(tq_bt_context* context) {
    return context == nullptr ? 0.0 : reinterpret_cast<BacktestContext*>(context)->equity();
}

int tq_bt_buy_open(tq_bt_context* context, uint64_t volume) {
    return context == nullptr
               ? 0
               : static_cast<int>(reinterpret_cast<BacktestContext*>(context)->buy_open(volume));
}

int tq_bt_sell_open(tq_bt_context* context, uint64_t volume) {
    return context == nullptr
               ? 0
               : static_cast<int>(reinterpret_cast<BacktestContext*>(context)->sell_open(volume));
}

int tq_bt_buy_close(tq_bt_context* context, uint64_t volume) {
    return context == nullptr
               ? 0
               : static_cast<int>(reinterpret_cast<BacktestContext*>(context)->buy_close(volume));
}

int tq_bt_sell_close(tq_bt_context* context, uint64_t volume) {
    return context == nullptr
               ? 0
               : static_cast<int>(reinterpret_cast<BacktestContext*>(context)->sell_close(volume));
}

int tq_bt_target_position(tq_bt_context* context, int64_t target) {
    return context == nullptr
               ? 0
               : static_cast<int>(
                     reinterpret_cast<BacktestContext*>(context)->target_position(target));
}

} // extern "C"
