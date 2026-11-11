# DEB / RPM 打包发布

本文描述如何把 C++ 后端、WASM 插件和 SolidJS 前端打成一个可安装的
`tqsdk` 包。包布局与 [DEPLOYMENT.md](DEPLOYMENT.md) 一致。

## 1. 构建依赖

Release 打包机需要：

```text
cmake >= 3.20
ninja
clang++
vcpkg（VCPKG_ROOT）
pnpm / Node.js >= 22
python3 + tar + ar
rpm-build（仅生成 RPM 时需要）
```

DEB 由仓库内的 `scripts/package_deb.py` 生成，不要求安装 `dpkg-deb`；
DEB 依赖 `passwd`（创建系统用户）和常见 libc/libstdc++；
RPM 需要 `rpmbuild`，也可以通过 `TQSDK_RPMBUILD` 指定路径。

## 2. 一键打包

```bash
export VCPKG_ROOT=/path/to/vcpkg

# 默认 deb+rpm+tgz
./scripts/package_release.sh

# 只生成指定格式
./scripts/package_release.sh --formats deb
./scripts/package_release.sh --formats rpm
./scripts/package_release.sh --formats deb,rpm,tgz
```

首次运行会：

1. 配置 `build-package/`（Release、CTP、无测试）；
2. 构建 `tqsdk-gw` / `tqsdk-node`；
3. 如果 `web/node_modules` 不存在则 `pnpm install --frozen-lockfile`；
4. 构建前端 `web/dist`；
5. `cmake --install --prefix /usr` 到 staging；
6. 注入 Web、systemd、配置模板、Caddy/nginx 示例和文档；
7. 生成 `.deb`、`.rpm`、`.tar.gz`。

输出：

```text
dist/
  tqsdk_0.1.0_amd64.deb
  tqsdk-0.1.0-1.x86_64.rpm
  tqsdk-0.1.0-linux-x86_64.tar.gz
  tqsdk-0.1.0.files.txt
  SHA256SUMS
  stage/tqsdk-0.1.0/
```

指定版本：

```bash
./scripts/package_release.sh --version 0.2.0
```

只复用已有 C++ 构建和 Web dist：

```bash
./scripts/package_release.sh --skip-build --skip-web
```

RPM 工具不在 PATH 时：

```bash
TQSDK_RPMBUILD=/path/to/rpmbuild ./scripts/package_release.sh --formats rpm
```

## 3. 包内容

```text
/usr/bin/tqsdk-gw
/usr/bin/tqsdk-node
/usr/lib/libtqsdk_core.so*
/usr/lib/libthost*.so*                 # CTP runtime，随包分发
/usr/lib/systemd/system/tqsdk-gw.service
/usr/lib/systemd/system/tqsdk-node.service
/usr/lib/systemd/system/tqsdk.target
/usr/share/tqsdk/web/                  # SolidJS 静态站点 + WASM 插件
/usr/share/tqsdk/examples/caddy/
/usr/share/tqsdk/examples/nginx/
/usr/share/doc/tqsdk/
/etc/tqsdk/gw.toml.example
/etc/tqsdk/node.toml.example
/etc/tqsdk/tqsdk.env.example
```

安装后 post-install 会创建 `tqsdk` 用户/组和状态/日志目录，并在**不覆盖**已有
配置的前提下把示例复制为：

```text
/etc/tqsdk/gw.toml
/etc/tqsdk/node.toml
/etc/tqsdk/tqsdk.env
```

`tqsdk.env` 为 0600，必须编辑 token；`gw.toml` / `node.toml` 为 root:tqsdk 0640。

## 4. 安装

Debian/Ubuntu：

```bash
sudo dpkg -i dist/tqsdk_0.1.0_amd64.deb
# 缺依赖时：
sudo apt-get -f install
```

Fedora/RHEL：

```bash
sudo rpm -Uvh dist/tqsdk-0.1.0-1.x86_64.rpm
```

安装后：

```bash
sudoedit /etc/tqsdk/tqsdk.env
sudoedit /etc/tqsdk/gw.toml
sudoedit /etc/tqsdk/node.toml
sudo systemctl enable --now tqsdk.target
systemctl status tqsdk-gw tqsdk-node
journalctl -u tqsdk-node -f
```

TLS/静态站点：按 [DEPLOYMENT.md](DEPLOYMENT.md) 安装
`/usr/share/tqsdk/examples/caddy/Caddyfile` 或 nginx 配置。node 只监听本机，
Caddy/nginx 负责 `/usr/share/tqsdk/web` 静态资源和 `/api` 反向代理。

## 5. 验证

```bash
# Debian
dpkg-deb -I dist/tqsdk_0.1.0_amd64.deb
dpkg-deb -c dist/tqsdk_0.1.0_amd64.deb | head

# RPM
rpm -qpi dist/tqsdk-0.1.0-1.x86_64.rpm
rpm -qpl dist/tqsdk-0.1.0-1.x86_64.rpm | head
rpm -qpR dist/tqsdk-0.1.0-1.x86_64.rpm

# 二进制 RPATH（应包含 $ORIGIN/../lib）
readelf -d dist/stage/tqsdk-0.1.0/usr/bin/tqsdk-node | grep RUNPATH

# 包内 systemd 语法（安装前可做静态替换检查）
./scripts/verify_deploy.sh
```

## 6. 发布

生成校验文件后，可把 `dist/` 作为 release artifact：

```bash
(cd dist && sha256sum -c SHA256SUMS)
gh release create v0.1.0 dist/tqsdk_0.1.0_amd64.deb \
  dist/tqsdk-0.1.0-1.x86_64.rpm dist/tqsdk-0.1.0-linux-x86_64.tar.gz \
  dist/SHA256SUMS --title "TQSDK 0.1.0" --notes "release"
```

正式 apt/yum 仓库还需要：

- 对包做 GPG 签名（`dpkg-sig`/`rpm --addsign`）；
- 生成 apt `Packages/Release` 或 yum `repodata`；
- HTTPS 仓库和客户端公钥安装；
- 保留每次 release 的 `SHA256SUMS` 和构建日志。

## 7. 容器镜像

DEB/RPM 之外还提供 Docker/Podman 镜像入口：

```bash
./scripts/build_container.sh --engine podman --smoke-test
```

详见 [CONTAINERS.md](CONTAINERS.md)。

## 8. 注意事项

- `VITE_API_TOKEN` 如果参与构建，会嵌入前端静态资源；仅限受控部署，生产优先
  使用反向代理认证/OIDC。
- 当前包同时携带 C++ 头文件和 CMake 导出文件，方便下游开发；如果发布严格
  runtime 产品，可再拆 `tqsdk-dev`。
- 本机 2026-11-11 已实际生成并验证：
  `dist/tqsdk_0.1.0_amd64.deb`、`dist/tqsdk-0.1.0-1.x86_64.rpm`、
  `dist/tqsdk-0.1.0-linux-x86_64.tar.gz`；DEB 通过 `dpkg-deb` 容器校验，
  RPM 通过 `rpm -qpi/-qpl` 校验，包内 `tqsdk-node --version` 可执行。
- `.deb`/`.rpm` 未签名；正式发布应在 CI 中使用 GPG 签名并发布到 apt/yum 仓库。
- RPM 的 `rpmbuild` 必须完整安装 `rpm-build` 脚本；仅复制一个 `rpmbuild`
  可执行文件可能在 `%install` 的 brp/check 阶段缺脚本。
- DEB 的 postinst 会创建系统用户，容器/无 systemd 环境安装后需手动启动。
