#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// 固定窗口时间序列。
///
/// 语义与 TqSdk 的 DataSeries 对齐：
/// - 只保留最近 capacity 个元素；
/// - append 超出容量时从头部丢弃；
/// - at/back 只做边界检查，不抛异常；
/// - revision 是追加/重置序号，调用方可据此判断是否需要重绘。
template <typename T>
class DataSeries {
public:
    explicit DataSeries(std::size_t capacity = 200) : capacity_(normalize_capacity(capacity)) {}

    void reset(std::size_t capacity) {
        capacity_ = normalize_capacity(capacity);
        items_.clear();
        ++revision_;
    }

    void clear() noexcept {
        items_.clear();
        ++revision_;
    }

    void append(T value) {
        items_.push_back(std::move(value));
        while (items_.size() > capacity_) {
            items_.pop_front();
        }
        ++revision_;
    }

    std::size_t size() const noexcept { return items_.size(); }
    bool        empty() const noexcept { return items_.empty(); }
    std::size_t capacity() const noexcept { return capacity_; }
    void        set_capacity(std::size_t capacity) { capacity_ = normalize_capacity(capacity); }
    std::uint64_t revision() const noexcept { return revision_; }

    T*       at(std::size_t index) noexcept {
        return index < items_.size() ? &items_[index] : nullptr;
    }
    const T* at(std::size_t index) const noexcept {
        return index < items_.size() ? &items_[index] : nullptr;
    }
    T*       back() noexcept { return items_.empty() ? nullptr : &items_.back(); }
    const T* back() const noexcept { return items_.empty() ? nullptr : &items_.back(); }

    auto begin() noexcept { return items_.begin(); }
    auto end() noexcept { return items_.end(); }
    auto begin() const noexcept { return items_.begin(); }
    auto end() const noexcept { return items_.end(); }

private:
    static std::size_t normalize_capacity(std::size_t value) noexcept {
        return value == 0 ? 1 : value;
    }

    std::size_t    capacity_;
    std::deque<T>  items_;
    std::uint64_t  revision_ = 0;
};

} // namespace tqsdk
