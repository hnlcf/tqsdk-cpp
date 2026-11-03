#include "tqsdk/gateway/ctp/ctp_md_gateway.h"

#include "tqsdk/gateway/ctp/ctp_convert.h"
#include "tqsdk/util/spawn_logged.h"

#include <filesystem>

namespace asio = boost::asio;

namespace tqsdk {

using namespace ctp;

CtpMdGateway::CtpMdGateway(asio::any_io_executor ex, CtpConfig cfg)
    : ex_(std::move(ex)),
      cfg_(std::move(cfg)),
      ch_(ex_),
      state_(std::make_shared<MdState>(ex_)) {}

CtpMdGateway::~CtpMdGateway() {
    stop_impl();
}

void CtpMdGateway::install_callbacks() {
    auto cb                = std::make_shared<MdCallbacks>();
    cb->front_connected    = [this]() { handle_front_connected(); };
    cb->front_disconnected = [this](int reason) { handle_front_disconnected(reason); };
    cb->login              = [this](const CThostFtdcRspUserLoginField& r,
                                    const CThostFtdcRspInfoField*      info,
                                    int                                req,
                                    bool last) { handle_login(r, info, req, last); };
    cb->sub_market         = [this](const CThostFtdcSpecificInstrumentField* spec,
                                    const CThostFtdcRspInfoField*            info,
                                    int                                      req,
                                    bool last) { handle_sub_market(spec, info, req, last); };
    cb->depth              = [this](const CThostFtdcDepthMarketDataField& d) { handle_depth(d); };
    {
        std::lock_guard<std::mutex> lock(state_->cb_mu);
        state_->callbacks = std::move(cb);
    }
}

asio::awaitable<void> CtpMdGateway::start() {
    if (started_ || stopped_) {
        co_return;
    }
    started_ = true;
    if (cfg_.md_front.empty()) {
        throw std::runtime_error("CTP md_front is empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(cfg_.flow_path, ec);

    install_callbacks();
    state_->alive.store(true, std::memory_order_release);
    spi_ = std::make_unique<CtpMdSpi>(state_);
    api_ = CThostFtdcMdApi::CreateFtdcMdApi(cfg_.flow_path.c_str());
    if (api_ == nullptr) {
        throw std::runtime_error("CreateFtdcMdApi failed");
    }
    api_->RegisterSpi(spi_.get());
    api_->RegisterFront(const_cast<char*>(cfg_.md_front.c_str()));
    api_->Init();
    SPDLOG_INFO("[CtpMd] connecting to {}", cfg_.md_front);
    co_return;
}

void CtpMdGateway::stop_impl() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    state_->alive.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state_->cb_mu);
        state_->callbacks.reset();
    }
    state_->queue->stop();
    if (api_ != nullptr) {
        api_->RegisterSpi(nullptr);
        api_->Release();
        api_ = nullptr;
    }
    ch_.close();
}

asio::awaitable<void> CtpMdGateway::stop() {
    stop_impl();
    co_return;
}

void CtpMdGateway::emit(MarketDataEvent ev) {
    if (stopped_) {
        return;
    }
    co_spawn_logged(
        ex_,
        [this, e = std::move(ev)]() mutable -> asio::awaitable<void> {
            try {
                co_await ch_.async_send(std::move(e));
            } catch (const std::exception& ex) {
                SPDLOG_DEBUG("[CtpMd] emit after close: {}", ex.what());
            }
        },
        "CtpMd.emit");
}

void CtpMdGateway::resubscribe_desired() {
    if (!logged_in_ || api_ == nullptr || desired_.empty()) {
        return;
    }
    std::vector<char*> ptrs;
    ptrs.reserve(desired_.size());
    for (const auto& [sym, refs] : desired_) {
        if (refs > 0) {
            ptrs.push_back(const_cast<char*>(sym.c_str()));
        }
    }
    if (ptrs.empty()) {
        return;
    }
    int ret = api_->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    if (ret != 0) {
        emit(MarketDataEvent{MarketDataEvent::Kind::Error,
                             nlohmann::json{},
                             "CTP SubscribeMarketData ret=" + std::to_string(ret)});
    }
}

