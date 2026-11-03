#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::ctp {

/// 跨 gateway 重启保留的 CTP 订单幂等/撤单映射。
///
/// 文件很小，只保存订单身份与本地缓存；完整成交/持仓仍以 CTP 查询为准。
struct StoredOrder {
    std::string    rid;
    std::string    ref;
    std::string    instrument_id;
    std::string    exchange_id;
    std::string    order_sys_id;
    std::string    status;
    std::string    message;
    std::string    trading_day;
    int            front_id    = 0;
    int            session_id  = 0;
    bool           ack_success = false;
    nlohmann::json order_json  = nlohmann::json::object();
};

class CtpOrderStore {
public:
    explicit CtpOrderStore(std::string path);

    /// 读取文件；损坏/不存在时返回空 store，不抛异常。
    void                     load();
    bool                     find(const std::string& rid, StoredOrder& out) const;
    void                     upsert(StoredOrder order);
    void                     set_trading_day(std::string day);
    const std::string&       trading_day() const noexcept { return trading_day_; }
    std::vector<StoredOrder> orders() const;

    /// 原子写：tmp + rename，避免进程崩溃留下半截 JSON。
    void save() const;

private:
    std::string                        path_;
    std::string                        trading_day_;
    std::map<std::string, StoredOrder> orders_;
};

} // namespace tqsdk::ctp
