#include "tqsdk/ipc/uds_channel.h"

namespace tqsdk {
namespace ipc {

std::unique_ptr<UdsChannel> UdsChannel::connect(asio::io_context& io, const std::string& path) {
    Socket s(io);
    s.connect(asio::local::stream_protocol::endpoint(path));
    return std::make_unique<UdsChannel>(std::move(s));
}

} // namespace ipc
} // namespace tqsdk
