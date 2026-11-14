#pragma once

#include "tqsdk/constants.h"
#include "tqsdk/pch.h"

#include <shared_mutex>

namespace tqsdk {

class AuthTokenSource;

/// 标准合约目录（天勤 openmd symbol.json / latest.json）。
///
/// 只保留前端/网关真正需要的字段，避免把 300MB latest.json 的完整 DOM 常驻内存。
/// 目录由后台线程按配置刷新；HTTP 读取使用共享锁，不阻塞行情/交易线程。
class InstrumentCatalog {
public:
    struct Config {
        bool                 enabled = false;
        std::string          file    = "symbol.json";
        std::string          url     = std::string(constants::kTqSymbolUrl);
        std::chrono::seconds refresh_interval{6 * 3600};
        std::chrono::seconds initial_delay{60};
    };

    struct Instrument {
        std::string instrument_id;
        std::string exchange_id;
        std::string ins_id;
        std::string name;
        std::string class_name;
        std::string product_id;
        std::string product_short_name;
        std::string underlying_product;
        std::string underlying_symbol;
        std::string option_class; ///< CALL / PUT
        double      strike_price    = 0.0;
        double      last_price      = 0.0;
        double      pre_volume      = 0.0;
        double      open_interest   = 0.0;
        double      volume_multiple = 0.0;
        double      price_tick      = 0.0;
        int         price_decs      = 0;
        int         delivery_year   = 0;
        int         delivery_month  = 0;
        double      expire_datetime = 0.0;
        bool        expired         = false;
        bool        is_option() const noexcept {
            return class_name == "FUTURE_OPTION" || class_name == "OPTION";
        }
    };

    struct ListFilter {
        std::string classes; ///< 逗号分隔；空表示全部
        std::string exchange;
        std::string product;
        std::string query;
        std::string underlying;
        bool        include_expired = false;
        std::size_t limit           = 5000;
        std::size_t offset          = 0;
    };

    struct OptionProduct {
        std::string exchange;
        std::string product;
        std::string product_name;
        std::string underlying_symbol;
        double      expire_datetime = 0.0;
        int         calls           = 0;
        int         puts            = 0;
    };

    explicit InstrumentCatalog(Config cfg, std::shared_ptr<AuthTokenSource> auth = nullptr);
    ~InstrumentCatalog();

    InstrumentCatalog(const InstrumentCatalog&)            = delete;
    InstrumentCatalog& operator=(const InstrumentCatalog&) = delete;

    /// 同步加载本地文件；不存在时尝试 latest.json。失败返回 false，不抛异常。
    bool load_local();
    /// 下载 URL 到配置的 file，再原子重载；失败保留旧目录。
    bool refresh_now();

    /// 启动空闲刷新线程。首次若本地目录为空，会先尝试加载/下载。
    void start_background_refresh();
    void stop();

    bool           loaded() const;
    nlohmann::json status_json() const;

    std::vector<Instrument>    list(const ListFilter& filter) const;
    std::optional<Instrument>  find(std::string_view instrument_id) const;
    std::vector<Instrument>    option_contracts(std::string_view underlying,
                                                std::size_t      limit = 500) const;
    std::vector<OptionProduct> option_products() const;

    static std::string    product_of(std::string_view underlying);
    static nlohmann::json to_json(const Instrument& instrument);

private:
    bool        load_from_file(const std::string& path);
    void        swap_items(std::vector<Instrument> items, const std::string& source);
    bool        download_to_file();
    std::string effective_path() const;
    void        refresh_loop(std::stop_token stop);

    Config                                       cfg_;
    std::shared_ptr<AuthTokenSource>             auth_;
    mutable std::shared_mutex                    mutex_;
    std::vector<Instrument>                      items_;
    std::unordered_map<std::string, std::size_t> index_;
    std::string                                  source_path_;
    std::chrono::system_clock::time_point        loaded_at_{};
    std::size_t                                  active_  = 0;
    std::size_t                                  futures_ = 0;
    std::size_t                                  options_ = 0;
    std::jthread                                 worker_;
    std::mutex                                   worker_mutex_;
    std::condition_variable                      worker_cv_;
};

} // namespace tqsdk
