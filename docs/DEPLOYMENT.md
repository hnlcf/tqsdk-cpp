# 生产化部署：TLS、认证、systemd、审计

本文件描述目前代码可交付的生产化边界。节点/网关仍建议由同一台受控主机运行；
对外只暴露 TLS 反向代理，node 绑定 loopback。

## 1. 目录与用户

```bash
sudo useradd --system --home /var/lib/tqsdk --shell /usr/sbin/nologin tqsdk
sudo install -d -o tqsdk -g tqsdk -m 0750 /opt/tqsdk /etc/tqsdk /var/lib/tqsdk /var/log/tqsdk
sudo install -d -o tqsdk -g tqsdk -m 0750 /opt/tqsdk/bin /opt/tqsdk/docs
```

安装二进制、配置和文档：

```text
/opt/tqsdk/bin/tqsdk-gw
/opt/tqsdk/bin/tqsdk-node
/etc/tqsdk/gw.toml
/etc/tqsdk/node.toml
/etc/tqsdk/tqsdk.env       # 0600
/var/lib/tqsdk/            # 状态/缓存
/var/log/tqsdk/            # 日志/审计
```

## 2. 认证

### 2.1 node 配置

`/etc/tqsdk/node.toml`：

```toml
[serve]
bind = "127.0.0.1"
port = 18081

[serve.auth]
enabled = true
token_env = "TQSDK_SERVE_TOKEN"
allow_query_token = true
exempt_healthz = true
```

`/etc/tqsdk/tqsdk.env`（mode 0600）：

```bash
TQSDK_SERVE_TOKEN=<openssl rand -hex 32 的输出>
```

不要将 token 写进 TOML 或仓库。`enabled=true` 且 token 为空时 node 会拒绝启动。

### 2.2 REST 调用

```bash
curl -H "Authorization: Bearer $TQSDK_SERVE_TOKEN" \
  http://127.0.0.1:18081/accounts

curl -H "X-API-Key: $TQSDK_SERVE_TOKEN" \
  http://127.0.0.1:18081/quote/SHFE.rb2701

# /healthz 默认免认证，供本机/反向代理探活
curl http://127.0.0.1:18081/healthz
```

`/readyz` 和所有业务接口都受保护。浏览器 WebSocket 不能设置 Authorization 头，
因此客户端在 `allow_query_token=true` 时使用：

```text
wss://host/stream?account=A&token=<token>
```

query token 可能出现在反向代理访问日志中。生产环境应：

- 只允许 TLS；
- 反向代理关闭 query string 日志或对 `token` 脱敏；
- 更严格场景使用反向代理侧 OIDC/Basic Auth，并把 node 认证作为第二层；
- 定期轮换 token。

### 2.3 前端

Vite 构建时可设置：

```bash
VITE_API_TOKEN=<token> pnpm build
```

前端会为 REST 添加 `Authorization: Bearer`，并在 WS URL 加 `token`。这只适合
受控部署；公开站点应使用登录/OIDC 会话，不应把长期 token 编译进静态资源。

## 3. TLS

### 3.1 设计

TLS 在反向代理层终止，node 只监听 `127.0.0.1:18081`。这样：

- HTTP 与 WebSocket 共用普通上游连接；
- 证书/ACME 与 C++ 进程解耦；
- node 不直接暴露公网；
- 仍通过 `[serve.auth]` 做第二层认证。

### 3.2 Caddy（推荐）

```bash
sudo install -m 0644 deploy/caddy/Caddyfile /etc/caddy/Caddyfile
sudo systemctl edit caddy   # 设置 TQSDK_DOMAIN、TQSDK_ACME_EMAIL
sudo caddy validate --config /etc/caddy/Caddyfile
sudo systemctl reload caddy
```

Caddy 自动申请/续期证书并转发 WebSocket Upgrade。当前工作区未安装 caddy/nginx，
因此只做了配置静态检查（`./scripts/verify_deploy.sh`）；真实证书和浏览器 wss
握手仍需在部署主机验一次。

### 3.3 nginx

```bash
sudo install -m 0644 deploy/nginx/tqsdk.conf /etc/nginx/conf.d/tqsdk.conf
sudo nginx -t
sudo systemctl reload nginx
```

替换域名和证书路径；确保 `proxy_set_header Upgrade`/`Connection` 生效。

### 3.4 防火墙

```bash
# 只开放 80/443；18081 仅本机
sudo firewall-cmd --permanent --add-service=http
sudo firewall-cmd --permanent --add-service=https
sudo firewall-cmd --reload
```

## 4. systemd

安装单元：

```bash
sudo install -m 0644 deploy/systemd/tqsdk-gw.service /etc/systemd/system/
sudo install -m 0644 deploy/systemd/tqsdk-node.service /etc/systemd/system/
sudo install -m 0644 deploy/systemd/tqsdk.target /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now tqsdk.target
```

静态检查：

```bash
./scripts/verify_deploy.sh
```

如果使用 UDS：socket 必须放在 `/run/tqsdk`（`RuntimeDirectory=tqsdk`）而不是
`/tmp`，因为 unit 使用 `PrivateTmp=true`，两个服务会看到不同的 `/tmp`。

单元已包含：

- `Restart=on-failure`；
- `NoNewPrivileges`、`ProtectSystem=strict`、`ProtectHome`；
- `RuntimeDirectory`/`StateDirectory`/`LogsDirectory`；
- `LimitNOFILE=65536`；
- `ExecStart` 固定配置路径。

`systemd-analyze verify` 在未安装 `/opt/tqsdk/bin/*` 时会报可执行文件不存在；
这属于部署前预期提示，不是单元语法错误。安装二进制后可复验：

```bash
systemd-analyze verify /etc/systemd/system/tqsdk-gw.service \
  /etc/systemd/system/tqsdk-node.service /etc/systemd/system/tqsdk.target
```

## 5. 日志与审计

### 5.1 日志脱敏

`setup_logging()` 默认启用脱敏 formatter，覆盖：

- `password` / `passwd`；
- `token` / `access_token` / `refresh_token`；
- `client_secret`；
- `Authorization`；
- `Bearer ...`。

脱敏发生在所有 sink（stdout/文件）格式化之后，测试见
`tests/util/log_test.cpp::LogUtil.RedactsSecrets`。

注意：脱敏是兜底，不应主动打印原始 token/密码/请求体。

### 5.2 审计

`[audit]` 配置：

```toml
[audit]
enabled = true
file = "/var/log/tqsdk/audit.log"
max_bytes = 10485760
max_files = 5
```

记录内容为 JSONL：

```json
{"ts":1794383417730,"event":"http","method":"POST","target":"/order?account=A","status":200,"remote":"127.0.0.1:54321"}
```

覆盖：

- 非 GET 请求（下单/撤单/task/risk 配置）；
- 401 认证失败；
- WS 认证失败/升级。

不记录请求 body；target 中的 token 会脱敏。日志可喂给 Loki/ELK/filebeat；
不要只依赖 stdout journal，审计文件应单独保留和备份。

## 6. 验收清单

- [ ] `curl /healthz` 无 token 返回 200；
- [ ] `curl /accounts` 无 token 返回 401；
- [ ] Bearer / X-API-Key / WS query token 均可用；
- [ ] `wss://.../stream` 通过 TLS 升级成功；
- [ ] 反向代理不记录 token query（或已脱敏）；
- [ ] systemd 重启后 gw/node 恢复；
- [ ] audit.log 有 POST/401/WS 记录且无明文 token；
- [ ] 日志文件无 password/client_secret；
- [ ] 证书自动续期告警已配置。
