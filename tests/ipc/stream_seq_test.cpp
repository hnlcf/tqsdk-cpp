#include "tqsdk/pch.h"

#include "tqsdk/ipc/stream_seq.h"

using namespace tqsdk::ipc;

TEST(StreamTracker, InOrderGapDuplicateAndEpochChange) {
    StreamTracker tracker(StreamKind::Md);

    StreamMeta m{"epoch-a", 10};
    EXPECT_EQ(tracker.observe(m), StreamTracker::Result::Started);
    EXPECT_EQ(tracker.expected_seq(), 11u);

    m.seq = 11;
    EXPECT_EQ(tracker.observe(m), StreamTracker::Result::InOrder);
    EXPECT_EQ(tracker.expected_seq(), 12u);

    m.seq = 11;
    EXPECT_EQ(tracker.observe(m), StreamTracker::Result::Duplicate);
    EXPECT_EQ(tracker.duplicate_count(), 1u);

    StreamGap gap;
    m.seq = 15;
    EXPECT_EQ(tracker.observe(m, &gap), StreamTracker::Result::Gap);
    EXPECT_EQ(gap.expected, 12u);
    EXPECT_EQ(gap.received, 15u);
    EXPECT_EQ(gap.missing, 3u);
    EXPECT_EQ(tracker.gap_count(), 1u);

    // 新 epoch：当作新流，从当前 seq 重新开始。
    m.epoch = "epoch-b";
    m.seq   = 100;
    EXPECT_EQ(tracker.observe(m), StreamTracker::Result::EpochChanged);
    EXPECT_EQ(tracker.expected_seq(), 101u);
    EXPECT_EQ(tracker.epoch_change_count(), 1u);

    // reset 丢弃基线，下一次 observe 重新 Started。
    tracker.reset();
    m.seq = 500;
    EXPECT_EQ(tracker.observe(m), StreamTracker::Result::Started);
    EXPECT_EQ(tracker.expected_seq(), 501u);
}

TEST(StreamTracker, SeqZeroIsIgnored) {
    StreamTracker tracker(StreamKind::Td);
    EXPECT_EQ(tracker.observe(StreamMeta{"", 0}), StreamTracker::Result::Ignored);
    EXPECT_FALSE(tracker.initialized());
}

TEST(OutboundStream, AssignsSeqAndReplays) {
    OutboundStream stream(StreamKind::Md, "epoch-1", ReplayConfig{8, 1024});

    auto a = stream.next_meta();
    auto b = stream.next_meta();
    auto c = stream.next_meta();
    ASSERT_EQ(a.seq, 1u);
    ASSERT_EQ(b.seq, 2u);
    ASSERT_EQ(c.seq, 3u);
    EXPECT_EQ(a.epoch, "epoch-1");

    stream.record(a.seq, FrameType::MarketDataPush, FrameEncoding::Json, {1, 2});
    stream.record(b.seq, FrameType::MarketDataPush, FrameEncoding::Json, {3, 4});
    stream.record(c.seq, FrameType::MarketDataPush, FrameEncoding::Json, {5, 6});

    std::vector<ReplayFrame> frames;
    ASSERT_TRUE(stream.replay_from(2, frames));
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].seq, 2u);
    EXPECT_EQ(frames[1].seq, 3u);
    EXPECT_EQ(frames[0].payload, (std::vector<uint8_t>{3, 4}));

    frames.clear();
    EXPECT_TRUE(stream.replay_from(4, frames)); // 下一条尚未产生：空 replay 合法
    EXPECT_TRUE(frames.empty());

    EXPECT_FALSE(stream.replay_from(0, frames));
    EXPECT_FALSE(stream.replay_from(5, frames));
}

TEST(OutboundStream, EvictsOldFramesByCountAndBytes) {
    OutboundStream stream(StreamKind::Td, "epoch-1", ReplayConfig{2, 5});
    for (uint64_t seq = 1; seq <= 4; ++seq) {
        auto m = stream.next_meta();
        stream.record(m.seq, FrameType::TradePush, FrameEncoding::Json, {1, 1});
    }
    EXPECT_EQ(stream.buffered_frames(), 2u); // 字节上限 5 -> 每帧 2B，最多 2 帧
    EXPECT_EQ(stream.first_seq(), 3u);

    std::vector<ReplayFrame> frames;
    EXPECT_FALSE(stream.replay_from(1, frames)); // 已被淘汰
    EXPECT_TRUE(stream.replay_from(3, frames));
    EXPECT_EQ(frames.size(), 2u);
}
