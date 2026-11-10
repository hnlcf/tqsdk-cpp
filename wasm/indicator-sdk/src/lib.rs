//! Minimal, dependency-free ABI helpers for TQSDK WebAssembly indicators.
//!
//! This crate intentionally has no host imports. A plugin receives only the
//! historical OHLCV buffers and numeric parameters, and returns numeric output
//! arrays. Networking, DOM, files and timers are not part of the ABI.

use std::alloc::{alloc as system_alloc, dealloc as system_dealloc, Layout};
use std::slice;

pub const HEADER_BYTES: usize = 24;
pub const MAGIC: u32 = 0x3157_5154; // "TQW1" little-endian
pub const ABI_VERSION: u32 = 1;
pub const FIELD_COUNT: usize = 5;
pub const MAX_BARS: usize = 20_000;
pub const MAX_PARAMS: usize = 8;
pub const MAX_OUTPUTS: usize = 16;

/// Aligned allocation used by the JS host through the exported `alloc` function.
pub extern "C" fn alloc(size: u32) -> *mut u8 {
    let size = (size as usize).max(1);
    // 8-byte alignment is required by the f64 output/input buffers.
    match Layout::from_size_align(size, 8) {
        Ok(layout) => unsafe { system_alloc(layout) },
        Err(_) => std::ptr::null_mut(),
    }
}

pub extern "C" fn dealloc(ptr: *mut u8, size: u32) {
    if ptr.is_null() {
        return;
    }
    let size = (size as usize).max(1);
    if let Ok(layout) = Layout::from_size_align(size, 8) {
        unsafe { system_dealloc(ptr, layout) };
    }
}

pub struct Context<'a> {
    bar_count: usize,
    params: &'a [f64],
    fields: &'a [f64],
    output: &'a mut [f64],
    output_count: usize,
}

impl<'a> Context<'a> {
    pub fn bar_count(&self) -> usize {
        self.bar_count
    }

    pub fn param(&self, index: usize) -> f64 {
        self.params.get(index).copied().unwrap_or(0.0)
    }

    pub fn param_usize(&self, index: usize, fallback: usize) -> usize {
        let value = self.param(index);
        if value.is_finite() && value > 0.0 {
            value as usize
        } else {
            fallback
        }
    }

    fn field(&self, index: usize) -> &'a [f64] {
        let start = index * self.bar_count;
        &self.fields[start..start + self.bar_count]
    }

    pub fn open(&self) -> &'a [f64] {
        self.field(0)
    }

    pub fn high(&self) -> &'a [f64] {
        self.field(1)
    }

    pub fn low(&self) -> &'a [f64] {
        self.field(2)
    }

    pub fn close(&self) -> &'a [f64] {
        self.field(3)
    }

    pub fn volume(&self) -> &'a [f64] {
        self.field(4)
    }

    pub fn output_count(&self) -> usize {
        self.output_count
    }

    pub fn output(&mut self, index: usize) -> &mut [f64] {
        let start = index * self.bar_count;
        &mut self.output[start..start + self.bar_count]
    }

    /// Copy a full output series. Returns false for an invalid index or length.
    pub fn write_output(&mut self, index: usize, values: &[f64]) -> bool {
        if index >= self.output_count || values.len() != self.bar_count {
            return false;
        }
        self.output(index).copy_from_slice(values);
        true
    }
}

fn read_u32(input: &[u8], offset: usize) -> Option<u32> {
    let bytes = input.get(offset..offset + 4)?;
    Some(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// Execute a plugin entry point against the TQW1 binary input layout.
///
/// The entry point must write every declared output and return 0. A non-zero
/// return is propagated as a negative error code.
pub fn execute(
    input_ptr: *const u8,
    input_len: usize,
    output_ptr: *mut u8,
    output_capacity: usize,
    entry: fn(&mut Context<'_>) -> i32,
) -> i32 {
    if input_ptr.is_null() || output_ptr.is_null() || input_len < HEADER_BYTES {
        return -1;
    }
    let input = unsafe { slice::from_raw_parts(input_ptr, input_len) };
    let (Some(magic), Some(version), Some(bars), Some(params_count), Some(outputs_count)) = (
        read_u32(input, 0),
        read_u32(input, 4),
        read_u32(input, 8),
        read_u32(input, 12),
        read_u32(input, 16),
    ) else {
        return -2;
    };
    if magic != MAGIC || version != ABI_VERSION {
        return -3;
    }
    let bars = bars as usize;
    let params_count = params_count as usize;
    let outputs_count = outputs_count as usize;
    if bars > MAX_BARS
        || params_count > MAX_PARAMS
        || outputs_count == 0
        || outputs_count > MAX_OUTPUTS
    {
        return -4;
    }
    let expected_input = HEADER_BYTES + params_count * 8 + FIELD_COUNT * bars * 8;
    let output_len = outputs_count * bars * 8;
    if input_len != expected_input || output_capacity != output_len {
        return -5;
    }

    let params =
        unsafe { slice::from_raw_parts(input_ptr.add(HEADER_BYTES) as *const f64, params_count) };
    let fields = unsafe {
        slice::from_raw_parts(
            input_ptr.add(HEADER_BYTES + params_count * 8) as *const f64,
            FIELD_COUNT * bars,
        )
    };
    let output = unsafe { slice::from_raw_parts_mut(output_ptr as *mut f64, outputs_count * bars) };
    let mut context = Context {
        bar_count: bars,
        params,
        fields,
        output,
        output_count: outputs_count,
    };
    if entry(&mut context) != 0 {
        return -6;
    }
    output_len as i32
}

/// Export the standard WebAssembly entry points for a plugin.
///
/// The plugin only needs to provide an entry function:
///
/// ```ignore
/// fn indicator(ctx: &mut Context<'_>) -> i32 { ... }
/// tqsdk_indicator_sdk::export_indicator!(indicator);
/// ```
#[macro_export]
macro_rules! export_indicator {
    ($entry:path) => {
        #[no_mangle]
        pub extern "C" fn alloc(size: u32) -> *mut u8 {
            $crate::alloc(size)
        }

        #[no_mangle]
        pub extern "C" fn dealloc(ptr: *mut u8, size: u32) {
            $crate::dealloc(ptr, size);
        }

        #[no_mangle]
        pub extern "C" fn run(
            input_ptr: *const u8,
            input_len: usize,
            output_ptr: *mut u8,
            output_capacity: usize,
        ) -> i32 {
            $crate::execute(input_ptr, input_len, output_ptr, output_capacity, $entry)
        }
    };
}
