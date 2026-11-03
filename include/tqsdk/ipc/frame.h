#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {
namespace ipc {

/// IPC 帧类型定义
///
/// | 编号   |	     类型	           |    方向	                        |   触发时机	           |   应答	           | 可靠性 |
/// |:------:|:-----------------------:|:----------------------------------:|:------------------------:|:-----------------:|:---------:|
/// | `0x01` |	    `Hello`            | 任意 client →  core	            | 连接建立后首帧	       | `HelloAck`	       | 必达，超时关闭 |
/// | `0x02` |	    `HelloAck`         | core →  client	                    | 校验握手	               | —	               | 必达 |
/// | `0x03` |	    `Subscribe`        | strategy →  core	                | 接入 / 重连 / gap	       | `SubscribeAck`	   | 必达 |
/// | `0x04` |	    `SubscribeAck`     | core →  strategy	                | 订阅受理	               | —	               | 必达 |
/// | `0x05` |	    `Snapshot`         | core →  strategy	                | 首次订阅 / resume 失败   | —	               | 必达 |
/// | `0x06` |	    `Diff`             | core →  strategy	                | 每次 merge 后	           | —	               | 行情可丢 / 交易必达 |
/// | `0x07` |	    `Command`          | strategy →  core; core →  gateway	| 用户下单/撤单/查询	   | `CommandAck`	   | 必达（应用层重传） |
/// | `0x08` |	    `CommandAck`       | gateway →  core; core →  strategy	| 命令处理结果	           | —	               | 必达 |
/// | `0x09` |	    `GatewayEvent`     | gateway →  core; core →  strategy	| 连接状态/错误	           | —	               | 必达 |
/// | `0x0A` |	    `MarketDataPush`   | gateway →  core	                | 柜台行情到达	           | —	               | 可丢 |
/// | `0x0B` |	    `TradePush`        | gateway →  core	                | 订单/成交/账户变化	   | —	               | 必达 |
/// | `0x0C` |	    `QueryOrders`      | strategy →  core →  gateway	    | 命令 ack 超时兜底	       | `QueryOrdersAck`  | 必达 |
/// | `0x0D` |	    `QueryOrdersAck`   | gateway →  core →  strategy	    | 查询结果	               | —	               | 必达 |
/// | `0x0E` |	    `Heartbeat`        | 双向	                            | 每 30s 空闲	           | `HeartbeatAck`	   | 可丢 |
/// | `0x0F` |	    `HeartbeatAck`     | 双向	                            | 收到心跳	               | —	               | 可丢 |
/// | `0x10` |	    `Goodbye`          | 双向	                            | 优雅关闭	               | —	               | 尽力 |
/// | `0x11` | `Resume`        | node → gateway | 重连后恢复 seq | `ResumeAck` | 必达 |
/// | `0x12` | `ResumeAck`     | gateway → node | 恢复策略（replay/snapshot） | — | 必达 |
/// | `0x13` | `TradeSnapshot` | gateway → node | 交易状态对账 | — | 必达 |
enum class FrameType : uint8_t {
    Hello          = 0x01,
    HelloAck       = 0x02,
    Subscribe      = 0x03,
    SubscribeAck   = 0x04,
    Snapshot       = 0x05,
    Diff           = 0x06,
    Command        = 0x07,
    CommandAck     = 0x08,
    GatewayEvent   = 0x09,
    MarketDataPush = 0x0A,
    TradePush      = 0x0B,
    QueryOrders    = 0x0C,
    QueryOrdersAck = 0x0D,
    Heartbeat      = 0x0E,
    HeartbeatAck   = 0x0F,
    Goodbye        = 0x10,
    Resume         = 0x11,
    ResumeAck      = 0x12,
    TradeSnapshot  = 0x13,
};

enum class FrameEncoding : uint8_t {
    Json        = 0x00,
    MessagePack = 0x01
};

/// IPC 帧结构体，所有 IPC 消息都封装在 Frame 中传输
///
/// `[ length: u32 BE ][ type: u8 ][ encoding: u8 ][ payload: length-2 bytes ]`
struct Frame {
    uint32_t      length;    // 帧长度
    FrameType     type;      // 帧类型
    FrameEncoding encoding;  // 帧编码方式
    uint8_t       payload[]; // 帧负载
};

/// 接收侧结果：与 Frame 一一对应，但 payload 用 vector 承载以便 move
struct ReceivedFrame {
    FrameType            type;
    FrameEncoding        encoding;
    std::vector<uint8_t> payload;
};

using ConstSlice   = std::span<const uint8_t>;
using MutableSlice = std::span<uint8_t>;

} // namespace ipc
} // namespace tqsdk
