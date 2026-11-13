#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h"
#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

/// 脚本化交易网关。
///
/// 行为：insert_order() 立即回 ack；随后异步推一条包含订单终态的 trade diff。
/// 选项可在注入前配置：
///   - auto_fill       : 是否在收到报单后自动"成交并终结"（默认 true）
///   - reply_delay_ms  : 终态 diff 的延迟（默认 0，用 timer 模拟真实回报延迟）
///   - reject_next     : 下一笔单是否被柜台拒绝（ack.success = false）
///
/// 生成的 diff 与天勤/CTP 网关同构，因此上层 TqApi 无需区分 mock 与真网关。
class MockTdGateway : public TradingGateway {
public:
    explicit MockTdGateway(asio::any_io_executor ex)
        : ex_(ex),
          ch_(ex) {}

    // ---- 脚本化开关 ----
    bool auto_fill      = true;
    int  reply_delay_ms = 0;
    bool reject_next    = false;

    asio::awaitable<void> start() override { co_return; }

    asio::awaitable<void> stop() override {
        ch_.close();
        co_return;
    }

    asio::awaitable<CommandAck> insert_order(const InsertOrderReq& req,
                                             const OrderId&        rid) override {
        if (reject_next) {
            reject_next = false;
            co_return CommandAck{.success = false, .order_id = rid, .error_msg = "mock: rejected"};
        }

        CommandAck ack{.success = true, .order_id = rid, .error_msg = {}};

        // 异步推终态：先回 ack，稍后（可选延迟）推 diff。
        if (auto_fill) {
            schedule_final_diff(rid, req);
        }
        co_return ack;
    }

    asio::awaitable<CommandAck> cancel_order(const CancelOrderReq& req) override {
        // 阶段 2 撤单先简单回 ack；真实状态推进走 diff。
        co_return CommandAck{.success = true, .order_id = req.order_id, .error_msg = {}};
    }

    Channel<TradeEvent>& event_output() override { return ch_; }

    // ---- 供测试直接注入任意 trade diff ----
    void inject_diff(nlohmann::json diff) {
        TradeEvent ev{TradeEvent::Kind::Diff, std::move(diff), {}, {}};
        co_spawn_logged(
            ex_,
            [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
                co_await ch_.async_send(std::move(e));
            },
            "MockTd.inject_diff");
    }

private:
    // 构造一条与天勤同构的"订单终态"diff。
    static nlohmann::json make_filled_diff(const OrderId& rid, const InsertOrderReq& req) {
        nlohmann::json order{{"order_id", rid},
                             {"instrument_id", req.instrument_id},
                             {"direction", magic_enum::enum_name(req.direction)},
                             {"offset", magic_enum::enum_name(req.offset)},
                             {"volume_orign", req.volume},
                             {"volume_left", 0},
                             {"volume_cond", magic_enum::enum_name(req.volume_cond)},
                             {"price_type", magic_enum::enum_name(req.price_type)},
                             {"limit_price", req.limit_price},
                             {"status", magic_enum::enum_name(OrderStatus::FINISHED)},
                             {"is_dead", true},
                             {"is_error", false},
                             {"last_msg", "全部成交"}};
        // 外层 key 用空的 ""，instrument_id 由 order 内部字段携带；
        // 同时若限价有效，登记一条成交 trade。
        nlohmann::json user{{"orders", {{rid, order}}}};

        if (std::isfinite(req.limit_price)) {
            nlohmann::json trade{{"order_id", rid},
                                 {"instrument_id", req.instrument_id},
                                 {"direction", magic_enum::enum_name(req.direction)},
                                 {"offset", magic_enum::enum_name(req.offset)},
                                 {"volume", req.volume},
                                 {"price", req.limit_price}};
            user["trades"] = {{fmt::format("{}_t1", rid), trade}};
        }

        return nlohmann::json{{"trade", {{"", {{"user", user}}}}}};
    }

    void schedule_final_diff(const OrderId& rid, const InsertOrderReq& req) {
        auto diff = make_filled_diff(rid, req);
        if (reply_delay_ms <= 0) {
            inject_diff(std::move(diff));
            return;
        }
        auto timer = std::make_shared<asio::steady_timer>(ex_);
        timer->expires_after(std::chrono::milliseconds(reply_delay_ms));
        auto* self = this;
        timer->async_wait([self, timer, d = std::move(diff)](boost::system::error_code ec) mutable {
            if (ec) {
                return;
            }
            self->inject_diff(std::move(d));
        });
    }

    asio::any_io_executor ex_;
    Channel<TradeEvent>   ch_;
};

} // namespace tqsdk
