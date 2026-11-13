#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

// 在 executor 上启动协程；协程抛出的任何异常都会被捕获并记日志
// tag 用于日志中标识协程来源(如 "TqApi.merge_loop")
template <typename F>
void co_spawn_logged(boost::asio::any_io_executor ex, F&& f, std::string_view tag) {
    boost::asio::co_spawn(
        ex, std::forward<F>(f), [tag = std::string(tag)](std::exception_ptr eptr) {
            if (!eptr) {
                return;
            }
            try {
                std::rethrow_exception(eptr);
            } catch (const boost::system::system_error& e) {
                // operation_aborted 是关闭路径,降级为 warn
                if (e.code() == boost::asio::error::operation_aborted) {
                    SPDLOG_DEBUG("[{}] coroutine aborted (normal shutdown)", tag);
                } else {
                    SPDLOG_ERROR("[{}] system_error: {} ({})", tag, e.what(), e.code().value());
                }
            } catch (const std::exception& e) {
                SPDLOG_ERROR("[{}] exception: {}", tag, e.what());
            } catch (...) {
                SPDLOG_ERROR("[{}] unknown exception", tag);
            }
        });
}

} // namespace tqsdk
