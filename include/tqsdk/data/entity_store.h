#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

template <typename Entity, typename Key = std::string>
class EntityStore {
public:
    // 不存在则默认构造;存在则返回已有引用。
    // created 非空时回写"本次是否新建"，供调用方决定是否触发一次性的订阅等副作用。
    Entity& get_or_create(const Key& key, bool* created = nullptr) {
        auto [it, inserted] = data_.try_emplace(key);
        if (created) {
            *created = inserted;
        }
        return it->second;
    }

    Entity* find(const Key& key) noexcept {
        auto it = data_.find(key);
        return it == data_.end() ? nullptr : &it->second;
    }

    const Entity* find(const Key& key) const noexcept {
        auto it = data_.find(key);
        return it == data_.end() ? nullptr : &it->second;
    }

    // 把 bits 合进 changed_fields;首次变脏时入 dirty_list_
    void mark_changed(Entity& e, uint64_t bits) noexcept {
        if (bits == 0) {
            return;
        }
        bool was_clean = (e.changed_fields == 0);
        e.changed_fields |= bits;
        if (was_clean) {
            dirty_list_.push_back(&e);
        }
    }

    // 只清"这一轮脏过的",与总实体数无关
    void clear_changes() noexcept {
        for (Entity* e : dirty_list_) {
            e->changed_fields = 0;
        }
        dirty_list_.clear();
    }

    /// 清空全部实体（用于交易状态对账重建）。
    void clear() noexcept {
        data_.clear();
        dirty_list_.clear();
    }

    std::size_t size() const noexcept { return data_.size(); }
    bool        empty() const noexcept { return data_.empty(); }

    auto begin() noexcept { return data_.begin(); }
    auto end() noexcept { return data_.end(); }
    auto begin() const noexcept { return data_.begin(); }
    auto end() const noexcept { return data_.end(); }

private:
    std::unordered_map<Key, Entity> data_;
    std::vector<Entity*>            dirty_list_;
};

} // namespace tqsdk
