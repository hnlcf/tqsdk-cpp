#include "tqsdk/instrument/instrument_catalog.h"

#include "tqsdk/auth/tq_auth.h"

#include "tqsdk/util/http_download.h"
#include "tqsdk/util/json_value.h"

#include <fstream>

namespace tqsdk {

namespace {

constexpr std::size_t kMaxListLimit = 20'000;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

InstrumentCatalog::InstrumentCatalog(Config cfg, std::shared_ptr<TqAuth> auth)
    : cfg_(std::move(cfg)),
      auth_(std::move(auth)) {}

InstrumentCatalog::~InstrumentCatalog() {
    try {
        stop();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Catalog] destructor stop failed: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[Catalog] destructor stop failed: unknown exception");
    }
}

std::string InstrumentCatalog::product_of(std::string_view underlying) {
    const auto dot = underlying.find('.');
    if (dot == std::string_view::npos) {
        return std::string{underlying};
    }
    std::string rest{underlying.substr(dot + 1)};
    auto        end = rest.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(rest[end - 1]))) {
        --end;
    }
    // CFFEX 期权标的是 CSI.000300 / SSE.000016 这类指数，不能把纯数字尾部剥掉。
    if (end == 0 || end == rest.size()) {
        return std::string{underlying};
    }
    return std::string{underlying.substr(0, dot)} + "." + rest.substr(0, end);
}

nlohmann::json InstrumentCatalog::to_json(const Instrument& i) {
    std::string product = i.underlying_product;
    if (product.empty()) {
        product = product_of(i.underlying_symbol.empty() ? i.instrument_id : i.underlying_symbol);
    }
    return nlohmann::json{
        {"instrument_id", i.instrument_id},
        {"class", i.class_name},
        {"exchange_id", i.exchange_id},
        {"ins_id", i.ins_id},
        {"name", i.name},
        {"product_id", i.product_id},
        {"product", product},
        {"product_name", i.product_short_name},
        {"underlying_product", i.underlying_product},
        {"underlying_symbol", i.underlying_symbol},
        {"option_class", i.option_class},
        {"strike_price", i.strike_price},
        {"last_price", i.last_price},
        {"pre_volume", i.pre_volume},
        {"open_interest", i.open_interest},
        {"volume_multiple", i.volume_multiple},
        {"price_tick", i.price_tick},
        {"price_decs", i.price_decs},
        {"delivery_year", i.delivery_year},
        {"delivery_month", i.delivery_month},
        {"expire_datetime", i.expire_datetime},
        {"expired", i.expired},
    };
}

