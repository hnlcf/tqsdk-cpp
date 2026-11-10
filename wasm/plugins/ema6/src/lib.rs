//! Example TQSDK WASM indicator: multiple exponential moving averages.
//!
//! Build:
//!   cargo build --manifest-path wasm/plugins/ema6/Cargo.toml \
//!     --target wasm32-unknown-unknown --release

use tqsdk_indicator_sdk::{Context, FIELD_COUNT, HEADER_BYTES};

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
        ctx.param_usize(0, 6).clamp(1, 500),
        ctx.param_usize(1, 12).clamp(1, 500),
        ctx.param_usize(2, 24).clamp(1, 500),
        ctx.param_usize(3, 60).clamp(1, 500),
    ];
    let close = ctx.close();
    for (output, period) in periods.into_iter().enumerate() {
        if !ctx.write_output(output, &ema(close, period)) {
            return -1;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn alloc(size: u32) -> *mut u8 {
    tqsdk_indicator_sdk::alloc(size)
}

#[no_mangle]
pub extern "C" fn dealloc(ptr: *mut u8, size: u32) {
    tqsdk_indicator_sdk::dealloc(ptr, size);
}

#[no_mangle]
pub extern "C" fn run(
    input_ptr: *const u8,
    input_len: usize,
    output_ptr: *mut u8,
    output_capacity: usize,
) -> i32 {
    // Keep these constants referenced so the ABI layout stays visible in the
    // compiled plugin source and accidental layout changes fail in tests.
    let _ = (HEADER_BYTES, FIELD_COUNT);
    tqsdk_indicator_sdk::execute(input_ptr, input_len, output_ptr, output_capacity, indicator)
}
