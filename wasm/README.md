# TQSDK WASM Indicators

This directory contains the Rust SDK and example plugins for the WebUI
custom-indicator runtime.

## Layout

```text
wasm/
  indicator-sdk/       ABI helpers, allocator, input parser
  plugins/ema6/        multi EMA example plugin and manifest
```

The generated public assets live in:

```text
web/public/plugins/ema6/
  manifest.json
  ema6.wasm
  sha256.txt
```

## Build

```bash
rustup target add wasm32-unknown-unknown
./scripts/build_wasm_plugins.sh
```

The script builds with:

```text
--target wasm32-unknown-unknown --release
```

No WASI imports are used. The worker rejects any module that imports host
functions.

## Plugin contract

A plugin exports:

```text
memory
alloc(size) -> ptr
dealloc(ptr, size)
run(input_ptr, input_len, output_ptr, output_capacity) -> written_bytes
```

Input format `TQW1` and output layout are documented in
[docs/WEB_CUSTOM_INDICATORS.md](../docs/WEB_CUSTOM_INDICATORS.md).

## Writing a plugin

The bundled example exports four EMA outputs with periods 6/12/24/60.

The plugin only computes numbers:

```rust
use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let close = ctx.close();
    let period = ctx.param_usize(0, 6).clamp(1, 500);
    let mut out = vec![f64::NAN; ctx.bar_count()];
    // compute...
    if ctx.write_output(0, &out) { 0 } else { -1 }
}

#[no_mangle]
pub extern "C" fn run(
    input_ptr: *const u8,
    input_len: usize,
    output_ptr: *mut u8,
    output_capacity: usize,
) -> i32 {
    tqsdk_indicator_sdk::execute(
        input_ptr, input_len, output_ptr, output_capacity, indicator)
}
```

Declare plot metadata in `manifest.json`; the host owns the chart and creates
line/histogram/area/band/markers series. Plugins cannot draw arbitrary pixels
or access the DOM.

## Security boundary

Current runtime:

```text
dedicated Worker
  + WebAssembly with zero imports
  + 5 second compute timeout
  + strict manifest/path validation
  + output size validation
```

This is suitable for first-party plugins. Third-party distribution still needs
manifest signing, memory quotas and/or a unique-origin sandbox iframe.
