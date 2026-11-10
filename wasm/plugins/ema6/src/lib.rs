//! Example TQSDK WASM indicator: exponential moving average.
//!
//! Build:
//!   cargo build --manifest-path wasm/plugins/ema6/Cargo.toml \
//!     --target wasm32-unknown-unknown --release

use tqsdk_indicator_sdk::{Context, FIELD_COUNT, HEADER_BYTES};

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 6).clamp(1, 500);
    let close = ctx.close();
    let mut values = vec![f64::NAN; ctx.bar_count()];
    if !close.is_empty() {
        let alpha = 2.0 / (period as f64 + 1.0);
        let mut current = close[0];
        values[0] = current;
        for index in 1..close.len() {
            current = alpha * close[index] + (1.0 - alpha) * current;
            values[index] = current;
        }
    }
    if ctx.write_output(0, &values) {
        0
    } else {
        -1
    }
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
