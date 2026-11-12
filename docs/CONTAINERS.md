# Docker / Podman 部署镜像

镜像包含：

- `tqsdk-gw` / `tqsdk-node`；
- `libtqsdk_core.so` 和捆绑的 CTP runtime；
- 已构建的 SolidJS/WASM Web 终端；
- nginx（静态站点 + `/api` 反向代理）；
- 容器入口脚本和 Sim/实盘示例配置。

## 1. 先构建 release staging

镜像使用 `scripts/package_release.sh` 的 staging：

```bash
./scripts/package_release.sh --formats tgz
```

会生成：

```text
dist/stage/tqsdk-0.1.0/
  usr/bin/tqsdk-gw
  usr/bin/tqsdk-node
  usr/lib/libtqsdk_core.so*
  usr/share/tqsdk/web/
```

## 2. 构建镜像

Podman：

```bash
./scripts/build_container.sh --engine podman
```

Docker：

```bash
./scripts/build_container.sh --engine docker
```

脚本内部执行：

```text
podman/build -f deploy/container/Dockerfile
  --build-arg BASE_IMAGE=registry.fedoraproject.org/fedora:44
  --build-arg RELEASE_DIR=dist/stage/tqsdk-0.1.0
  -t tqsdk:0.1.0 .
```

Podman 使用 `--format docker`，以保留 `HEALTHCHECK` 元数据。

生成可分发镜像归档：

```bash
./scripts/build_container.sh --engine podman \
  --archive dist/tqsdk-image-0.1.0-x86_64.tar.gz
```

归档为 gzip 压缩的 docker-archive，可用：

```bash
podman load -i dist/tqsdk-image-0.1.0-x86_64.tar.gz
# 或
docker load -i dist/tqsdk-image-0.1.0-x86_64.tar.gz
```

`package_release.sh --formats deb,rpm,tgz,container` 会自动生成同名镜像 tgz。

## 3. 运行 Sim 演示

默认 `TQSDK_ROLE=node`，node 自带 SimTd，不需要 gw/CTP/外网：

```bash
podman run -d --name tqsdk-demo \
  -p 8080:8080 \
  tqsdk:0.1.0
```

打开：

```text
http://127.0.0.1:8080/
```

健康检查：

```bash
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/api/accounts
```

Sim 配置没有行情源，页面能加载但行情为空；这是预期行为，不是前端错误。

## 4. 运行实盘行情演示

镜像内带 `node-live.toml`：行情接 Tq MD，交易用 Sim。

```bash
podman run -d --name tqsdk-live \
  -p 8080:8080 \
  -e TQSDK_NODE_CONFIG=/etc/tqsdk/node-live.toml \
  tqsdk:0.1.0
```

当前环境已验证 `/api/quote/SHFE.rb2701` 返回实时买一/卖一，页面显示行情和盘口。
注意该配置连接公开 Tq MD，不包含真实 CTP 交易账号。

## 5. 生产运行

把真实配置以只读方式挂载：

```bash
podman run -d --name tqsdk-prod \
  -p 127.0.0.1:8080:8080 \
  -e TQSDK_ROLE=node \
  -e TQSDK_NODE_CONFIG=/etc/tqsdk/node.toml \
  -e TQSDK_SERVE_TOKEN="$(cat /etc/tqsdk/tqsdk.env | sed -n 's/^TQSDK_SERVE_TOKEN=//p')" \
  -v /etc/tqsdk/node.toml:/etc/tqsdk/node.toml:ro,Z \
  -v /var/lib/tqsdk:/var/lib/tqsdk:Z \
  -v /var/log/tqsdk:/var/log/tqsdk:Z \
  tqsdk:0.1.0
```

`TQSDK_ROLE`：

```text
node  启动 node + nginx（默认）
gw    只启动 gw
all   启动 gw + node + nginx；配置文件需自行组织
```

生产建议：

- 容器只绑定 `127.0.0.1`；
- TLS 由 Caddy/nginx 终止；
- token 从环境/secret 注入，不写镜像；
- 容器内 nginx 的 `/api` 反代不需要 `VITE_API_TOKEN` 编译；使用浏览器登录/反向代理认证更安全；
- 不把真实 CTP 密码写入镜像层。

## 6. Compose

`deploy/container/compose.yaml` 提供最小示例：

```bash
cd deploy/container
podman-compose up -d
# 或 docker compose up -d
```

Web：

```text
http://127.0.0.1:8080/
```

## 7. 发布

```bash
podman tag tqsdk:0.1.0 registry.example.com/tqsdk:0.1.0
podman push registry.example.com/tqsdk:0.1.0
```

发布前建议：

- `podman image inspect` 检查镜像架构和标签；
- 用 `podman run --rm ... --version` 验证二进制；
- 生成镜像 digest 并记录到 release；
- 镜像签名使用 cosign/sigstore 或 registry 原生签名。
