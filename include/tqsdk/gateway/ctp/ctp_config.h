#pragma once

#include "tqsdk/pch.h"

namespace tqsdk {

/// CTP 柜台连接参数（[ctp] 段）。
struct CtpConfig {
    std::string broker_id;
    std::string user_id;
    std::string password;
    std::string auth_code;
    std::string app_id;
    std::string td_front;
    std::string md_front;
    /// CTP API 流文件目录；需可写。
    std::string flow_path = "/tmp/tqsdk_ctp";
    /// 报单/撤单之间的最小间隔，防止触发柜台/交易所频率限制。
    std::chrono::milliseconds order_interval{100};
};

} // namespace tqsdk