bool InstrumentCatalog::load_from_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        SPDLOG_WARN("[Catalog] cannot open {}", path);
        return false;
    }

    nlohmann::json doc;
    try {
        input >> doc;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[Catalog] parse {} failed: {}", path, e.what());
        return false;
    }
    if (!doc.is_object()) {
        SPDLOG_ERROR("[Catalog] {} is not a JSON object", path);
        return false;
    }

    std::vector<Instrument> items;
    items.reserve(doc.size());
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        const auto& value = it.value();
        if (!value.is_object()) {
            continue;
        }
        Instrument i;
        i.instrument_id = json_value::string_or(value, "instrument_id", it.key());
        if (i.instrument_id.empty()) {
            i.instrument_id = it.key();
        }
        i.exchange_id        = json_value::string_or(value, "exchange_id", std::string{});
        i.ins_id             = json_value::string_or(value, "ins_id", std::string{});
        i.name               = json_value::string_or(value, "ins_name", std::string{});
        i.class_name         = json_value::string_or(value, "class", std::string{});
        i.product_id         = json_value::string_or(value, "product_id", std::string{});
        i.product_short_name = json_value::string_or(value, "product_short_name", i.name);
        i.underlying_product = json_value::string_or(value, "underlying_product", std::string{});
        i.underlying_symbol  = json_value::string_or(value, "underlying_symbol", std::string{});
        i.option_class       = json_value::string_or(value, "option_class", std::string{});
        i.strike_price       = json_value::number_or(value, "strike_price");
        i.last_price         = json_value::number_or(value, "last_price");
        i.pre_volume         = json_value::number_or(value, "pre_volume");
        i.open_interest      = json_value::number_or(value, "open_interest");
        i.volume_multiple    = json_value::number_or(value, "volume_multiple");
        i.price_tick         = json_value::number_or(value, "price_tick");
        i.price_decs = static_cast<int>(json_value::integer_or(value, "price_decs", i.price_decs));
        i.delivery_year =
            static_cast<int>(json_value::integer_or(value, "delivery_year", i.delivery_year));
        i.delivery_month =
            static_cast<int>(json_value::integer_or(value, "delivery_month", i.delivery_month));
        i.expire_datetime = json_value::number_or(value, "expire_datetime");
        i.expired         = json_value::boolean_or(value, "expired", false);
        if (i.option_class.empty() && i.is_option()) {
            // 兼容部分历史 symbol 只写了 option_class；若缺失，从 ins_id 末段推断。
            if (i.ins_id.find("-P-") != std::string::npos ||
                i.ins_id.find('P') != std::string::npos) {
                i.option_class = "PUT";
            } else if (i.ins_id.find("-C-") != std::string::npos) {
                i.option_class = "CALL";
            }
        }
        items.push_back(std::move(i));
    }

    if (items.empty()) {
        SPDLOG_WARN("[Catalog] {} contains no instruments", path);
        return false;
    }
    swap_items(std::move(items), path);
    return true;
}

void InstrumentCatalog::swap_items(std::vector<Instrument> items, const std::string& source) {
    std::unordered_map<std::string, std::size_t> index;
    index.reserve(items.size());
    const std::size_t total  = items.size();
    std::size_t       active = 0, futures = 0, options = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        index[items[i].instrument_id] = i;
        if (!items[i].expired) {
            ++active;
        }
        if (items[i].class_name == "FUTURE" || items[i].class_name == "FUTURE_CONT" ||
            items[i].class_name == "FUTURE_INDEX") {
            ++futures;
        }
        if (items[i].is_option()) {
            ++options;
        }
    }

    {
        std::unique_lock lock(mutex_);
        items_       = std::move(items);
        index_       = std::move(index);
        source_path_ = source;
        loaded_at_   = std::chrono::system_clock::now();
        active_      = active;
        futures_     = futures;
        options_     = options;
    }
    SPDLOG_INFO("[Catalog] loaded {} instruments from {} (active={}, futures={}, options={})",
                total,
                source,
                active,
                futures,
                options);
}

bool InstrumentCatalog::load_local() {
    // 显式配置的文件存在但损坏时，不回退到 symbol/latest：避免把另一个
    // 未知版本的目录当成当前配置，也避免覆盖仍可用的旧快照。
    if (!cfg_.file.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(cfg_.file, ec) && !ec) {
            return load_from_file(cfg_.file);
        }
    }

    for (const char* fallback : {"symbol.json", "latest.json"}) {
        std::error_code ec;
        if (std::filesystem::exists(fallback, ec) && !ec && load_from_file(fallback)) {
            return true;
        }
    }
    SPDLOG_WARN("[Catalog] no local instrument file found (tried {})", cfg_.file);
    return false;
}

