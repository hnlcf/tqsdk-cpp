use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let period = ctx.param_usize(0, 20).clamp(1, 500);
    let high = ctx.high();
    let low = ctx.low();
    let mut upper = vec![f64::NAN; high.len()];
    let mut lower = vec![f64::NAN; low.len()];
    let mut middle = vec![f64::NAN; high.len()];
    for index in (period - 1)..high.len() {
        let start = index + 1 - period;
        let mut top = f64::NEG_INFINITY;
        let mut bottom = f64::INFINITY;
        for cursor in start..=index {
            top = top.max(high[cursor]);
            bottom = bottom.min(low[cursor]);
        }
        upper[index] = top;
        lower[index] = bottom;
        middle[index] = (top + bottom) / 2.0;
    }
    if !ctx.write_output(0, &upper)
        || !ctx.write_output(1, &lower)
        || !ctx.write_output(2, &middle)
    {
        return -1;
    }
    0
}

tqsdk_indicator_sdk::export_indicator!(indicator);
