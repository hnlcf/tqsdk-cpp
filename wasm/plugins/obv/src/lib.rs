use tqsdk_indicator_sdk::Context;

fn indicator(ctx: &mut Context<'_>) -> i32 {
    let close = ctx.close();
    let volume = ctx.volume();
    let mut out = vec![0.0; close.len()];
    if close.len() > 1 {
        for index in 1..close.len() {
            let delta = if close[index] > close[index - 1] {
                volume[index]
            } else if close[index] < close[index - 1] {
                -volume[index]
            } else {
                0.0
            };
            out[index] = out[index - 1] + delta;
        }
    }
    if ctx.write_output(0, &out) { 0 } else { -1 }
}

tqsdk_indicator_sdk::export_indicator!(indicator);
