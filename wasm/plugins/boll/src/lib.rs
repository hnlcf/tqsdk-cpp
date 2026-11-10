use tqsdk_indicator_sdk::Context;

fn bands(close: &[f64], period: usize, multiplier: f64) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
    let mut upper = vec![f64::NAN; close.len()];
    let mut lower = vec![f64::NAN; close.len()];
    let mut middle = vec![f64::NAN; close.len()];
    let mut sum = 0.0;
    let mut sum_sq = 0.0;
    for index in 0..close.len() {
        let value = close[index];
        sum += value;
        sum_sq += value * value;
        if index >= period {
            let old = close[index - period];
            sum -= old;
            sum_sq -= old * old;
        }
        if index + 1 >= period {
            let mean = sum / period as f64;
            let variance = (sum_sq / period as f64 - mean * mean).max(0.0);
            let stddev = variance.sqrt();
            middle[index] = mean;
            upper[index] = mean + multiplier * stddev;
            lower[index] = mean - multiplier * stddev;
        }
    }
    (upper, lower, middle)
}

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 20).clamp(2, 500);
    let multiplier = ctx.param(1).max(0.1);
    let close = ctx.close();
    let (upper, lower, middle) = bands(close, period, multiplier);
    if !ctx.write_output(0, &upper)
        || !ctx.write_output(1, &lower)
        || !ctx.write_output(2, &middle)
    {
        return -1;
    }
    0
}

tqsdk_indicator_sdk::export_indicator!(indicator);
