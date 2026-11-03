# 多线程运行时与 Strand 模型

本文描述 node/gw 的线程模型、executor 边界、配置方式，以及为什么当前
阶段仍然把数据面收敛到一条 core strand。

## 1. 运行参数

配置：

```toml
[runtime]
threads = 4
```

命令行可覆盖：

```bash
./build/tqsdk-node -c config/node.toml --threads 4
./build/tqsdk-gw   -c config/gw.toml   --threads 2
```

`threads` 会经过 `normalize_worker_threads()` 收敛到 `[1, 64]`，然后由
`run_io_context()` 启动 N-1 个后台线程 + 主线程共同 `io.run()`。

## 2. node 的执行器布局

```text
                    io_context (N worker threads)
                    /                           \
        io_ex (accept + 每连接 strand)          core_ex (strand)
        - acceptor                              - TqApi
        - tcp::socket 读/写                     - DataManager / DiffMergeEngine
        - HTTP 解析/序列化                       - RemoteMdGateway / RemoteTdGateway
        - WebSocket 握手前的 HTTP 分支           - IpcSession / IpcClient
                                                - WebSocketHub 全部状态
                                                - HTTP 路由 handler
```

### 2.1 HTTP 请求路径

1. `accept_loop` 在 `io_ex` 上 accept；
2. 每条连接创建一条 `asio::make_strand(io_ex)`，session 协程绑定该 strand；
3. session 在自己的 strand 上 `http::async_read`；
4. 进入路由前 `dispatch(core_ex)`，在 core strand 上执行 handler
   （读 DataManager、`insert_order`/`cancel_order` 等）；
5. handler 返回后 `dispatch(sock.get_executor())` 回到连接 strand，
   完成 `http::async_write`，再继续下一轮 read。

因此：

- 不同 HTTP 连接可以真正并行读/写；
- 同一连接内部天然串行，不需要额外锁；
- 所有 DataManager 访问仍然严格串行，现有单线程数据模型不需要加锁。

### 2.2 WebSocket 路径

WebSocket 升级请求在 session 中先 `dispatch(core_ex)`，再交给
`WebSocketHub::accept()`。Hub 的 `clients_` / 引用计数 / 订阅表 / 写队列
全部只在 core strand 上访问；`Hub::push_loop` 也是 core 上唯一调用
`TqApi::wait_update()` 的协程。WebSocket 的 reader/writer 协程同样在
core strand 上运行，因此当前阶段 WS 数据面是串行的。

### 2.3 网关与 IPC

`make_tqapi(io, cfg, core_ex)` 把 `core_ex` 传给：

- `TqApi` 的 md_loop / td_loop / update_signal；
- `TqMdGateway` / `SimTdGateway` / `RemoteMdGateway` / `RemoteTdGateway`；
- `IpcSession` / `IpcClient` / `GatewayIpcServer`。

所以行情、交易、合并、IPC 读写都在同一条 core strand 上，跨组件调用不需要
额外同步。HTTP 的并行 I/O 与 core 的串行状态更新在 `session()` 的
`dispatch` 边界处交接。

## 3. gw 的执行器布局

gw 当前使用**一条进程级 core strand** 承载：

- 真实网关（TqMdGateway / SimTdGateway）；
- 每个 `GatewayIpcServer`、其 md/td pump、控制循环；
- 每个连接的 `IpcSession` 后台读/心跳/看门狗协程。

worker 线程数 > 1 时，socket I/O 可以由多个线程推进，但所有业务状态更新
仍被 strand 串行化。

多 node 共享网关的“单消费者 Channel”问题已由 `ipc/event_bus.h` 解决：
EventBus 是唯一消费者，把事件非阻塞复制到每个 `GatewayIpcServer` 的有界 sink。
但 sink 的 `try_send()` 仍要求与 bus 同 executor；要给每个连接独立 strand，
还需要把 sink 入队改成“post 到连接 strand”或线程安全队列。因此 gw 当前仍把
总线、server、session 收敛到一条 core strand，先把正确性固定下来。

## 4. 生命周期与关闭顺序

- `run_io_context()` 在所有 worker 线程 join 后才返回；
- node 收到信号后 `io.stop()`，主线程随后：
  1. `http->stop()`（关闭 acceptor / WebSocketHub）；
  2. `api->close()`（关闭网关与 IPC client）；
  3. `io.restart(); io.run_for(200ms)` 排空 close 触发的回调；
- gw 关闭 acceptor、所有活动 server，再让 io_context 自然排空；
- `IpcSession::start()` 握手超时时会先 `close()`，等待
  read/heartbeat/watchdog 三个协程全部退出后才抛异常，避免调用方立即析构
  造成 UAF；
- `IpcClient` 断线后把旧 session 移入 `retired_sessions_`，等后台协程收尾
  后再随 client 析构。

## 5. 约束与注意事项

1. **core strand 上的对象不要在其他 executor 上直接改**。新增 serve 路由时，
   如果需要访问 `TqApi/DataManager`，应保持在 `HttpServer::session()` 的
   dispatch 区间内。
2. **`Channel<T>` 仍是单消费者、executor-affine**。不要在 core strand 之外
   直接 `async_receive()`；多消费者 fan-out 需要单独设计。
3. **`TqApi::wait_update()` 只支持一个 waiter**。当前由 `WebSocketHub::push_loop`
   独占；不要在其他组件里再起第二个等待者。
4. **timer cancel 要在其 owner executor 上执行**。现有组件都绑定 core/home
   strand，跨线程关闭时通过 `io.stop()` + 生命周期收尾，不要从任意线程直接
   `cancel()` 别的 strand 的 timer。
5. **HTTP handler 内避免长时间同步阻塞**。core strand 被阻塞会拖住行情合并
   和 WebSocket 推送；重 CPU 工作应下沉到线程池或拆成异步分片。

## 6. 测试

- `tests/serve/threading_test.cpp`：
  - 4 个 worker 线程；
  - 32 个并发 HTTP 客户端（`/healthz` 与 `/account` 混合）；
  - 校验全部 200、无失败。
- `tests/serve/ws_stream_test.cpp`、`tests/serve/ws_protocol_test.cpp`：
  WebSocket 协议与 hub 行为。
- `tests/ipc/gateway_server_test.cpp`：
  TCP 重连、Resume、epoch 变化快照、md gap。
- TSan 构建：

```bash
cmake -S . -B build-tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=/home/hnlcf/dev/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg_installed" -DVCPKG_MANIFEST_INSTALL=OFF \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -fuse-ld=mold" \
  -DTQSDK_BUILD_TESTS=ON -DTQSDK_ENABLE_CTP=OFF
cmake --build build-tsan --target tqsdk_tests -j
TSAN_OPTIONS=halt_on_error=1 ./build-tsan/tqsdk_tests \
  --gtest_filter='Threading.*:WsStream.*:GatewayIpcServerTest.*:IpcClientTest.*'
```

## 7. 后续演进

当前是“多线程传输 + 单 core 数据面”的安全折中。下一步可做：

1. 在 EventBus 基础上，让每条 gw 连接拥有独立 strand：把 sink 入队改成
   post 到连接 strand 的无阻塞投递；
2. 把 `Channel<T>` 升级为跨 executor、多消费者安全实现（或改用
   `asio::experimental::channel`）；
3. 将序列化/大 JSON 构造下沉到 thread_pool，core 只做指针交换；
4. 为 WebSocket 每客户端分配独立 strand（需要先解决 Hub 状态的跨 strand 访问）；
5. 增加 perf baseline、长时间 soak、队列水位与线程亲和度指标。
