#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::ipc {

/// 一条下游连接对共享行情网关的订阅账本。
///
/// 设计约束：
///   - 只在 gateway control strand 上访问，不加锁；
///   - symbol 引用计数把“显式 quote 订阅”和“K 线依赖的 quote”统一起来；
///   - 返回 Delta，调用方负责在 gateway core strand 上真正 subscribe/release，
///     网络 await 不发生在账本内部。
class GatewaySubscriptionBook {
public:
    struct KlineRequest {
        std::string symbol;
        int64_t     duration_ns = 0;
        int         view_width  = 200;
    };

    struct Delta {
        std::vector<std::string>                     symbols;
        std::vector<std::pair<std::string, int64_t>> klines;
    };

    Delta subscribe(const std::vector<std::string>&  symbols,
                    const std::vector<KlineRequest>& klines);
    Delta unsubscribe(const std::vector<std::string>&  symbols,
                      const std::vector<KlineRequest>& klines);
    /// 连接关闭时一次性归还全部引用。
    Delta release_all();

    bool has_symbol(std::string_view symbol) const;
    bool has_kline(std::string_view symbol, int64_t duration_ns) const;

private:
    static std::string kline_key(std::string_view symbol, int64_t duration_ns);
    void               use_symbol(const std::string& symbol, Delta& delta);
    void               release_symbol_once(const std::string& symbol, Delta& delta);

    std::vector<std::string>                     symbols_;
    std::vector<std::pair<std::string, int64_t>> klines_;
    std::map<std::string, int>                   symbol_use_;
    std::set<std::string>                        explicit_symbols_;
    std::set<std::string>                        kline_keys_;
};

} // namespace tqsdk::ipc
