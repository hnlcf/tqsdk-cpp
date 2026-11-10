use tqsdk_indicator_sdk::Context;

fn ema(values: &[f64], period: usize) -> Vec<f64> {
    let mut out = vec![f64::NAN; values.len()];
    if values.is_empty() {
        return out;
    }
    let alpha = 2.0 / (period as f64 + 1.0);
    let mut current = values[0];
    out[0] = current;
    for index in 1..values.len() {
        current = alpha * values[index] + (1.0 - alpha) * current;
        out[index] = current;
    }
    out
}

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let fast = ctx.param_usize(0, 12).clamp(1, 500);
    let slow = ctx.param_usize(1, 26).clamp(1, 500);
    let signal = ctx.param_usize(2, 9).clamp(1, 500);
    let close = ctx.close();
    let fast_ema = ema(close, fast);
    let slow_ema = ema(close, slow);
    let dif: Vec<f64> = fast_ema
        .iter()
        .zip(slow_ema.iter())
        .map(|(a, b)| a - b)
        .collect();
    let dea = ema(&dif, signal);
    let histogram: Vec<f64> = dif
        .iter()
        .zip(dea.iter())
        .map(|(a, b)| 2.0 * (a - b))
        .collect();
    if !ctx.write_output(0, &dif)
        || !ctx.write_output(1, &dea)
        || !ctx.write_output(2, &histogram)
    {
        return -1;
    }
    0
}

tqsdk_indicator_sdk::export_indicator!(indicator);
