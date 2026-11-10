use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 9).clamp(1, 500);
    let k_smooth = ctx.param_usize(1, 3).clamp(1, 100) as f64;
    let d_smooth = ctx.param_usize(2, 3).clamp(1, 100) as f64;
    let high = ctx.high();
    let low = ctx.low();
    let close = ctx.close();
    let mut k_out = vec![f64::NAN; close.len()];
    let mut d_out = vec![f64::NAN; close.len()];
    let mut j_out = vec![f64::NAN; close.len()];
    if close.len() < period {
        let ok = ctx.write_output(0, &k_out)
            && ctx.write_output(1, &d_out)
            && ctx.write_output(2, &j_out);
        return if ok { 0 } else { -1 };
    }
    let mut k = 50.0;
    let mut d = 50.0;
    for index in period - 1..close.len() {
        let mut highest = high[index];
        let mut lowest = low[index];
        for probe in index + 1 - period..=index {
            highest = highest.max(high[probe]);
            lowest = lowest.min(low[probe]);
        }
        let rsv = if highest > lowest {
            (close[index] - lowest) / (highest - lowest) * 100.0
        } else {
            50.0
        };
        k = k * (k_smooth - 1.0) / k_smooth + rsv / k_smooth;
        d = d * (d_smooth - 1.0) / d_smooth + k / d_smooth;
        let j = 3.0 * k - 2.0 * d;
        k_out[index] = k;
        d_out[index] = d;
        j_out[index] = j;
    }
    let ok = ctx.write_output(0, &k_out)
        && ctx.write_output(1, &d_out)
        && ctx.write_output(2, &j_out);
    if ok { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
