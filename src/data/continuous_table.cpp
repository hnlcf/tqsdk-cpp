#include "tqsdk/data/continuous_table.h"

#include "tqsdk/util/http_download.h"
#include "tqsdk/util/log.h"

#include <algorithm>
#include <ctime>
#include <fstream>

namespace tqsdk {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string date_key_impl(std::int64_t datetime_ns) {
    const auto seconds = std::chrono::seconds(datetime_ns / 1'000'000'000LL);
    auto       point   = std::chrono::system_clock::time_point{seconds} + std::chrono::hours(8);
    const auto time    = std::chrono::system_clock::to_time_t(point);
    std::tm    tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[16]{};
    std::snprintf(
        buffer, sizeof(buffer), "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buffer;
}

bool number(std::string_view value, double& out) {
    if (value.empty())
        return false;
    try {
        std::size_t used = 0;
        out              = std::stod(std::string(value), &used);
        return used == value.size();
    } catch (...) {
        return false;
    }
}

} // namespace

ContinuousTable::ContinuousTable(ContinuousTableConfig config)
    : config_(std::move(config)) {}

ContinuousTable::~ContinuousTable() {
    stop();
}

std::string ContinuousTable::date_key(std::int64_t datetime_ns) {
    return date_key_impl(datetime_ns);
}

bool ContinuousTable::load_local() {
    if (!config_.enabled || config_.file.empty())
        return false;
    std::ifstream input(config_.file);
    if (!input.good())
        return false;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parse(text);
}

bool ContinuousTable::parse(std::string_view json_text) {
    try {
        const auto root = nlohmann::json::parse(json_text);
        if (!root.is_object())
            return false;
        ProductMap products;
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!it.value().is_array())
                continue;
            std::vector<Segment> segments;
            for (const auto& row : it.value()) {
                if (!row.is_array() || row.size() < 2 || !row[0].is_string() || !row[1].is_string())
                    continue;
                segments.push_back(Segment{row[0].get<std::string>(), row[1].get<std::string>()});
            }
            if (segments.empty())
                continue;
            std::sort(segments.begin(), segments.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.date < rhs.date;
            });
            products.emplace(it.key(), std::move(segments));
        }
        if (products.empty())
            return false;
        {
            std::lock_guard lock(mutex_);
            products_ = std::move(products);
            last_error_.clear();
            loaded_at_ms_ = now_ms();
        }
        return true;
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        last_error_ = e.what();
        return false;
    }
}

bool ContinuousTable::ready() const noexcept {
    std::lock_guard lock(mutex_);
    return !products_.empty();
}

std::string ContinuousTable::product_of(std::string_view symbol) const {
    constexpr std::string_view main_prefix  = "KQ.m@";
    constexpr std::string_view index_prefix = "KQ.i@";
    if (symbol.starts_with(main_prefix))
        return std::string(symbol.substr(main_prefix.size()));
    if (symbol.starts_with(index_prefix))
        return std::string(symbol.substr(index_prefix.size()));
    return {};
}

bool ContinuousTable::is_main_continuous(std::string_view symbol) const {
    return symbol.starts_with("KQ.m@") && symbol.size() > 5;
}

std::vector<ContinuousRoll> ContinuousTable::rolls_for(std::string_view product,
                                                       std::string_view start_date,
                                                       std::string_view end_date) const {
    std::lock_guard lock(mutex_);
    const auto      it = products_.find(std::string(product));
    if (it == products_.end())
        return {};
    std::vector<ContinuousRoll> out;
    for (std::size_t i = 1; i < it->second.size(); ++i) {
        const auto& current = it->second[i];
        if (!start_date.empty() && current.date < start_date)
            continue;
        if (!end_date.empty() && current.date > end_date)
            continue;
        out.push_back(ContinuousRoll{current.date, it->second[i - 1].contract, current.contract});
    }
    return out;
}

