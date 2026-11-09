# 期权 ATM 与 Greeks

## 1. 模型

实现欧式 Black-Scholes：

```cpp
OptionSpec spec;
spec.spot = 3000;
spec.strike = 3000;
spec.time_to_expiry = 30.0 / 365.0;
spec.risk_free_rate = 0.02;
spec.volatility = 0.2;
spec.right = OptionRight::CALL;

auto valuation = black_scholes(spec);
```

返回：

- 理论价、内在价值、时间价值；
- Delta / Gamma / Vega / Theta / Rho；
- Vega 是波动率变化 1.0 的价格变化，乘 0.01 即一个波动率点；
- Theta 是每年衰减，按交易日换算时可再除以 365。

## 2. 隐含波动率与 ATM

```cpp
implied_volatility(market_price, spec);
atm_strike(spot, strikes);
compute_option_chain(spot, strikes, T, r, q, vol,
                     market_prices, underlying, /*atm_window=*/5);
```

- IV 使用有界二分，价格越界返回空；
- ATM 选择离现货绝对距离最近的执行价；
- chain 只返回 ATM 附近窗口，按 strike 排序，每个执行价同时给出 CALL/PUT。

## 3. REST

```text
GET /option/analytics/{underlying}?spot=3000&rate=0.02&dividend=0&vol=0.2&window=5
```

字段：

- `atm_strike`、`spot`、`time_to_expiry`；
- `rows[].instrument_id/right/strike/price/intrinsic/time_value`；
- `rows[].market_price`、`rows[].implied_volatility`；
- `rows[].greeks`。

`spot` 缺省时尝试读取标的 quote；`expire_datetime` 来自合约目录。

## 4. Web

期权页现在：

- 平值行权价和高亮行由后端 `atm_strike` 驱动，不再硬编码 3000；
- 平值隐波显示 ATM 行；
- 选中合约后显示 Delta/Gamma/Vega/Theta/Rho/IV；
- 理论值窗口默认 ATM ±6 档。

## 5. 假设与边界

- 欧式、连续无套利、无跳空、无波动率微笑；
- 不建模期货保证金/期权保证金和行权交割；
- 无风险利率、股息率、波动率参数由调用方提供，默认值不是市场推荐；
- 期权实际交易价格可能偏离理论价；Greeks 用于风险观察，不构成投资建议。
