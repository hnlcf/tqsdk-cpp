#include "tqsdk/trading/holiday_calendar.h"

#include "tqsdk/util/http_download.h"

#include <fstream>

#if defined(_WIN32)
    #include <process.h>
#else
    #include <unistd.h>
#endif

namespace tqsdk {

namespace {

int parse_date_digits(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return 0;
    }
    auto digit = [&](int at) { return text[at] >= '0' && text[at] <= '9'; };
    if (!digit(0) || !digit(1) || !digit(2) || !digit(3) || !digit(5) || !digit(6) || !digit(8) ||
        !digit(9)) {
        return 0;
    }
    const int year =
        (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 + (text[3] - '0');
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day   = (text[8] - '0') * 10 + (text[9] - '0');
    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    return year * 10000 + month * 100 + day;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

int parse_holiday_date(std::string_view text) noexcept {
    return parse_date_digits(text);
}

HolidayCalendar::HolidayCalendar(Config cfg)
    : cfg_(std::move(cfg)) {}

HolidayCalendar::~HolidayCalendar() {
    stop();
}

bool HolidayCalendar::load_local() {
    const auto text = read_file(cfg_.file);
    if (text.empty()) {
        return false;
    }
    try {
        load_from_json_text(text, cfg_.file);
        return true;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[Holiday] load {} failed: {}", cfg_.file, e.what());
        return false;
    }
}

bool HolidayCalendar::refresh_now() {
    if (!cfg_.enabled || cfg_.url.empty()) {
        return false;
    }
    std::lock_guard lock(refresh_mutex_);
    try {
        SPDLOG_INFO("[Holiday] downloading {}", cfg_.url);
        const auto text_result = util::http_get(cfg_.url, "tqsdk-node/0.1 holiday-calendar");
        if (!text_result) {
            SPDLOG_WARN("[Holiday] download failed: {} ({})",
                        text_result.error().detail,
                        util::http_error_name(text_result.error().code));
            return false;
        }
        const auto& text = *text_result;
        load_from_json_text(text, cfg_.url);

#if defined(_WIN32)
        const auto pid = static_cast<long>(_getpid());
#else
        const auto pid = static_cast<long>(::getpid());
#endif
        const auto tmp = fmt::format("{}.tmp.{}", cfg_.file, pid);
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, cfg_.file, ec);
        if (ec) {
            std::filesystem::remove(cfg_.file, ec);
            ec.clear();
            std::filesystem::rename(tmp, cfg_.file, ec);
        }
        return true;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[Holiday] download failed: {}", e.what());
        return false;
    }
}

void HolidayCalendar::load_from_json_text(std::string_view text, std::string_view source) {
    const auto doc = nlohmann::json::parse(text);
    if (!doc.is_array()) {
        throw std::runtime_error("holiday JSON must be an array");
    }
    std::unordered_set<int> dates;
    dates.reserve(doc.size());
    for (const auto& item : doc) {
        if (item.is_string()) {
            if (const int date = parse_date_digits(item.get<std::string>()); date != 0) {
                dates.insert(date);
            }
        }
    }
    if (dates.empty()) {
        throw std::runtime_error("holiday JSON contains no valid dates");
    }
    std::size_t count = 0;
    {
        std::unique_lock lock(mutex_);
        dates_  = std::move(dates);
        source_ = std::string{source};
        ++version_;
        count = dates_.size();
        loaded_.store(true, std::memory_order_release);
    }
    SPDLOG_INFO("[Holiday] loaded {} dates from {}", count, source);
}

void HolidayCalendar::start_background_refresh() {
    if (!cfg_.enabled) {
        return;
    }
    std::lock_guard lock(worker_mutex_);
    if (worker_.joinable()) {
        return;
    }
    stop_requested_ = false;
    if (!loaded()) {
        load_local();
    }
    worker_ = std::thread([this] {
        if (cfg_.initial_delay.count() > 0) {
            std::unique_lock lock(worker_mutex_);
            stop_cv_.wait_for(lock, cfg_.initial_delay, [&] { return stop_requested_; });
        }
        while (true) {
            {
                std::lock_guard lock(worker_mutex_);
                if (stop_requested_) {
                    return;
                }
            }
            refresh_now();
            std::unique_lock lock(worker_mutex_);
            stop_cv_.wait_for(lock, cfg_.refresh_interval, [&] { return stop_requested_; });
        }
    });
}

void HolidayCalendar::stop() {
    {
        std::lock_guard lock(worker_mutex_);
        stop_requested_ = true;
    }
    stop_cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

bool HolidayCalendar::loaded() const {
    return loaded_.load(std::memory_order_acquire);
}

bool HolidayCalendar::is_holiday(int date_key) const {
    std::shared_lock lock(mutex_);
    return dates_.contains(date_key);
}

std::vector<int> HolidayCalendar::holidays() const {
    std::shared_lock lock(mutex_);
    return {dates_.begin(), dates_.end()};
}

nlohmann::json HolidayCalendar::status_json() const {
    std::shared_lock lock(mutex_);
    return nlohmann::json{
        {"enabled", cfg_.enabled},
        {"loaded", loaded_.load(std::memory_order_relaxed)},
        {"source", source_},
        {"dates", dates_.size()},
        {"version", version_},
    };
}

HolidayCalendar& HolidayCalendar::global() {
    static HolidayCalendar instance;
    return instance;
}

void HolidayCalendar::start_global() {
    global().start_background_refresh();
}

} // namespace tqsdk
