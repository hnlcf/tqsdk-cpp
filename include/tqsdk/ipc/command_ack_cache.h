#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/gateway/trading_gateway.h"

namespace tqsdk::ipc {

/// 命令幂等缓存。
///
/// node 在 ACK 超时后可能用同一 rid 重发 Command；gw 必须复用首次 ack，
/// 避免把非幂等的 CTP 报单重复提交。缓存有界，按插入顺序淘汰最旧项。
class CommandAckCache {
public:
    explicit CommandAckCache(std::size_t max_entries = 4096)
        : max_entries_(max_entries) {}

    std::optional<CommandAck> find(std::string_view key) const {
        auto it = acks_.find(std::string{key});
        if (it == acks_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void put(std::string key, CommandAck ack) {
        if (max_entries_ == 0) {
            return;
        }
        if (acks_.contains(key)) {
            acks_[key] = std::move(ack);
            return;
        }
        while (keys_.size() >= max_entries_ && !keys_.empty()) {
            acks_.erase(keys_.front());
            keys_.pop_front();
        }
        keys_.push_back(key);
        acks_.emplace(std::move(key), std::move(ack));
    }

    void clear() {
        acks_.clear();
        keys_.clear();
    }

    std::size_t size() const noexcept { return acks_.size(); }

private:
    std::size_t                                 max_entries_;
    std::unordered_map<std::string, CommandAck> acks_;
    std::deque<std::string>                     keys_;
};

} // namespace tqsdk::ipc