void CtpMdGateway::handle_front_connected() {
    if (stopped_ || api_ == nullptr) {
        return;
    }
    SPDLOG_INFO("[CtpMd] front connected, login as {}", cfg_.user_id);
    CThostFtdcReqUserLoginField req{};
    set_field(req.BrokerID, sizeof(req.BrokerID), cfg_.broker_id);
    set_field(req.UserID, sizeof(req.UserID), cfg_.user_id);
    set_field(req.Password, sizeof(req.Password), cfg_.password);
    int ret = api_->ReqUserLogin(&req, 1);
    if (ret != 0) {
        emit(MarketDataEvent{MarketDataEvent::Kind::Error,
                             nlohmann::json{},
                             "CTP ReqUserLogin ret=" + std::to_string(ret)});
    }
}

void CtpMdGateway::handle_front_disconnected(int reason) {
    logged_in_ = false;
    if (stopped_) {
        return;
    }
    SPDLOG_WARN("[CtpMd] front disconnected reason={}", reason);
    emit(MarketDataEvent{MarketDataEvent::Kind::Disconnected,
                         nlohmann::json{},
                         "CTP md disconnected reason=" + std::to_string(reason)});
}

void CtpMdGateway::handle_login(const CThostFtdcRspUserLoginField& rsp,
                                const CThostFtdcRspInfoField*      info,
                                int                                request_id,
                                bool                               is_last) {
    (void)rsp;
    (void)request_id;
    (void)is_last;
    if (stopped_) {
        return;
    }
    if (info != nullptr && info->ErrorID != 0) {
        SPDLOG_WARN("[CtpMd] login failed: {} ({})", cstr(info->ErrorMsg), info->ErrorID);
        emit(MarketDataEvent{MarketDataEvent::Kind::Error,
                             nlohmann::json{},
                             "CTP md login failed: " + cstr(info->ErrorMsg)});
        return;
    }
    logged_in_ = true;
    SPDLOG_INFO("[CtpMd] login ok");
    resubscribe_desired();
}

void CtpMdGateway::handle_sub_market(const CThostFtdcSpecificInstrumentField* spec,
                                     const CThostFtdcRspInfoField*            info,
                                     int                                      request_id,
                                     bool                                     is_last) {
    (void)request_id;
    (void)is_last;
    if (stopped_) {
        return;
    }
    if (info != nullptr && info->ErrorID != 0) {
        emit(MarketDataEvent{MarketDataEvent::Kind::Error,
                             nlohmann::json{},
                             std::string{"CTP subscribe "} +
                                 (spec ? cstr(spec->InstrumentID) : "") +
                                 " failed: " + cstr(info->ErrorMsg)});
    }
}

void CtpMdGateway::handle_depth(const CThostFtdcDepthMarketDataField& depth) {
    if (stopped_ || !logged_in_) {
        return;
    }
    const std::string symbol = cstr(depth.InstrumentID);
    auto              it     = desired_.find(symbol);
    if (it == desired_.end() || it->second <= 0) {
        return;
    }
    nlohmann::json diff{{"quotes", {{symbol, depth_json(depth)}}}};
    emit(MarketDataEvent{MarketDataEvent::Kind::Diff, std::move(diff), {}});
}

asio::awaitable<void> CtpMdGateway::subscribe(const std::vector<std::string>& symbols) {
    bool changed = false;
    for (const auto& s : symbols) {
        if (!s.empty() && desired_[s]++ == 0) {
            changed = true;
        }
    }
    if (changed) {
        resubscribe_desired();
    }
    co_return;
}

asio::awaitable<void>
CtpMdGateway::release(const std::vector<std::string>&                     symbols,
                      const std::vector<std::pair<std::string, int64_t>>& klines) {
    (void)klines;
    // 先复制合约名：desired_ erase 后原 key 的 c_str 会失效。
    std::vector<std::string> unsub_names;
    for (const auto& s : symbols) {
        auto it = desired_.find(s);
        if (it == desired_.end()) {
            continue;
        }
        if (--it->second <= 0) {
            unsub_names.push_back(it->first);
            desired_.erase(it);
        }
    }
    std::vector<char*> unsub;
    unsub.reserve(unsub_names.size());
    for (auto& s : unsub_names) {
        unsub.push_back(s.data());
    }
    if (!unsub.empty() && logged_in_ && api_ != nullptr) {
        int ret = api_->UnSubscribeMarketData(unsub.data(), static_cast<int>(unsub.size()));
        if (ret != 0) {
            emit(MarketDataEvent{MarketDataEvent::Kind::Error,
                                 nlohmann::json{},
                                 "CTP UnSubscribeMarketData ret=" + std::to_string(ret)});
        }
    }
    co_return;
}

} // namespace tqsdk
