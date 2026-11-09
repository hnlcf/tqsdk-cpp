# TqSdk 快期账户认证

## 1. 为什么需要

官方 TqSdk 在下载 `latest.json` / `symbol.json` 等 openmd 资源时，会在 HTTP
请求中携带快期账户换取的 access token：

```text
Authorization: Bearer <access_token>
```

不带认证时：

- 部分资源下载可能被限速；
- 下载可能超时，留下不完整 JSON；
- 后续名称服务/权限接口无法使用。

本项目原先只做 CTP 账户认证，没有快期账户认证。现在增加了 `TqAuth`。

## 2. 登录流程

实现对齐官方 `tqsdk/auth.py`：

```text
POST https://auth.shinnytech.com/auth/realms/shinnytech/protocol/openid-connect/token
Content-Type: application/x-www-form-urlencoded

grant_type=password
client_id=shinny_tq
client_secret=...
username=<快期账号>
password=<快期密码>
```

响应中的 `access_token` 用于 HTTP Bearer header，`refresh_token` 用于过期前刷新。
token 的 `exp` 从 JWT payload 读取，不校验签名；读取失败时使用 `expires_in`。

## 3. 配置

`config/node.toml`：

```toml
[tq_auth]
enabled = true
user_name = "your_tq_user"
password = "your_tq_password"
auth_url = "https://auth.shinnytech.com"
request_timeout_sec = 30
refresh_margin_sec = 30
ns_url = "https://api.shinnytech.com/ns"
broker_url_prefix = "https://files.shinnytech.com"
mode = "real"
```

推荐通过不会入库的 `config/live.local.env`：

```bash
TQ_AUTH_ENABLED=1
TQ_AUTH_USER=your_tq_user
TQ_AUTH_PASSWORD=your_tq_password
TQ_AUTH_URL=https://auth.shinnytech.com
```

`run_live.sh` 会把这些值写入 `run/live/node.toml`（`run/` 已被 `.gitignore` 忽略）。

## 4. 使用位置

`tqsdk-node` 启动时：

1. 读取 `[tq_auth]`；
2. 调用 `TqAuth::login()`；
3. 把 `TqAuth` 注入 `InstrumentCatalog`；
4. 后台下载合约目录时自动附加：
   - `Authorization: Bearer ...`；
   - `User-Agent: tqsdk-cpp/0.1`。

登录失败不会阻止行情/交易进程启动；目录刷新会继续尝试，并在日志中记录原因。

## 5. 与官方 auth.py 对齐的能力

`TqAuth` 已提供：

| 官方能力 | C++ |
|---|---|
| `login()` | `login()` |
| refresh_token | `refresh()` / 自动刷新 |
| JWT `sub` / `grants` | `auth_id()` / `grants_json()` |
| `expire_datetime` | `expire_datetime()` |
| `expire_days_left` / `account_type` | `expire_days_left()` / `product_type()` |
| `_add_account` | `add_account(account_id)` |
| `_get_td_url` | `get_td_url(broker_id, account_id)` |
| `_get_md_url` | `get_md_url(stock, backtest)` |
| `init(mode)` | `init("real"|"bt")` |
| `_base_headers` | `Authorization`、`User-Agent`、`Accept` |

扩展配置：

```toml
ns_url = "https://api.shinnytech.com/ns"
broker_url_prefix = "https://files.shinnytech.com"
mode = "real"
```

注意：当前已经实际接入的是 **登录 + token 刷新 + 合约目录 Bearer 下载**；
`get_td_url` / `get_md_url` / `add_account` 已实现为公共能力，后续接入 Tq
交易/行情网关时直接使用，不需要再复制一遍认证代码。

## 6. 安全

- 账号密码不得提交到仓库；
- `run/live/node.toml`、`config/live.local.env` 不进入版本控制；
- 日志不打印密码和 token；
- 当前 access token 只保存在 node 进程内存；
- 建议下一步将 token 缓存加密到本机，并支持只读 token 文件。

## 6. 验证

```bash
ctest --test-dir build-ui -R TqAuth --output-on-failure
```

覆盖：

- base64url JWT payload 解码；
- 未启用认证不生成 Bearer header；
- `[tq_auth]` TOML 解析。

真实登录需要有效的快期账号，不能使用 mock 代替。
