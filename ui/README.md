# TQSDK Qt UI

独立的 Qt 6 Widgets 桌面前端。它只通过 node 的 REST + WebSocket 获取行情/
交易数据，**不链接 `tqsdk_core`**。

完整设计见 [`../docs/UI_QT.md`](../docs/UI_QT.md)。

## 依赖

- Qt 6.5+
- Qt6 Core / Gui / Widgets / Network / WebSockets
- Linux 下通常还需要 X11/Wayland、字体和 OpenGL 运行库

vcpkg 至少需要：

```json
"qtbase",
"qtwebsockets"
```

如果 `"qt"` metaport 已包含所需模块，可以只使用 metaport；配置阶段会明确
报告缺少的 Qt 组件。

## 构建

UI 默认关闭，避免影响后端核心构建。先安装 Qt，再用 UI preset：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset ui-dev
cmake --build --preset ui-dev --target tqsdk-ui
./build-ui/bin/tqsdk-ui
```

或者手动：

```bash
cmake -S . -B build-ui -DTQSDK_BUILD_UI=ON
cmake --build build-ui --target tqsdk-ui -j
```

## 当前阶段

Phase 0.5：

- 窗口壳和布局占位；
- `NodeClient` 传输无关接口；
- `HttpWsNodeClient`：HTTP 快照/下单 + WebSocket 订阅/diff；
- 页面尚未绑定数据，K 线自绘、期货列表、期权链和交易面板在 Phase 1/2 接入。

启动时指定 node：

```bash
./build-ui/bin/tqsdk-ui --node-url http://127.0.0.1:18081
```
