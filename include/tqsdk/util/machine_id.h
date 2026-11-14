#pragma once

#include "tqsdk/pch.h"

namespace tqsdk::util {

/// 返回本机第一块非 loopback、非全零网卡的 MAC 地址。
/// 格式示例：`AA-BB-CC-DD-EE-FF`。找不到时返回空串。
std::string machine_mac_address();

} // namespace tqsdk::util
