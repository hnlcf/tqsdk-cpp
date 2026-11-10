use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 14).clamp(1, 500);
    let high = ctx.high();
    let low = ctx.low();
    let close = ctx.close();
    let mut out = vec![f64::NAN; close.len()];
    if close.len() < period {
        return if ctx.write_output(0, &out) { 0 } else { -1 };
    }
    let mut tr = vec![0.0; close.len()];
    tr[0] = high[0] - low[0];
    for index in 1..close.len() {
        tr[index] = (high[index] - low[index])
            .max((high[index] - close[index - 1]).abs())
            .max((low[index] - close[index - 1]).abs());
    }
    let mut current = tr[..period].iter().sum::<f64>() / period as f64;
    out[period - 1] = current;
    for index in period..close.len() {
        current = (current * (period as f64 - 1.0) + tr[index]) / period as f64;
        out[index] = current;
    }
    if ctx.write_output(0, &out) { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
