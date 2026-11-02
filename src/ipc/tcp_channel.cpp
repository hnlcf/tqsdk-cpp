#include "tqsdk/ipc/tcp_channel.h"

namespace tqsdk {
namespace ipc {

std::unique_ptr<TcpChannel>
TcpChannel::connect(asio::io_context& io, const std::string& host, uint16_t port) {
    asio::ip::tcp::resolver resolver(io);
    auto results = resolver.resolve(host, std::to_string(port));
    Socket s(io);
    asio::connect(s, results);
    return std::make_unique<TcpChannel>(std::move(s));
}

} // namespace ipc
} // namespace tqsdk
