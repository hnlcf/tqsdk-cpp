use tqsdk_indicator_sdk::Context;

fn sma(close: &[f64], period: usize) -> Vec<f64> {
    let mut out = vec![f64::NAN; close.len()];
    let mut sum = 0.0;
    for index in 0..close.len() {
        sum += close[index];
        if index >= period {
            sum -= close[index - period];
        }
        if index + 1 >= period {
            out[index] = sum / period as f64;
        }
    }
    out
}

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let periods = [
        ctx.param_usize(0, 5).clamp(1, 500),
        ctx.param_usize(1, 10).clamp(1, 500),
        ctx.param_usize(2, 20).clamp(1, 500),
        ctx.param_usize(3, 60).clamp(1, 500),
    ];
    let close = ctx.close();
    for (output, period) in periods.into_iter().enumerate() {
        if !ctx.write_output(output, &sma(close, period)) {
            return -1;
        }
    }
    0
}

tqsdk_indicator_sdk::export_indicator!(indicator);
