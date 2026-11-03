#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/ipc/protocol.h"

namespace tqsdk {
namespace ipc {

/// 生成一个足够区分 gw 实例/重启的流 epoch（不要求全局唯一，只要求进程重启可变）。
inline std::string make_stream_epoch(std::string_view prefix = "ipc") {
    static std::atomic<uint64_t> counter{0};
    const auto                   ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
    return std::string(prefix) + "-" + std::to_string(ns) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

/// 一次 gap 的描述，交给上层决定 md resync / td reconcile。
struct StreamGap {
    StreamKind  kind = StreamKind::Md;
    std::string epoch;
    uint64_t    expected = 0;
    uint64_t    received = 0;
    uint64_t    missing  = 0;
};

/// 单条入站流的序号跟踪器。
///
/// 约定：
///   - seq == 0：旧对端未携带 seq，忽略（保持兼容）；
///   - epoch 变化：视为一次新流，直接以当前 seq 为新起点；
///   - seq < expected：重复帧；
///   - seq > expected：缺口。
///
/// 只在单一线程/strand 上使用（由 IpcClient 的读循环独占）。
class StreamTracker {
public:
    enum class Result {
        Ignored,      ///< 旧帧，没有 meta
        Started,      ///< 首次看到该流
        EpochChanged, ///< gw 重启/新流实例
        InOrder,
        Duplicate,
        Gap,
    };

    explicit StreamTracker(StreamKind kind)
        : kind_(kind) {}

    Result observe(const StreamMeta& meta, StreamGap* gap = nullptr) noexcept {
        if (meta.seq == 0) {
            return Result::Ignored;
        }
        if (!initialized_) {
            initialized_ = true;
            epoch_       = meta.epoch;
            expected_    = meta.seq + 1;
            return Result::Started;
        }
        if (meta.epoch != epoch_) {
            epoch_    = meta.epoch;
            expected_ = meta.seq + 1;
            ++epoch_changes_;
            return Result::EpochChanged;
        }
        if (meta.seq == expected_) {
            ++expected_;
            return Result::InOrder;
        }
        if (meta.seq < expected_) {
            ++duplicates_;
            return Result::Duplicate;
        }

        const uint64_t missing = meta.seq - expected_;
        if (gap != nullptr) {
            gap->kind     = kind_;
            gap->epoch    = meta.epoch;
            gap->expected = expected_;
            gap->received = meta.seq;
            gap->missing  = missing;
        }
        expected_ = meta.seq + 1;
        ++gaps_;
        return Result::Gap;
    }

    /// 丢弃历史基线（下一次 observe 以收到的 seq 重新开始）。
    /// 不清零统计计数，便于观测整个进程生命周期。
    void reset() noexcept {
        epoch_.clear();
        expected_    = 1;
        initialized_ = false;
    }

    StreamKind         kind() const noexcept { return kind_; }
    const std::string& epoch() const noexcept { return epoch_; }
    uint64_t           expected_seq() const noexcept { return expected_; }
    bool               initialized() const noexcept { return initialized_; }
    uint64_t           gap_count() const noexcept { return gaps_; }
    uint64_t           duplicate_count() const noexcept { return duplicates_; }
    uint64_t           epoch_change_count() const noexcept { return epoch_changes_; }

private:
    StreamKind  kind_;
    std::string epoch_;
    uint64_t    expected_      = 1;
    bool        initialized_   = false;
    uint64_t    gaps_          = 0;
    uint64_t    duplicates_    = 0;
    uint64_t    epoch_changes_ = 0;
};

/// 可重放帧：保存已编码的 payload，重连时直接按原字节重发。
struct ReplayFrame {
    uint64_t             seq      = 0;
    FrameType            type     = FrameType::Heartbeat;
    FrameEncoding        encoding = kDefaultEncoding;
    std::vector<uint8_t> payload;
};

struct ReplayConfig {
    std::size_t max_frames = 4096;
    std::size_t max_bytes  = 16u << 20; // 16MB
};

/// gw 侧单条出站流：分配 seq、记录 replay buffer。
///
/// 调用约束：每流单 writer（由 GatewayIpcServer 的 md_pump/td_pump 保证）。
class OutboundStream {
public:
    OutboundStream(StreamKind kind, std::string epoch, ReplayConfig cfg = {})
        : kind_(kind),
          epoch_(std::move(epoch)),
          cfg_(cfg) {
        if (cfg_.max_frames == 0) {
            cfg_.max_frames = 1;
        }
        if (cfg_.max_bytes == 0) {
            cfg_.max_bytes = 1;
        }
    }

    StreamKind         kind() const noexcept { return kind_; }
    const std::string& epoch() const noexcept { return epoch_; }
    uint64_t           next_seq() const noexcept { return next_seq_; }
    uint64_t           first_seq() const noexcept {
        return frames_.empty() ? next_seq_ : frames_.front().seq;
    }
    std::size_t buffered_frames() const noexcept { return frames_.size(); }
    std::size_t buffered_bytes() const noexcept { return bytes_; }

    /// 取下一个 seq。分配后若发送失败，seq 会留空；发送失败通常意味着连接断开。
    StreamMeta next_meta() {
        StreamMeta m;
        m.epoch = epoch_;
        m.seq   = next_seq_++;
        return m;
    }

    /// 记录一条已编码帧。超出容量时从最旧帧开始淘汰。
    void
    record(uint64_t seq, FrameType type, FrameEncoding encoding, std::vector<uint8_t> payload) {
        bytes_ += payload.size();
        frames_.push_back(ReplayFrame{seq, type, encoding, std::move(payload)});
        while (frames_.size() > cfg_.max_frames || bytes_ > cfg_.max_bytes) {
            evict_front();
        }
    }

    /// 尝试从 next_seq 开始重放。返回 false 表示缓冲区已不覆盖该位置。
    bool replay_from(uint64_t next_seq, std::vector<ReplayFrame>& out) const {
        if (next_seq == 0 || next_seq > next_seq_) {
            return false;
        }
        if (!frames_.empty() && next_seq < frames_.front().seq) {
            return false;
        }
        out.clear();
        if (next_seq == next_seq_) {
            return true;
        }
        for (const auto& f : frames_) {
            if (f.seq >= next_seq) {
                out.push_back(f);
            }
        }
        return !out.empty() && out.front().seq == next_seq;
    }

private:
    void evict_front() {
        if (frames_.empty()) {
            return;
        }
        bytes_ -= frames_.front().payload.size();
        frames_.pop_front();
    }

    StreamKind              kind_;
    std::string             epoch_;
    ReplayConfig            cfg_;
    uint64_t                next_seq_ = 1;
    std::size_t             bytes_    = 0;
    std::deque<ReplayFrame> frames_;
};

} // namespace ipc
} // namespace tqsdk