bool InstrumentCatalog::download_to_file() {
    if (cfg_.url.empty()) {
        return false;
    }
    try {
        SPDLOG_INFO("[Catalog] downloading {}", cfg_.url);
        util::HttpHeaders headers;
        std::string       user_agent = "tqsdk-node/0.1 instrument-catalog";
        if (auth_) {
            user_agent               = auth_->user_agent();
            const auto authorization = auth_->authorization_header();
            if (!authorization.empty()) {
                headers.emplace("Authorization", authorization);
            } else {
                SPDLOG_WARN("[Catalog] TqAuth unavailable; downloading without bearer token");
            }
        }
        std::string body = util::http_get(cfg_.url, user_agent, std::chrono::seconds(120), headers);
        // 下载必须先在内存中完成 JSON 校验，再原子替换；否则网络截断会把
        // 旧的可用 symbol.json 覆盖成半截文件。
        if (body.empty()) {
            throw std::runtime_error("empty response");
        }
        const auto parsed = nlohmann::json::parse(body);
        if (!parsed.is_object() || parsed.empty()) {
            throw std::runtime_error("response is not a non-empty JSON object");
        }
        const auto tmp = cfg_.file + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                SPDLOG_WARN("[Catalog] cannot write {}", tmp);
                return false;
            }
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        }
        std::error_code ec;
        std::filesystem::rename(tmp, cfg_.file, ec);
        if (ec) {
            std::filesystem::remove(cfg_.file, ec);
            ec.clear();
            std::filesystem::rename(tmp, cfg_.file, ec);
        }
        if (ec) {
            SPDLOG_WARN("[Catalog] rename {} failed: {}", cfg_.file, ec.message());
            return false;
        }
        SPDLOG_INFO("[Catalog] downloaded {} bytes to {}", body.size(), cfg_.file);
        return true;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[Catalog] download failed: {}", e.what());
        return false;
    }
}

bool InstrumentCatalog::refresh_now() {
    if (!download_to_file()) {
        return false;
    }
    return load_from_file(cfg_.file);
}

void InstrumentCatalog::start_background_refresh() {
    if (!cfg_.enabled) {
        return;
    }
    stop();
    worker_ = std::jthread([this](std::stop_token stop) { refresh_loop(stop); });
}

void InstrumentCatalog::stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        {
            std::lock_guard lock(worker_mutex_);
        }
        worker_cv_.notify_all();
        worker_.join();
    }
}

void InstrumentCatalog::refresh_loop(std::stop_token stop) {
    bool had_local = loaded();
    if (!had_local) {
        if (!load_local() && !stop.stop_requested()) {
            refresh_now();
        }
        had_local = loaded();
    }

    std::unique_lock lock(worker_mutex_);
    if (had_local && !stop.stop_requested()) {
        // 本地目录已可用：先让进程稳定运行，再在空闲窗口尝试下载。
        worker_cv_.wait_for(lock, cfg_.initial_delay, [&] { return stop.stop_requested(); });
        if (!stop.stop_requested()) {
            lock.unlock();
            refresh_now();
            lock.lock();
        }
    }
    while (!stop.stop_requested()) {
        worker_cv_.wait_for(lock, cfg_.refresh_interval, [&] { return stop.stop_requested(); });
        if (stop.stop_requested()) {
            break;
        }
        lock.unlock();
        refresh_now();
        lock.lock();
    }
}

bool InstrumentCatalog::loaded() const {
    std::shared_lock lock(mutex_);
    return !items_.empty();
}

std::optional<InstrumentCatalog::Instrument>
InstrumentCatalog::find(std::string_view instrument_id) const {
    std::shared_lock lock(mutex_);
    auto             it = index_.find(std::string{instrument_id});
    if (it == index_.end()) {
        return std::nullopt;
    }
    return items_[it->second];
}

