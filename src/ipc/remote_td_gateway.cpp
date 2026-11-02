#include "tqsdk/ipc/remote_td_gateway.h"

#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {
namespace ipc {

RemoteTdGateway::RemoteTdGateway(IpcClient&                client,
                                 asio::any_io_executor     ex,
                                 std::chrono::milliseconds ack_timeout)
    : client_(client),
      ex_(ex),
      ch_(ex),
      ack_timeout_(ack_timeout) {
    client_.set_td_handler([this](ReceivedFrame&& f) { on_frame(std::move(f)); });
}

asio::awaitable<void> RemoteTdGateway::start() {
    co_return;
}

asio::awaitable<void> RemoteTdGateway::stop() {
    ch_.close();
    co_return;
}

asio::awaitable<CommandAck> RemoteTdGateway::insert_order(const InsertOrderReq& req,
                                                          const OrderId&        rid) {
    CommandBody body;
    body.op     = CommandOp::InsertOrder;
    body.rid    = rid;
    body.insert = req;
    co_return co_await client_.send_command_and_wait(body, ack_timeout_);
}

asio::awaitable<CommandAck> RemoteTdGateway::cancel_order(const CancelOrderReq& req) {
    CommandBody body;
    body.op     = CommandOp::CancelOrder;
    body.cancel = req;
    co_return co_await client_.send_command_and_wait(body, ack_timeout_);
}

void RemoteTdGateway::push(TradeEvent ev) {
    co_spawn_logged(
        ex_,
        [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
            co_await ch_.async_send(std::move(e));
        },
        "RemoteTd.push");
}

void RemoteTdGateway::on_frame(ReceivedFrame&& frame) {
    switch (frame.type) {
    case FrameType::TradePush: {
        try {
            auto body = decode_as<FrameType::TradePush>(frame);
            TradeEvent ev;
            ev.kind    = TradeEvent::Kind::Diff;
            ev.payload = std::move(body.data);
            push(std::move(ev));
        } catch (const std::exception& e) {
            SPDLOG_WARN("RemoteTdGateway: bad TradePush: {}", e.what());
        }
        break;
    }
    case FrameType::CommandAck: {
        // 命令级即时回执：IpcClient 已完成 send_command_and_wait 的关联；
        // 这里额外把 Ack 作为 TradeEvent::Kind::Ack 推给上层（TqApi 可选消费）。
        try {
            auto body = decode_as<FrameType::CommandAck>(frame);
            TradeEvent ev;
            ev.kind = TradeEvent::Kind::Ack;
            ev.ack  = body.ack;
            push(std::move(ev));
        } catch (const std::exception& e) {
            SPDLOG_WARN("RemoteTdGateway: bad CommandAck: {}", e.what());
        }
        break;
    }
    case FrameType::GatewayEvent: {
        try {
            auto body = decode_as<FrameType::GatewayEvent>(frame);
            if (body.kind == GatewayEventKind::Connected) {
                return;
            }
            TradeEvent ev;
            if (body.kind == GatewayEventKind::Disconnected) {
                ev.kind = TradeEvent::Kind::Disconnected;
            } else {
                ev.kind      = TradeEvent::Kind::Error;
                ev.error_msg = body.text;
            }
            push(std::move(ev));
        } catch (const std::exception& e) {
            SPDLOG_WARN("RemoteTdGateway: bad GatewayEvent: {}", e.what());
        }
        break;
    }
    default:
        break;
    }
}

} // namespace ipc
} // namespace tqsdk