nlohmann::json ContinuousTable::status_json() const {
    std::lock_guard lock(mutex_);
    return nlohmann::json{{"enabled", config_.enabled},
                          {"ready", !products_.empty()},
                          {"products", products_.size()},
                          {"file", config_.file},
                          {"url", config_.url},
                          {"loaded_at_ms", loaded_at_ms_},
                          {"error", last_error_}};
}

void ContinuousTable::start_background_refresh() {
    if (!config_.enabled || worker_.joinable())
        return;
    stop_requested_ = false;
    worker_         = std::jthread([this](std::stop_token stop) {
        {
            std::unique_lock lock(mutex_);
            stop_cv_.wait_for(lock, config_.initial_delay, [&] {
                return stop.stop_requested() || stop_requested_;
            });
        }
        while (!stop.stop_requested() && !stop_requested_) {
            try {
                const auto text = util::http_get(
                    config_.url, constants::kDefaultNodeUserAgent, std::chrono::seconds(30));
                if (parse(text) && !config_.file.empty()) {
                    const auto temp = config_.file + ".tmp";
                    {
                        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                        out << text;
                    }
                    std::error_code ec;
                    std::filesystem::rename(temp, config_.file, ec);
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex_);
                last_error_ = e.what();
                SPDLOG_WARN("[continuous] refresh failed: {}", e.what());
            }
            std::unique_lock lock(mutex_);
            stop_cv_.wait_for(lock, config_.refresh_interval, [&] {
                return stop.stop_requested() || stop_requested_;
            });
        }
    });
}

void ContinuousTable::stop() noexcept {
    stop_requested_ = true;
    if (worker_.joinable()) {
        worker_.request_stop();
        stop_cv_.notify_all();
        worker_.join();
    }
}

AdjustedKline
ContinuousTable::adjust_daily(const std::vector<KlineBar>&                        raw,
                              const std::vector<ContinuousRoll>&                  rolls,
                              const std::map<std::string, std::vector<KlineBar>>& contract_bars,
                              KlineAdjustMode                                     mode) {
    AdjustedKline result;
    result.bars = raw;
    if (mode == KlineAdjustMode::None || raw.empty() || rolls.empty())
        return result;

    std::vector<std::pair<std::string, double>> ratios;
    ratios.reserve(rolls.size());
    for (const auto& roll : rolls) {
        const auto old_it = contract_bars.find(roll.from);
        const auto new_it = contract_bars.find(roll.to);
        if (old_it == contract_bars.end() || new_it == contract_bars.end()) {
            result.warnings.push_back("missing contract bars for roll " + roll.date);
            continue;
        }
        const KlineBar* old_bar = nullptr;
        const KlineBar* new_bar = nullptr;
        for (const auto& bar : old_it->second) {
            if (date_key(bar.datetime) < roll.date)
                old_bar = &bar;
            else
                break;
        }
        for (const auto& bar : new_it->second) {
            if (date_key(bar.datetime) >= roll.date) {
                new_bar = &bar;
                break;
            }
        }
        if (old_bar == nullptr || new_bar == nullptr || !std::isfinite(old_bar->close) ||
            !std::isfinite(new_bar->close) || old_bar->close <= 0.0 || new_bar->close <= 0.0) {
            result.warnings.push_back("no usable prices for roll " + roll.date);
            continue;
        }
        ratios.emplace_back(roll.date, new_bar->close / old_bar->close);
    }
    if (ratios.empty())
        return result;

    for (auto& bar : result.bars) {
        const auto date   = date_key(bar.datetime);
        double     factor = 1.0;
        for (const auto& [roll_date, ratio] : ratios) {
            if (mode == KlineAdjustMode::Forward) {
                if (roll_date > date)
                    factor *= ratio;
            } else if (roll_date <= date) {
                factor /= ratio;
            }
        }
        if (std::abs(factor - 1.0) < 1e-12)
            continue;
        if (std::isfinite(bar.open))
            bar.open *= factor;
        if (std::isfinite(bar.high))
            bar.high *= factor;
        if (std::isfinite(bar.low))
            bar.low *= factor;
        if (std::isfinite(bar.close))
            bar.close *= factor;
    }
    result.adjusted = true;
    return result;
}

} // namespace tqsdk
