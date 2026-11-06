#include "tqsdk/pch.h"

#include "tqsdk/gateway/ctp/ctp_query_pipeline.h"

namespace asio = boost::asio;
using tqsdk::ctp::CtpQueryPipeline;

namespace {

CtpQueryPipeline::Callbacks make_callbacks(int&              sent,
                                           int&              completed,
                                           bool&             initial_completed,
                                           int&              failed,
                                           int&              restarted,
                                           std::vector<int>& request_ids) {
    return CtpQueryPipeline::Callbacks{
        .send =
            [&](CtpQueryPipeline::Job& job) {
                job.request_id = ++sent;
                request_ids.push_back(job.request_id);
                return 0;
            },
        .completed =
            [&](bool initial) {
                ++completed;
                initial_completed = initial;
            },
        .failed  = [&](CtpQueryPipeline::Kind) { ++failed; },
        .restart = [&]() { ++restarted; },
    };
}

} // namespace

TEST(CtpQueryPipeline, CompletesSingleJobAndMarksSnapshot) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    CtpQueryPipeline pipeline(
        io.get_executor(),
        make_callbacks(sent, completed, initial_completed, failed, restarted, request_ids));

    pipeline.start(true, {{CtpQueryPipeline::Kind::Account, "", 0, 0}});
    EXPECT_TRUE(pipeline.running());
    EXPECT_TRUE(pipeline.has_active());
    EXPECT_TRUE(pipeline.active_matches(CtpQueryPipeline::Kind::Account, 1));
    EXPECT_EQ(pipeline.pending(), 1u);

    pipeline.finish();
    EXPECT_FALSE(pipeline.running());
    EXPECT_TRUE(pipeline.snapshot_ready());
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(initial_completed);
    EXPECT_EQ(failed, 0);
}

TEST(CtpQueryPipeline, FailureAndWatchdogStillCompleteWindow) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    CtpQueryPipeline pipeline(
        io.get_executor(),
        make_callbacks(sent, completed, initial_completed, failed, restarted, request_ids));
    pipeline.set_query_timeout(std::chrono::milliseconds(20));

    pipeline.start(true, {{CtpQueryPipeline::Kind::Order, "", 0, 0}});
    io.run_for(std::chrono::milliseconds(80));

    EXPECT_FALSE(pipeline.running());
    EXPECT_FALSE(pipeline.has_active());
    EXPECT_TRUE(pipeline.snapshot_ready());
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(completed, 1);
    EXPECT_EQ(pipeline.pending(), 0u);
}

TEST(CtpQueryPipeline, EnqueueSpecsExtendsPendingWindow) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    CtpQueryPipeline pipeline(
        io.get_executor(),
        make_callbacks(sent, completed, initial_completed, failed, restarted, request_ids));

    pipeline.start(true, {{CtpQueryPipeline::Kind::Account, "", 0, 0}});
    pipeline.enqueue_specs({"SHFE.rb2701", "DCE.m2701"});
    EXPECT_EQ(pipeline.pending(), 3u);
    pipeline.finish();
    EXPECT_TRUE(pipeline.running());
    EXPECT_EQ(completed, 0);
}

TEST(CtpQueryPipeline, ThrottledSendIsRetried) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    int              attempts = 0;
    CtpQueryPipeline pipeline(io.get_executor(),
                              CtpQueryPipeline::Callbacks{
                                  .send =
                                      [&](CtpQueryPipeline::Job& job) {
                                          job.request_id = ++sent;
                                          if (attempts++ == 0) {
                                              return -2;
                                          }
                                          request_ids.push_back(job.request_id);
                                          return 0;
                                      },
                                  .completed =
                                      [&](bool initial) {
                                          ++completed;
                                          initial_completed = initial;
                                      },
                                  .failed  = [&](CtpQueryPipeline::Kind) { ++failed; },
                                  .restart = [&]() { ++restarted; },
                              });

    pipeline.start(true, {{CtpQueryPipeline::Kind::Account, "", 0, 0}});
    EXPECT_TRUE(pipeline.running());
    EXPECT_FALSE(pipeline.has_active());
    io.run_for(std::chrono::milliseconds(1200));
    EXPECT_TRUE(pipeline.has_active());
    pipeline.finish();
    EXPECT_EQ(failed, 0);
    EXPECT_EQ(completed, 1);
}

TEST(CtpQueryPipeline, ResetDiscardsTimedRestart) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    CtpQueryPipeline pipeline(
        io.get_executor(),
        make_callbacks(sent, completed, initial_completed, failed, restarted, request_ids));

    pipeline.schedule_restart(std::chrono::milliseconds(10));
    pipeline.reset();
    io.run_for(std::chrono::milliseconds(50));
    EXPECT_EQ(restarted, 0);
    EXPECT_FALSE(pipeline.running());
}

TEST(CtpQueryPipeline, RefreshRequestWaitsForCurrentWindow) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    CtpQueryPipeline pipeline(
        io.get_executor(),
        make_callbacks(sent, completed, initial_completed, failed, restarted, request_ids));

    pipeline.start(true, {{CtpQueryPipeline::Kind::Account, "", 0, 0}});
    pipeline.start(false, {{CtpQueryPipeline::Kind::Account, "", 0, 0}});
    EXPECT_EQ(pipeline.pending(), 1u); // 当前窗口不被并发打开
    pipeline.finish();
    EXPECT_EQ(completed, 1);

    io.run_for(std::chrono::milliseconds(300));
    EXPECT_EQ(restarted, 1); // complete 后的 debounce restart
}

TEST(CtpQueryPipeline, EmptyWindowCompletesImmediately) {
    asio::io_context io;
    int              sent = 0, completed = 0, failed = 0, restarted = 0;
    bool             initial_completed = false;
    std::vector<int> request_ids;
    CtpQueryPipeline pipeline(
        io.get_executor(),
        make_callbacks(sent, completed, initial_completed, failed, restarted, request_ids));

    pipeline.start(true, {});
    EXPECT_FALSE(pipeline.running());
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(pipeline.snapshot_ready());
}
