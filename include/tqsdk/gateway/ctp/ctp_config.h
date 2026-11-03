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
};

} // namespace tqsdk
