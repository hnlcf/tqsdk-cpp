#pragma once

#include "tqsdk/pch.h"

#include "tqsdk/domain/kline.h"

namespace tqsdk {

/// 天勤主连换月表：product -> [[yyyymmdd, contract], ...]。
struct ContinuousTableConfig {
    bool                 enabled = true;
    std::string          url     = "https://files.shinnytech.com/continuous_table.json";
    std::string          file    = "continuous_table.json";
    std::chrono::seconds refresh_interval{std::chrono::hours(24)};
    std::chrono::seconds initial_delay{std::chrono::seconds(1)};
};

struct ContinuousRoll {
    std::string date; ///< yyyymmdd, new contract becomes main on this date
    std::string from; ///< contract before roll
    std::string to;   ///< contract after roll
};

enum class KlineAdjustMode {
    None,
    Forward, ///< 前复权：最新价格不变，历史价格按换月比例调整
    Back,    ///< 后复权：历史价格不变，后续价格按换月比例调整
};

struct AdjustedKline {
    std::vector<KlineBar>    bars;
    std::vector<std::string> warnings;
    bool                     adjusted = false;
};

/// 连续合约表与 1d 复权算法。
///
/// 只对 `KQ.m@...` 主连做复权；`KQ.i@...` 指数本身无换月跳空，不做复权。
/// 复权需要换月附近两个具体合约的日线收盘价；调用方通过 contract_bars 提供
/// `{"DCE.jm2509": [...], "DCE.jm2601": [...]}`。缺失时返回 warnings 且不调整
/// 对应换月。
class ContinuousTable {
public:
    explicit ContinuousTable(ContinuousTableConfig config);
    ~ContinuousTable();

    bool load_local();
    bool parse(std::string_view json_text);
    void start_background_refresh();
    void stop() noexcept;

    bool                        enabled() const noexcept { return config_.enabled; }
    bool                        ready() const noexcept;
    std::string                 product_of(std::string_view symbol) const;
    bool                        is_main_continuous(std::string_view symbol) const;
    std::vector<ContinuousRoll> rolls_for(std::string_view product,
                                          std::string_view start_date,
                                          std::string_view end_date) const;
    nlohmann::json              status_json() const;

    /// 天勤日线 datetime(ns) -> 上海时区 yyyymmdd。
    static std::string date_key(std::int64_t datetime_ns);

    /// 纯函数复权；raw 必须按 datetime 升序。
    static AdjustedKline
    adjust_daily(const std::vector<KlineBar>&                        raw,
                 const std::vector<ContinuousRoll>&                  rolls,
                 const std::map<std::string, std::vector<KlineBar>>& contract_bars,
                 KlineAdjustMode                                     mode);

private:
    struct Segment {
        std::string date;
        std::string contract;
    };
    using ProductMap = std::map<std::string, std::vector<Segment>>;

    ContinuousTableConfig       config_;
    mutable std::mutex          mutex_;
    ProductMap                  products_;
    std::string                 last_error_;
    std::int64_t                loaded_at_ms_ = 0;
    std::jthread                worker_;
    std::condition_variable_any stop_cv_;
    bool                        stop_requested_ = false;
};

} // namespace tqsdk
