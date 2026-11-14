#include "tqsdk/util/machine_id.h"

#if defined(__linux__)
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netpacket/packet.h>
    #include <sys/socket.h>
#endif

namespace tqsdk::util {

#if defined(__linux__)
namespace {

std::string format_mac(const unsigned char* bytes) {
    return fmt::format("{:02X}-{:02X}-{:02X}-{:02X}-{:02X}-{:02X}",
                       bytes[0],
                       bytes[1],
                       bytes[2],
                       bytes[3],
                       bytes[4],
                       bytes[5]);
}

bool all_zero(const unsigned char* bytes) {
    for (int i = 0; i < 6; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

} // namespace
#endif

std::string machine_mac_address() {
#if defined(__linux__)
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        return {};
    }
    std::string result;
    for (const ifaddrs* ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_PACKET) {
            continue;
        }
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto* address = reinterpret_cast<const sockaddr_ll*>(ifa->ifa_addr);
        if (address->sll_halen < 6 || all_zero(address->sll_addr)) {
            continue;
        }
        result = format_mac(address->sll_addr);
        break;
    }
    ::freeifaddrs(interfaces);
    return result;
#else
    return {};
#endif
}

} // namespace tqsdk::util
