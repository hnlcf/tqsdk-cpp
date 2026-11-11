#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// C ABI used by the ctypes-based Python binding.
/// The C++ BacktestEngine remains the source of truth; this layer only owns
/// handles and copies values across the language boundary.

typedef struct tq_bt_engine  tq_bt_engine;
typedef struct tq_bt_context tq_bt_context;

typedef struct tq_bt_config {
    double initial_balance;
    double volume_multiple;
    double margin_rate;
    double commission_rate;
    double commission_per_lot;
    int    execute_on_bar_close;
} tq_bt_config;

typedef struct tq_bt_bar {
    int64_t  datetime;
    double   open;
    double   high;
    double   low;
    double   close;
    uint64_t volume;
} tq_bt_bar;

typedef struct tq_bt_fill {
    int64_t  datetime;
    int32_t  direction; ///< 0=BUY, 1=SELL
    int32_t  offset;    ///< 0=OPEN, 1=CLOSE
    double   price;
    uint64_t volume;
    double   commission;
    double   realized_pnl;
} tq_bt_fill;

typedef struct tq_bt_stats {
    size_t  bars;
    size_t  fills;
    size_t  win_fills;
    double  initial_balance;
    double  final_equity;
    double  total_return;
    double  realized_pnl;
    double  commission;
    double  max_drawdown;
    double  max_drawdown_pct;
    int64_t final_position;
    /// 0=OK, 1=exception, 2=callback failed
    int32_t     status;
    const char* error;
} tq_bt_stats;

/// Strategy callback receives the current bar and a context valid only during
/// the callback. Return 0 to continue, non-zero to mark the run failed.
typedef int (*tq_bt_strategy_fn)(void* user, const tq_bt_bar* bar, tq_bt_context* context);

tq_bt_engine* tq_bt_create(const tq_bt_config* config);
void          tq_bt_destroy(tq_bt_engine* engine);

int tq_bt_run(tq_bt_engine*     engine,
              const tq_bt_bar*  bars,
              size_t            bars_count,
              tq_bt_strategy_fn strategy,
              void*             user,
              tq_bt_stats*      out_stats);

int64_t tq_bt_position(tq_bt_engine* engine);
double  tq_bt_equity(tq_bt_engine* engine);

size_t tq_bt_fill_count(tq_bt_engine* engine);
int    tq_bt_fill_at(tq_bt_engine* engine, size_t index, tq_bt_fill* out_fill);

const char* tq_bt_last_error(tq_bt_engine* engine);

int64_t tq_bt_context_position(tq_bt_context* context);
double  tq_bt_context_equity(tq_bt_context* context);

int tq_bt_buy_open(tq_bt_context* context, uint64_t volume);
int tq_bt_sell_open(tq_bt_context* context, uint64_t volume);
int tq_bt_buy_close(tq_bt_context* context, uint64_t volume);
int tq_bt_sell_close(tq_bt_context* context, uint64_t volume);
int tq_bt_target_position(tq_bt_context* context, int64_t target);

#ifdef __cplusplus
} // extern "C"
#endif
