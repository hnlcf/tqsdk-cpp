#include "tqsdk/gateway/ctp/ctp_order_store.h"

#include <filesystem>
#include <fstream>

namespace tqsdk::ctp {

namespace {

nlohmann::json to_json(const StoredOrder& o) {
    return nlohmann::json{
        {"rid", o.rid},
        {"ref", o.ref},
        {"instrument_id", o.instrument_id},
        {"exchange_id", o.exchange_id},
        {"order_sys_id", o.order_sys_id},
        {"status", o.status},
        {"message", o.message},
        {"trading_day", o.trading_day},
        {"front_id", o.front_id},
        {"session_id", o.session_id},
        {"ack_success", o.ack_success},
        {"order_json", o.order_json},
    };
}

StoredOrder from_json(const nlohmann::json& j) {
    StoredOrder o;
    o.rid           = j.value("rid", std::string{});
    o.ref           = j.value("ref", std::string{});
    o.instrument_id = j.value("instrument_id", std::string{});
    o.exchange_id   = j.value("exchange_id", std::string{});
    o.order_sys_id  = j.value("order_sys_id", std::string{});
    o.status        = j.value("status", std::string{});
    o.message       = j.value("message", std::string{});
    o.trading_day   = j.value("trading_day", std::string{});
    o.front_id      = j.value("front_id", 0);
    o.session_id    = j.value("session_id", 0);
    o.ack_success   = j.value("ack_success", false);
    if (auto it = j.find("order_json"); it != j.end() && it->is_object()) {
        o.order_json = *it;
    }
    return o;
}

} // namespace

CtpOrderStore::CtpOrderStore(std::string path)
    : path_(std::move(path)) {}

void CtpOrderStore::load() {
    orders_.clear();
    trading_day_.clear();
    std::ifstream in(path_);
    if (!in.is_open()) {
        return;
    }
    try {
        nlohmann::json j;
        in >> j;
        trading_day_ = j.value("trading_day", std::string{});
        if (auto it = j.find("orders"); it != j.end() && it->is_object()) {
            for (const auto& [rid, value] : it->items()) {
                auto order   = from_json(value);
                order.rid    = rid;
                orders_[rid] = std::move(order);
            }
        }
        SPDLOG_INFO("[CtpStore] loaded {} persisted order(s) from {}", orders_.size(), path_);
    } catch (const std::exception& e) {
        SPDLOG_WARN("[CtpStore] load {} failed: {}", path_, e.what());
        orders_.clear();
        trading_day_.clear();
    }
}

bool CtpOrderStore::find(const std::string& rid, StoredOrder& out) const {
    auto it = orders_.find(rid);
    if (it == orders_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

void CtpOrderStore::upsert(StoredOrder order) {
    if (order.rid.empty()) {
        return;
    }
    orders_[order.rid] = std::move(order);
    save();
}

void CtpOrderStore::set_trading_day(std::string day) {
    if (!trading_day_.empty() && trading_day_ != day) {
        SPDLOG_INFO("[CtpStore] trading day {} -> {}; clear persisted orders", trading_day_, day);
        orders_.clear();
    }
    trading_day_ = std::move(day);
    save();
}

std::vector<StoredOrder> CtpOrderStore::orders() const {
    std::vector<StoredOrder> out;
    out.reserve(orders_.size());
    for (const auto& [rid, order] : orders_) {
        (void)rid;
        out.push_back(order);
    }
    return out;
}

void CtpOrderStore::save() const {
    const auto      path = std::filesystem::path(path_);
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    nlohmann::json j;
    j["trading_day"] = trading_day_;
    j["orders"]      = nlohmann::json::object();
    for (const auto& [rid, order] : orders_) {
        j["orders"][rid] = to_json(order);
    }

    const auto tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            SPDLOG_WARN("[CtpStore] open {} for write failed", tmp);
            return;
        }
        out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        out.put('\n');
    }
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        std::filesystem::remove(path_, ec);
        ec.clear();
        std::filesystem::rename(tmp, path_, ec);
        if (ec) {
            SPDLOG_WARN("[CtpStore] rename {} -> {} failed: {}", tmp, path_, ec.message());
        }
    }
}

} // namespace tqsdk::ctp