std::vector<InstrumentCatalog::Instrument> InstrumentCatalog::list(const ListFilter& filter) const {
    std::set<std::string> classes;
    {
        std::stringstream ss(filter.classes);
        std::string       item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                classes.insert(item);
            }
        }
    }
    const auto              query = lower_ascii(filter.query);
    std::vector<Instrument> out;
    {
        std::shared_lock lock(mutex_);
        out.reserve(std::min(filter.limit + filter.offset, items_.size()));
        for (const auto& i : items_) {
            if (!filter.include_expired && i.expired) {
                continue;
            }
            if (!classes.empty() && !classes.contains(i.class_name)) {
                continue;
            }
            if (!filter.exchange.empty() && i.exchange_id != filter.exchange) {
                continue;
            }
            if (!filter.product.empty()) {
                const auto product =
                    i.underlying_product.empty()
                        ? product_of(i.underlying_symbol.empty() ? i.instrument_id
                                                                 : i.underlying_symbol)
                        : i.underlying_product;
                if (product != filter.product && i.product_id != filter.product) {
                    continue;
                }
            }
            if (!filter.underlying.empty() && i.underlying_symbol != filter.underlying) {
                continue;
            }
            if (!query.empty()) {
                const auto haystack = lower_ascii(i.instrument_id + " " + i.name + " " +
                                                  i.product_short_name + " " + i.ins_id);
                if (haystack.find(query) == std::string::npos) {
                    continue;
                }
            }
            out.push_back(i);
        }
    }
    std::sort(out.begin(), out.end(), [](const Instrument& a, const Instrument& b) {
        return std::tie(a.class_name, a.underlying_symbol, a.strike_price, a.instrument_id) <
               std::tie(b.class_name, b.underlying_symbol, b.strike_price, b.instrument_id);
    });
    const auto offset = std::min(filter.offset, out.size());
    const auto limit  = std::min(filter.limit == 0 ? kMaxListLimit : filter.limit, kMaxListLimit);
    const auto end    = std::min(out.size(), offset + limit);
    return std::vector<Instrument>(out.begin() + static_cast<std::ptrdiff_t>(offset),
                                   out.begin() + static_cast<std::ptrdiff_t>(end));
}

std::vector<InstrumentCatalog::OptionProduct> InstrumentCatalog::option_products() const {
    std::map<std::string, OptionProduct> grouped;
    std::shared_lock                     lock(mutex_);
    for (const auto& i : items_) {
        if (i.expired || !i.is_option() || i.underlying_symbol.empty()) {
            continue;
        }
        const auto product = product_of(i.underlying_symbol);
        const auto key     = i.exchange_id + "|" + product + "|" + i.underlying_symbol;
        auto&      p       = grouped[key];
        p.exchange         = i.exchange_id;
        p.product          = product;
        if (!i.product_short_name.empty()) {
            p.product_name = i.product_short_name;
        }
        p.underlying_symbol = i.underlying_symbol;
        if (p.expire_datetime == 0.0 ||
            (i.expire_datetime > 0.0 && i.expire_datetime < p.expire_datetime)) {
            p.expire_datetime = i.expire_datetime;
        }
        if (i.option_class == "CALL") {
            ++p.calls;
        } else if (i.option_class == "PUT") {
            ++p.puts;
        }
    }
    std::vector<OptionProduct> out;
    out.reserve(grouped.size());
    for (auto& [_, p] : grouped) {
        if (p.calls > 0 && p.puts > 0) {
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const OptionProduct& a, const OptionProduct& b) {
        return std::tie(a.exchange, a.product, a.underlying_symbol) <
               std::tie(b.exchange, b.product, b.underlying_symbol);
    });
    return out;
}

std::vector<InstrumentCatalog::Instrument>
InstrumentCatalog::option_contracts(std::string_view underlying, std::size_t limit) const {
    std::vector<Instrument> out;
    std::shared_lock        lock(mutex_);
    for (const auto& i : items_) {
        if (i.expired || !i.is_option() || i.underlying_symbol != underlying) {
            continue;
        }
        out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [](const Instrument& a, const Instrument& b) {
        return std::tie(a.strike_price, a.option_class) < std::tie(b.strike_price, b.option_class);
    });
    if (out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

nlohmann::json InstrumentCatalog::status_json() const {
    std::shared_lock lock(mutex_);
    return nlohmann::json{
        {"enabled", cfg_.enabled},
        {"loaded", !items_.empty()},
        {"source", source_path_},
        {"file", cfg_.file},
        {"url", cfg_.url},
        {"total", items_.size()},
        {"active", active_},
        {"futures", futures_},
        {"options", options_},
        {"loaded_at",
         loaded_at_ == std::chrono::system_clock::time_point{}
             ? 0
             : std::chrono::duration_cast<std::chrono::seconds>(loaded_at_.time_since_epoch())
                   .count()},
        {"refresh_interval_sec", cfg_.refresh_interval.count()},
    };
}

} // namespace tqsdk
