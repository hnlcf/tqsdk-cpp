use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 20).clamp(1, 500);
    let high = ctx.high();
    let low = ctx.low();
    let close = ctx.close();
    let mut out = vec![f64::NAN; close.len()];
    if close.len() < period {
        return if ctx.write_output(0, &out) { 0 } else { -1 };
    }
    let mut typical = vec![0.0; close.len()];
    for index in 0..close.len() {
        typical[index] = (high[index] + low[index] + close[index]) / 3.0;
    }
    for index in period - 1..close.len() {
        let start = index + 1 - period;
        let mean = typical[start..=index].iter().sum::<f64>() / period as f64;
        let deviation = typical[start..=index]
            .iter()
            .map(|value| (value - mean).abs())
            .sum::<f64>()
            / period as f64;
        out[index] = if deviation > 0.0 {
            (typical[index] - mean) / (0.015 * deviation)
        } else {
            0.0
        };
    }
    if ctx.write_output(0, &out) { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
