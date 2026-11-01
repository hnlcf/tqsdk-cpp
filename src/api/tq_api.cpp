#include "tqsdk/tqapi.h"

#include "tqsdk/util/spawn_logged.h"

namespace tqsdk {

TqApi::TqApi(asio::any_io_executor              ex,
             std::unique_ptr<MarketDataGateway> md,
             std::unique_ptr<TradingGateway>    td)
    : ex_(ex),
      md_(std::move(md)),
      td_(std::move(td)),
      merger_(data_),
      update_signal_(ex_) {}

TqApi::~TqApi() {
    close();
}

void TqApi::start() {
    if (started_) {
        return;
    }
    started_ = true;

    // 后台消费协程：各自独立 try/catch+日志，异常不会静默吞掉。
    co_spawn_logged(ex_, md_loop(), "TqApi.md_loop");
    co_spawn_logged(ex_, td_loop(), "TqApi.td_loop");

    // 启动网关（网关自身的异步启动也 spawn 出去，不阻塞构造方）。
    if (md_) {
        co_spawn_logged(
            ex_,
            [](MarketDataGateway& md) -> asio::awaitable<void> { co_await md.start(); }(*md_),
            "TqApi.md_start");
    }
    if (td_) {
        co_spawn_logged(
            ex_,
            [](TradingGateway& td) -> asio::awaitable<void> { co_await td.start(); }(*td_),
            "TqApi.td_start");
    }
}

void TqApi::close() {
    if (closed_) {
        return;
    }
    closed_ = true;

    // 关闭通道即可让 md_loop / td_loop 以 nullopt 退出。
    if (md_) {
        md_->output().close();
    }
    if (td_) {
        td_->event_output().close();
    }

    // 唤醒所有挂起的 wait_update：closed_ 置位后它们会返回 false。
    update_signal_.notify();
}

// ---------- 后台消费循环 ----------
asio::awaitable<void> TqApi::md_loop() {
    if (!md_) {
        co_return;
    }
    auto& ch = md_->output();
    while (true) {
        auto ev = co_await ch.async_receive();
        if (!ev) {
            break; // 通道已关闭
        }
        switch (ev->kind) {
        case MarketDataEvent::Kind::Diff:
        case MarketDataEvent::Kind::Kline:
            merger_.apply(ev->payload);
            update_signal_.notify();
            break;
        case MarketDataEvent::Kind::Disconnected:
            spdlog::warn("[TqApi.md_loop] market data disconnected");
            break;
        case MarketDataEvent::Kind::Error:
            spdlog::error("[TqApi.md_loop] md error: {}", ev->error_msg);
            break;
        }
    }
    spdlog::debug("[TqApi.md_loop] exited");
    co_return;
}

asio::awaitable<void> TqApi::td_loop() {
    if (!td_) {
        co_return;
    }
    auto& ch = td_->event_output();
    while (true) {
        auto ev = co_await ch.async_receive();
        if (!ev) {
            break;
        }
        switch (ev->kind) {
        case TradeEvent::Kind::Diff:
            merger_.apply(ev->payload);
            update_signal_.notify();
            break;
        case TradeEvent::Kind::Ack:
            // 命令级回执：阶段 2 不单独触发 wait_update，
            // 真正的状态推进由后续 Diff 驱动。
            break;
        case TradeEvent::Kind::Disconnected:
            spdlog::warn("[TqApi.td_loop] trading disconnected");
            break;
        case TradeEvent::Kind::Error:
            spdlog::error("[TqApi.td_loop] td error: {}", ev->error_msg);
            break;
        }
    }
    spdlog::debug("[TqApi.td_loop] exited");
    co_return;
}

// ---------- wait_update ----------
asio::awaitable<bool> TqApi::wait_update() {
    if (closed_) {
        co_return false;
    }

    // 进入新一轮等待前清位：把"上一轮 wait_update 返回后用户已读过"的变化标记复位。
    // 这样在本次 wait_update 返回后，用户仍可通过 is_changing 看到本轮变化；
    // 直到下一次进入 wait_update 才被清除（语义 = "自上次 wait 以来"）。
    if (cleared_once_) {
        data_.clear_all_changes();
    }
    cleared_once_ = true;

    co_await update_signal_.wait();

    if (closed_) {
        // close() 也会 notify 来唤醒这里。
        co_return false;
    }

    // 此刻挂起期间到达的所有 diff 都已被 md/td loop apply 过，
    // changed_fields 即为"本轮变化"，无需在此清位，直接返回给用户查询。
    co_return true;
}

// ---------- 本地读 ----------
Quote& TqApi::get_quote(std::string_view symbol) {
    auto& q = data_.quotes.get_or_create(std::string(symbol));
    if (q.instrument_id.empty()) {
        q.instrument_id = std::string(symbol);
    }
    // 阶段 2：订阅为空操作（mock 无需订阅）。
    if (md_) {
        std::vector<std::string> syms{std::string(symbol)};
        co_spawn_logged(
            ex_,
            [](MarketDataGateway& md, std::vector<std::string> s) -> asio::awaitable<void> {
                co_await md.subscribe(s);
            }(*md_, std::move(syms)),
            "TqApi.subscribe");
    }
    return q;
}

Order* TqApi::get_order(std::string_view order_id) {
    return data_.orders.find(std::string(order_id));
}

Account& TqApi::get_account() {
    return data_.account;
}

KlineStream* TqApi::get_kline(std::string_view symbol, int64_t duration_ns) {
    return data_.klines.find(symbol, duration_ns);
}

bool TqApi::is_changing(const Quote& q, uint64_t field_bits) const noexcept {
    return (q.changed_fields & field_bits) != 0;
}

bool TqApi::is_changing(const Order& o, uint64_t field_bits) const noexcept {
    return (o.changed_fields & field_bits) != 0;
}

bool TqApi::is_changing(const Account& a, uint64_t field_bits) const noexcept {
    return (a.changed_fields & field_bits) != 0;
}

// ---------- 交易 ----------
std::string TqApi::next_order_id() {
    return "rid_" + std::to_string(order_seq_.fetch_add(1, std::memory_order_relaxed));
}

asio::awaitable<Order*> TqApi::insert_order(const InsertOrderReq& req) {
    // 1) 本地造占位单，返回其指针（生命周期到 close 前，发单 ack 前即可见）。
    std::string rid = next_order_id();
    auto&       o   = data_.orders.get_or_create(rid);
    o.order_id      = rid;
    o.instrument_id = req.instrument_id;
    o.exchange_id   = req.exchange_id;
    o.direction     = req.direction;
    o.offset        = req.offset;
    o.price_type    = req.price_type;
    o.limit_price   = req.limit_price;
    o.time_cond     = req.time_cond;
    o.volume_cond   = req.volume_cond;
    o.min_volume    = req.min_volume;
    o.volume_orign  = req.volume;
    o.volume_left   = req.volume;
    o.status        = OrderStatus::ALIVE;
    o.is_dead       = false;
    o.is_error      = false;

    // 2) 发给网关（异步发命令，不等终态）。
    if (td_) {
        CommandAck ack = co_await td_->insert_order(req, rid);
        if (!ack.success) {
            // 网关当场拒绝：本地立即标记为错误终态，无需再等 Diff。
            o.is_error = true;
            o.is_dead  = true;
            o.status   = OrderStatus::FINISHED;
            o.last_msg = ack.error_msg;
            o.changed_fields |= (uint64_t)OrderField::is_error | (uint64_t)OrderField::is_dead |
                                (uint64_t)OrderField::status | (uint64_t)OrderField::last_msg;
        }
    } else {
        // 无交易网关：无法发单，直接标记为错误终态。
        o.is_error = true;
        o.is_dead  = true;
        o.status   = OrderStatus::FINISHED;
        o.last_msg = "no trading gateway";
        o.changed_fields |= (uint64_t)OrderField::is_error | (uint64_t)OrderField::is_dead |
                            (uint64_t)OrderField::status | (uint64_t)OrderField::last_msg;
    }

    co_return &o;
}

asio::awaitable<CommandAck> TqApi::cancel_order(const CancelOrderReq& req) {
    if (!td_) {
        co_return CommandAck{
            .success = false, .order_id = req.order_id, .error_msg = "no trading gateway"};
    }
    co_return co_await td_->cancel_order(req);
}

} // namespace tqsdk
