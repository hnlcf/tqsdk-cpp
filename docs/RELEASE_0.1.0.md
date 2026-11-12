# TQSDK 0.1.0 Release

## 组件

| 文件 | 内容 |
| --- | --- |
| `tqsdk_0.1.0_amd64.deb` | gateway/node、Web、systemd、配置、文档 |
| `tqsdk-libs_0.1.0_amd64.deb` | `libtqsdk_core`、`libtqsdk_backtest`、CTP runtime |
| `tqsdk-dev_0.1.0_amd64.deb` | C++ 头文件、CTP 头文件、CMake exports |
| `tqsdk-0.1.0-1.x86_64.rpm` | RPM runtime |
| `tqsdk-libs-0.1.0-1.x86_64.rpm` | RPM shared libraries |
| `tqsdk-devel-0.1.0-1.x86_64.rpm` | RPM development files |
| `tqsdk-0.1.0-linux-x86_64.tar.gz` | 可直接运行的 runtime tree |
| `tqsdk-libs-0.1.0-linux-x86_64.tar.gz` | 库文件 |
| `tqsdk-dev-0.1.0-linux-x86_64.tar.gz` | 开发文件 |
| `tqsdk-image-0.1.0-x86_64.tar.gz` | podman/docker docker-archive，gzip |
| `tqsdk_py-0.1.0-py3-none-any.whl` | Python `tqsdk_py` package |
| `SHA256SUMS` | 所有 artifact 校验 |

## 安装

```bash
# Debian/Ubuntu
sudo dpkg -i tqsdk-libs_0.1.0_amd64.deb tqsdk_0.1.0_amd64.deb
sudo dpkg -i tqsdk-dev_0.1.0_amd64.deb   # 仅开发需要

# Fedora/RHEL
sudo rpm -Uvh tqsdk-libs-0.1.0-1.x86_64.rpm tqsdk-0.1.0-1.x86_64.rpm
sudo rpm -Uvh tqsdk-devel-0.1.0-1.x86_64.rpm  # 仅开发需要
```

## 容器镜像

```bash
podman load -i tqsdk-image-0.1.0-x86_64.tar.gz
# 或
docker load -i tqsdk-image-0.1.0-x86_64.tar.gz

podman run -d --name tqsdk -p 8080:8080 tqsdk:0.1.0
curl -fsS http://127.0.0.1:8080/healthz
```

## Python

```bash
uv pip install tqsdk_py-0.1.0-py3-none-any.whl
uv run tqsdk-strategies
```

`BacktestRunner` 需要 native `libtqsdk_backtest.so`；通过 DEB/RPM 安装后通常
自动可找到，或设置：

```bash
export TQSDK_BACKTEST_LIB=/usr/lib/libtqsdk_backtest.so
```

## 本次修复

- CI 在 clang 18 + libstdc++ 14 下对 `std::visit` + nlohmann JSON 隐式转换
  出现模板异常规范递归；`ws_protocol.h` 改为显式 `to_json` 和显式 klines
  数组构造，CI 已恢复通过。
- `EmaDonchianStrategy.on_bar()` 改为增量 EMA/ATR/Donchian；保留 `Bar`
  Sequence 接口，不引入 numpy/pandas 硬依赖。

## 校验

```bash
sha256sum -c SHA256SUMS
```
