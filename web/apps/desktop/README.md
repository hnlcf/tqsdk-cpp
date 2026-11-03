# TQSDK Desktop (Tauri 2)

Tauri 2 桌面壳脚手架，Web UI 复用 `web/apps/web`。

## 前置

Linux 需要 WebKitGTK 4.1 开发包，例如 Fedora：

```bash
sudo dnf install webkit2gtk4.1-devel gtk3-devel libsoup3-devel
```

当前开发机未安装 `webkit2gtk4.1-devel`，因此本目录尚未在本机完成
`cargo build` 验证；脚手架本身已准备好。

## 运行

先在仓库根目录：

```fish
cd web
nvm use lts
npm install
```

然后启动桌面壳：

```fish
npm --workspace @tqsdk/desktop run dev
```

`beforeDevCommand` 通过 desktop 的 `web:dev` 脚本启动 `@tqsdk/web` 的 Vite dev server。

## 后端

当前桌面壳默认连接 `http://127.0.0.1:18081` 的 node。请先启动：

```bash
./tqsdk-node -c config/node.toml
```

下一步将 `tqsdk-node` 作为 Tauri sidecar 打包，并处理动态端口/本地 token。
