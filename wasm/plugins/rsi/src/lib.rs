use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 14).clamp(2, 500);
    let close = ctx.close();
    let mut out = vec![f64::NAN; close.len()];
    if close.len() <= period {
        return if ctx.write_output(0, &out) { 0 } else { -1 };
    }
    let mut gain = 0.0;
    let mut loss = 0.0;
    for index in 1..=period {
        let change = close[index] - close[index - 1];
        if change >= 0.0 {
            gain += change;
        } else {
            loss -= change;
        }
    }
    let mut avg_gain = gain / period as f64;
    let mut avg_loss = loss / period as f64;
    out[period] = if avg_loss == 0.0 {
        if avg_gain == 0.0 { 50.0 } else { 100.0 }
    } else {
        100.0 - 100.0 / (1.0 + avg_gain / avg_loss)
    };
    for index in (period + 1)..close.len() {
        let change = close[index] - close[index - 1];
        let (gain, loss) = if change >= 0.0 {
            (change, 0.0)
        } else {
            (0.0, -change)
        };
        avg_gain = (avg_gain * (period as f64 - 1.0) + gain) / period as f64;
        avg_loss = (avg_loss * (period as f64 - 1.0) + loss) / period as f64;
        out[index] = if avg_loss == 0.0 {
            if avg_gain == 0.0 { 50.0 } else { 100.0 }
        } else {
            100.0 - 100.0 / (1.0 + avg_gain / avg_loss)
        };
    }
    if ctx.write_output(0, &out) { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
