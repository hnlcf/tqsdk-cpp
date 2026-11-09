#pragma once

#include "tqsdk/constants.h"
#include "tqsdk/pch.h"

#include <condition_variable>
#include <shared_mutex>
#include <thread>

namespace tqsdk {

/// 中国期货非周末节假日表。
///
/// 数据源：https://files.shinnytech.com/shinny_chinese_holiday.json
/// 文件格式为 ["2003-01-01", ...]，只包含工作日节假日，不包含周末。
///
/// 该对象可独立用于测试；生产进程通过 `global()` 共享一份，由 CTP/Remote
/// gateway 在启动时调用 `start_global()`，后台线程按 refresh_interval 下载更新。
class HolidayCalendar {
public:
    struct Config {
        bool                 enabled = true;
        std::string          file    = "/tmp/tqsdk_holidays.json";
        std::string          url     = std::string(constants::kTqHolidayUrl);
        std::chrono::seconds refresh_interval{12 * 3600};
        std::chrono::seconds initial_delay{0};
    };

    HolidayCalendar() = default;
    explicit HolidayCalendar(Config cfg);
    ~HolidayCalendar();

    HolidayCalendar(const HolidayCalendar&)            = delete;
    HolidayCalendar& operator=(const HolidayCalendar&) = delete;

    /// 从本地文件加载；文件不存在/格式错误返回 false，不覆盖已有数据。
    bool load_local();
    /// 从 URL 下载并原子替换；失败保留旧数据。
    bool refresh_now();

    /// 启动后台刷新线程；幂等。首次若没有本地数据会立即尝试下载。
    void start_background_refresh();
    void stop();

    bool             loaded() const;
    bool             is_holiday(int date_key) const;
    std::vector<int> holidays() const;
    nlohmann::json   status_json() const;

    /// 进程级共享实例。调用者应在 gateway start() 中触发 start_global()。
    static HolidayCalendar& global();
    static void             start_global();

private:
    void load_from_json_text(std::string_view text, std::string_view source);
    void refresh_loop(std::stop_token stop);

    Config                    cfg_;
    mutable std::shared_mutex mutex_;
    std::unordered_set<int>   dates_;
    std::string               source_;
    std::size_t               version_ = 0;
    std::atomic<bool>         loaded_{false};
    std::thread               worker_;
    mutable std::mutex        worker_mutex_;
    std::mutex                refresh_mutex_;
    std::condition_variable   stop_cv_;
    bool                      stop_requested_ = false;
};

/// "YYYY-MM-DD" -> YYYYMMDD；非法返回 0。
int parse_holiday_date(std::string_view text) noexcept;

} // namespace tqsdk
