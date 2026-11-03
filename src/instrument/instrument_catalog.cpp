#include "tqsdk/instrument/instrument_catalog.h"

#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>

#include <fstream>

namespace tqsdk {

namespace beast = boost::beast;
namespace http  = boost::beast::http;
namespace asio  = boost::asio;

namespace {

constexpr std::size_t kMaxListLimit = 20'000;

double json_number(const nlohmann::json& j, const char* key, double fallback = 0.0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_number()) {
        return it->get<double>();
    }
    if (it->is_string()) {
        try {
            return std::stod(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

int json_int(const nlohmann::json& j, const char* key, int fallback = 0) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_number()) {
        return static_cast<int>(it->get<double>());
    }
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

struct UrlParts {
    bool        tls = true;
    std::string host;
    std::string port;
    std::string target;
};

UrlParts parse_url(std::string_view url) {
    UrlParts out;
    if (url.starts_with("https://")) {
        out.tls  = true;
        out.port = "443";
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        out.tls  = false;
        out.port = "80";
        url.remove_prefix(7);
    } else {
        throw std::runtime_error("unsupported instrument URL: " + std::string{url});
    }
    const auto slash = url.find('/');
    auto       host_port = url.substr(0, slash);
    out.target = slash == std::string_view::npos ? "/" : std::string{url.substr(slash)};
    if (const auto colon = host_port.rfind(':'); colon != std::string_view::npos) {
        out.host = std::string{host_port.substr(0, colon)};
        out.port = std::string{host_port.substr(colon + 1)};
    } else {
        out.host = std::string{host_port};
    }
    if (out.host.empty()) {
        throw std::runtime_error("empty host in instrument URL");
    }
    return out;
}

std::string fetch_url(std::string_view url) {
    const auto parsed = parse_url(url);
    asio::io_context io;
    asio::ip::tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(parsed.host, parsed.port);
    http::request<http::empty_body> req{http::verb::get, parsed.target, 11};
    req.set(http::field::host, parsed.host);
    req.set(http::field::user_agent, "tqsdk-node/0.1 instrument-catalog");
    req.set(http::field::accept, "application/json");

    beast::flat_buffer buffer;
    http::response<http::string_body> res;

    if (parsed.tls) {
        asio::ssl::context ctx{asio::ssl::context::tls_client};
        ctx.set_default_verify_paths();
        beast::ssl_stream<beast::tcp_stream> stream(io, ctx);
        stream.set_verify_mode(asio::ssl::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification(parsed.host));
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
            throw std::runtime_error("SSL_set_tlsext_host_name failed");
        }
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(60));
        beast::get_lowest_layer(stream).connect(endpoints);
        stream.handshake(asio::ssl::stream_base::client);
        http::write(stream, req);
        http::read(stream, buffer, res);
        // 不等待 TLS close_notify：部分 CDN 会保持长连接，sync shutdown 可能阻塞
        // 后台刷新线程。socket 析构时会关闭连接。
    } else {
        beast::tcp_stream stream(io);
        stream.expires_after(std::chrono::seconds(60));
        stream.connect(endpoints);
        http::write(stream, req);
        http::read(stream, buffer, res);
    }

    if (res.result() != http::status::ok) {
        throw std::runtime_error("instrument URL returned HTTP " +
                                 std::to_string(res.result_int()));
    }
    return std::move(res.body());
}

} // namespace

InstrumentCatalog::InstrumentCatalog(Config cfg)
    : cfg_(std::move(cfg)) {}

InstrumentCatalog::~InstrumentCatalog() {
    stop();
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
        i.instrument_id = value.value("instrument_id", it.key());
        if (i.instrument_id.empty()) {
            i.instrument_id = it.key();
        }
        i.exchange_id   = value.value("exchange_id", std::string{});
        i.ins_id        = value.value("ins_id", std::string{});
        i.name          = value.value("ins_name", std::string{});
        i.class_name    = value.value("class", std::string{});
        i.product_id    = value.value("product_id", std::string{});
        i.product_short_name = value.value("product_short_name", i.name);
        i.underlying_product = value.value("underlying_product", std::string{});
        i.underlying_symbol  = value.value("underlying_symbol", std::string{});
        i.option_class       = value.value("option_class", std::string{});
        i.strike_price       = json_number(value, "strike_price");
        i.last_price         = json_number(value, "last_price");
        i.pre_volume         = json_number(value, "pre_volume");
        i.open_interest      = json_number(value, "open_interest");
        i.volume_multiple    = json_number(value, "volume_multiple");
        i.price_tick         = json_number(value, "price_tick");
        i.price_decs         = json_int(value, "price_decs");
        i.delivery_year      = json_int(value, "delivery_year");
        i.delivery_month     = json_int(value, "delivery_month");
        i.expire_datetime    = json_number(value, "expire_datetime");
        i.expired            = value.value("expired", false);
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
    const std::size_t total = items.size();
    std::size_t active = 0, futures = 0, options = 0;
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
    std::vector<std::string> candidates;
    if (!cfg_.file.empty()) {
        candidates.push_back(cfg_.file);
    }
    candidates.push_back("symbol.json");
    candidates.push_back("latest.json");
    for (const auto& path : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            if (load_from_file(path)) {
                return true;
            }
        }
    }
    SPDLOG_WARN("[Catalog] no local instrument file found (tried symbol.json/latest.json)");
    return false;
}

bool InstrumentCatalog::download_to_file() {
    if (cfg_.url.empty()) {
        return false;
    }
    try {
        SPDLOG_INFO("[Catalog] downloading {}", cfg_.url);
        std::string body = fetch_url(cfg_.url);
        const auto  tmp  = cfg_.file + ".tmp";
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

std::vector<InstrumentCatalog::Instrument>
InstrumentCatalog::list(const ListFilter& filter) const {
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
    const auto query = lower_ascii(filter.query);
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
                        ? product_of(i.underlying_symbol.empty() ? i.instrument_id : i.underlying_symbol)
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
    std::shared_lock                    lock(mutex_);
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
        if (p.expire_datetime == 0.0 || (i.expire_datetime > 0.0 && i.expire_datetime < p.expire_datetime)) {
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
             : std::chrono::duration_cast<std::chrono::seconds>(
                   loaded_at_.time_since_epoch())
                   .count()},
        {"refresh_interval_sec", cfg_.refresh_interval.count()},
    };
}

} // namespace tqsdk
