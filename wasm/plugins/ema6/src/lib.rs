//! Example TQSDK WASM indicator: multiple exponential moving averages.
//!
//! Build:
//!   cargo build --manifest-path wasm/plugins/ema6/Cargo.toml \
//!     --target wasm32-unknown-unknown --release

use tqsdk_indicator_sdk::Context;

fn ema(close: &[f64], period: usize) -> Vec<f64> {
    let mut values = vec![f64::NAN; close.len()];
    if close.is_empty() {
        return values;
    }
    let alpha = 2.0 / (period as f64 + 1.0);
    let mut current = close[0];
    values[0] = current;
    for index in 1..close.len() {
        current = alpha * close[index] + (1.0 - alpha) * current;
        values[index] = current;
    }
    values
}

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let periods = [
        ctx.param_usize(0, 5).clamp(1, 500),
        ctx.param_usize(1, 10).clamp(1, 500),
        ctx.param_usize(2, 20).clamp(1, 500),
        ctx.param_usize(3, 60).clamp(1, 500),
        ctx.param_usize(4, 120).clamp(1, 500),
        ctx.param_usize(5, 200).clamp(1, 500),
        ctx.param_usize(6, 250).clamp(1, 500),
        ctx.param_usize(7, 500).clamp(1, 500),
    ];
    let close = ctx.close();
    for (output, period) in periods.into_iter().enumerate() {
        if !ctx.write_output(output, &ema(close, period)) {
            return -1;
        }
    }
    0
}

tqsdk_indicator_sdk::export_indicator!(indicator);
