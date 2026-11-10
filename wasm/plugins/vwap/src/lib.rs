use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let high = ctx.high();
    let low = ctx.low();
    let close = ctx.close();
    let volume = ctx.volume();
    let mut out = vec![f64::NAN; close.len()];
    let mut traded_value = 0.0;
    let mut traded_volume = 0.0;
    for index in 0..close.len() {
        let typical = (high[index] + low[index] + close[index]) / 3.0;
        let bar_volume = volume[index].max(0.0);
        traded_value += typical * bar_volume;
        traded_volume += bar_volume;
        out[index] = if traded_volume > 0.0 {
            traded_value / traded_volume
        } else {
            close[index]
        };
    }
    if ctx.write_output(0, &out) { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
