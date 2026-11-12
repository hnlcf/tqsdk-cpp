# Python 依赖、性能与 uv 打包

## 1. 是否要引入 numpy/pandas？

结论：**核心/live/策略接口不引入硬依赖**。

当前日线/4h 场景：

- 单品种研究历史约 2600 根日线；
- live 每次轮询约 200–2000 根；
- 信号在 closed bar 上计算，5 秒级轮询；
- `Bar` 列表的生命周期短，转换到 numpy/pandas 的收益很小。

真正需要关注的是**不要每根 bar 重新扫描全部历史**。现在
`EmaDonchianStrategy.on_bar()` 已使用增量 EMA/ATR/Donchian 状态，保留
`Bar` 列表接口，不需要 numpy/pandas。一次本地合成数据基准（2000 根）：

```text
decide()  约 3.3s
on_bar()  约 0.012s
```

如果后续做更大规模参数扫描，优先用 C++ `BacktestEngine`，最后才考虑
可选 NumPy 向量化研究脚本。

不建议把 pandas DataFrame 直接传入 `Strategy.on_bar()`：

- 策略插件会依赖 DataFrame dtype/index 细节；
- live 热路径需要额外转换；
- wheel 体积和部署依赖增加；
- `Bar`/`Signal` 是稳定、可跨回测/live 的公共接口。

因此 `pyproject.toml` 中：

```toml
dependencies = []          # live 核心零第三方依赖

[project.optional-dependencies]
research = ["numpy>=2.0", "pandas>=2.0"]
dev = ["pytest>=8", "ruff>=0.8"]
```

`PandasBarSource` 是可选适配器，只有传入/使用它时才 `import pandas`；
核心包导入不需要 pandas/numpy。

```python
from tqsdk_py import PandasBarSource

# 研究/数据准备阶段使用；返回的仍是通用 Bar 列表，策略接口不变。
bars = PandasBarSource("bars.csv").load("KQ.i@DCE.jm", 86_400_000_000_000)
```

`uv sync --extra research` 才会安装 numpy/pandas。

## 2. uv 环境

项目根目录：

```bash
cd /home/hnlcf/dev/tqsdk_cpp

# 最小 live/开发环境
uv sync --extra dev

# 研究/DataFrame 环境
uv sync --extra research --extra dev
```

运行：

```bash
uv run python -m unittest discover -s python/tests -v
uv run tqsdk-strategies
uv run tqsdk-live --list-strategies
```

`uv.lock` 已提交，锁定 Python 依赖版本。`.venv/` 不入库。

## 3. Python 包分层

```text
tqsdk-py / Python wheel
  ├── tqsdk_py/              主 API
  ├── tqsdk_backtest.py      ctypes 低层绑定（兼容入口）
  ├── tqsdk_strategy.py      旧导入名
  ├── tqsdk_live.py          旧 live CLI
  └── tqsdk_backtest_cli.py  通用回测 CLI

C++ / 系统包（CMake、DEB、RPM、TGZ）
  ├── tqsdk-backtest          libtqsdk_backtest.so
  ├── tqsdk-core              libtqsdk_core.so
  └── tqsdk-gw / tqsdk-node
```

Python wheel **默认不内嵌** `libtqsdk_backtest.so`：

- C++ 库依赖 Boost/OpenSSL/SQLite/CTP 等系统/第三方库；
- 把 `.so` 强行塞进 `py3-none-any` wheel 会破坏平台兼容性；
- DEB/RPM/TGZ 已有 C++ 库安装路径。

先构建 C++：

```bash
cmake --preset ui-dev
cmake --build --preset ui-dev --target tqsdk_backtest
```

Python 使用：

```bash
export TQSDK_BACKTEST_LIB="$PWD/build-ui/libtqsdk_backtest.so"
uv run tqsdk-backtest --strategy ema_donchian ...
```

安装 DEB/RPM 后，库在系统路径，通常不需要设置
`TQSDK_BACKTEST_LIB`。

## 4. 构建 wheel

```bash
uv build --wheel
```

输出：

```text
dist/tqsdk_py-0.1.0-py3-none-any.whl
```

安装：

```bash
uv pip install dist/tqsdk_py-0.1.0-py3-none-any.whl
```

如果希望**一个平台 wheel 同时带 native 回测库**，应单独做平台 wheel：

- 把 `libtqsdk_backtest.so` 和 `libtqsdk_core.so` 放入
  `python/tqsdk_py/lib/`；
- wheel tag 不能用 `py3-none-any`，必须按平台/ABI 构建；
- `find_library()` 增加 package-local `lib/` 搜索；
- 在 glibc 版本、CTP 动态库和 OpenSSL 版本上做 CI 验证。

当前项目默认采用“纯 Python wheel + 系统/CMake native 包”的方式更稳妥。

## 5. C++ 构建与 Python 安装的关系

```bash
# C++ 核心 + ctypes .so
cmake --preset ui-dev
cmake --build --preset ui-dev

# Python 环境
uv sync --extra dev

# 两者组合运行
TQSDK_BACKTEST_LIB=$PWD/build-ui/libtqsdk_backtest.so \
uv run tqsdk-backtest --strategy ema_donchian ...
```

CMake 的 `TQSDK_BUILD_PYTHON_BINDING=ON` 会安装：

```text
lib/libtqsdk_backtest.so
share/tqsdk/python/tqsdk_py/
share/tqsdk/python/tqsdk_*.py
share/tqsdk/config/daily_backtest_specs.json
```

## 6. 对外接口

推荐使用：

```python
import tqsdk_py
```

或：

```python
from tqsdk_py import default_manager, Strategy, register_strategy
```

兼容旧接口：

```python
import tqsdk_strategy
import tqsdk_live
import tqsdk_backtest
```

CLI：

```text
tqsdk-live
tqsdk-backtest
tqsdk-strategies
```

## 7. 建议的 CI 矩阵

```text
uv sync --extra dev
uv run python -m unittest discover -s python/tests

uv sync --extra research --extra dev
uv run python -m unittest discover -s python/tests   # PandasBarSource 可选测试

cmake --preset ui-dev
cmake --build --preset ui-dev
ctest --test-dir build-ui --output-on-failure
```

在 CI 的 Python 测试中显式设置：

```bash
TQSDK_BACKTEST_LIB=$PWD/build-ui/libtqsdk_backtest.so
```

没有 C++ 库时，`tqsdk_py` 的 live/策略接口仍然可以导入；只有实例化
`BacktestRunner`/调用 C++ 回测时才需要 native 库。
