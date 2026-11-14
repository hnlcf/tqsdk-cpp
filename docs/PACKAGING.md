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

# 默认 deb+rpm+tgz+container（本地有 podman 或 docker 时）
./scripts/package_release.sh

# 只生成指定格式
./scripts/package_release.sh --formats deb
./scripts/package_release.sh --formats rpm
./scripts/package_release.sh --formats deb,rpm,tgz
./scripts/package_release.sh --formats deb,rpm,tgz,container
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
  tqsdk_0.1.0_amd64.deb                 # gateway/node/web runtime
  tqsdk-libs_0.1.0_amd64.deb            # shared libraries
  tqsdk-dev_0.1.0_amd64.deb             # headers + CMake exports
  tqsdk-0.1.0-1.x86_64.rpm
  tqsdk-libs-0.1.0-1.x86_64.rpm
  tqsdk-devel-0.1.0-1.x86_64.rpm
  tqsdk-0.1.0-linux-x86_64.tar.gz       # runnable runtime tree
  tqsdk-libs-0.1.0-linux-x86_64.tar.gz
  tqsdk-dev-0.1.0-linux-x86_64.tar.gz
  tqsdk-image-0.1.0-x86_64.tar.gz       # podman/docker image archive
  tqsdk_py-0.1.0-py3-none-any.whl       # uv-built Python package (if uv exists)
  tqsdk-0.1.0.files.txt
  SHA256SUMS
  stage/tqsdk-0.1.0/
```

指定版本：

```bash
./scripts/package_release.sh --version 0.4.0
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
/usr/lib/libtqsdk_core.so*              # tqsdk-libs
/usr/lib/libtqsdk_ctp.so*               # tqsdk-libs，仅 gw 使用的 CTP 网关
/usr/lib/libtqsdk_backtest.so*          # tqsdk-libs
/usr/lib/libthost*.so*                  # tqsdk-libs，CTP runtime
/usr/include/tqsdk/                     # tqsdk-dev / tqsdk-devel
/usr/lib/cmake/tqsdk/                   # tqsdk-dev / tqsdk-devel
/usr/lib/systemd/system/tqsdk-gw.service
/usr/lib/systemd/system/tqsdk-node.service
/usr/lib/systemd/system/tqsdk.target
/usr/share/tqsdk/web/                  # SolidJS 静态站点 + WASM 插件
/usr/share/tqsdk/examples/caddy/
/usr/share/tqsdk/examples/nginx/
/usr/share/doc/tqsdk/
/etc/tqsdk/gw.toml.example
/etc/tqsdk/node.toml.example
/etc/tqsdk/ipc/*.toml
/etc/tqsdk/shared/*.toml
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
sudo dpkg -i dist/tqsdk-libs_0.1.0_amd64.deb \
             dist/tqsdk_0.1.0_amd64.deb
# 开发时需要：
sudo dpkg -i dist/tqsdk-dev_0.1.0_amd64.deb
```

Fedora/RHEL：

```bash
sudo rpm -Uvh dist/tqsdk-libs-0.1.0-1.x86_64.rpm \
             dist/tqsdk-0.1.0-1.x86_64.rpm
# 开发时需要：
sudo rpm -Uvh dist/tqsdk-devel-0.1.0-1.x86_64.rpm
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
# Debian（三个包）
for f in dist/tqsdk_0.1.0_amd64.deb dist/tqsdk-libs_0.1.0_amd64.deb dist/tqsdk-dev_0.1.0_amd64.deb; do
  ar p "$f" control.tar.gz | tar -xzO ./control
done

# RPM（三个包）
for f in dist/tqsdk-0.1.0-1.x86_64.rpm dist/tqsdk-libs-0.1.0-1.x86_64.rpm dist/tqsdk-devel-0.1.0-1.x86_64.rpm; do
  rpm -qpR "$f"
  rpm -qpl "$f" | head
done

# 二进制 RPATH（应包含 $ORIGIN/../lib）
readelf -d dist/stage/tqsdk-0.1.0/usr/bin/tqsdk-node | grep RUNPATH

# 包内 systemd 语法（安装前可做静态替换检查）
./scripts/verify_deploy.sh
```

## 6. 发布

生成校验文件后，把 `dist/` 作为 release artifact：

```bash
(cd dist && sha256sum -c SHA256SUMS)
gh release create v0.1.0 \
  dist/tqsdk_0.1.0_amd64.deb dist/tqsdk-libs_0.1.0_amd64.deb dist/tqsdk-dev_0.1.0_amd64.deb \
  dist/tqsdk-0.1.0-1.x86_64.rpm dist/tqsdk-libs-0.1.0-1.x86_64.rpm dist/tqsdk-devel-0.1.0-1.x86_64.rpm \
  dist/tqsdk-0.1.0-linux-x86_64.tar.gz dist/tqsdk-libs-0.1.0-linux-x86_64.tar.gz \
  dist/tqsdk-dev-0.1.0-linux-x86_64.tar.gz \
  dist/tqsdk-image-0.1.0-x86_64.tar.gz \
  dist/tqsdk_py-0.1.0-py3-none-any.whl \
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
- runtime、libs、dev/devel 已拆分：runtime 不包含头文件/CMake；
  libs 只包含共享库和 CTP runtime；dev/devel 只包含头文件和 CMake 导出；
- RPM 的开发包命名为 `tqsdk-devel`，DEB/TGZ 命名为 `tqsdk-dev`。
- 本机 2026-11-11 已实际生成并验证：
  `dist/tqsdk_0.1.0_amd64.deb`、`dist/tqsdk-0.1.0-1.x86_64.rpm`、
  `dist/tqsdk-0.1.0-linux-x86_64.tar.gz`；DEB 通过 `dpkg-deb` 容器校验，
  RPM 通过 `rpm -qpi/-qpl` 校验，包内 `tqsdk-node --version` 可执行。
- `.deb`/`.rpm` 未签名；正式发布应在 CI 中使用 GPG 签名并发布到 apt/yum 仓库。
- RPM 的 `rpmbuild` 必须完整安装 `rpm-build` 脚本；仅复制一个 `rpmbuild`
  可执行文件可能在 `%install` 的 brp/check 阶段缺脚本。
- DEB 的 postinst 会创建系统用户，容器/无 systemd 环境安装后需手动启动。


## 9. Tag 触发的 GitHub Release Action

`.github/workflows/release.yml` 在推送 `v*` tag 时执行：

1. 安装 CMake/Ninja/clang/uv/pnpm/rpm；
2. vcpkg 构建 `tqsdk-gw`、`tqsdk-node`、`tqsdk_backtest`；
   仓库 `third_party/ctp/` 已内置 CTP SDK，因此 Action 构建
   `TQSDK_ENABLE_CTP=ON`，包含真实 CTP runtime；
3. 构建 Web；
4. 跑 Python tests；
5. 调用 `scripts/package_release.sh` 生成 DEB/RPM/TGZ、image archive、wheel；
6. 用 `softprops/action-gh-release` 直接上传到该 tag 的 Release。

发版：

```bash
git tag -a v0.4.0 -m "TQSDK 0.4.0"
git push origin v0.4.0
```

不需要本机构建后再 `gh release upload`。如果仓库没有 `third_party/ctp`（例如 SDK 未随仓库分发），
才需要改为 `TQSDK_ENABLE_CTP=OFF` 或使用自托管 runner。
