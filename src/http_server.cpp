#include "lsm/http_server.hpp"

#include "lsm/errors.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kBadSocket = -1;
#endif

#include <cstring>
#include <string>

namespace lsm {

namespace {

void close_socket(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 404: return "Not Found";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

} // namespace

HttpServer::HttpServer(const std::string& addr, HttpHandler handler)
    : handler_(std::move(handler)) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw Error(ErrorCode::IOFailure, "WSAStartup failed");
    }
#endif
    const auto colon = addr.rfind(':');
    if (colon == std::string::npos) {
        throw Error(ErrorCode::InvalidArgument, "http_listen_addr must be host:port: " + addr);
    }
    const std::string host = addr.substr(0, colon);
    const int port = std::stoi(addr.substr(colon + 1));

    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSocket) {
        throw Error(ErrorCode::IOFailure, "cannot create HTTP socket");
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        close_socket(s);
        throw Error(ErrorCode::InvalidArgument, "invalid HTTP listen host: " + host);
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 || ::listen(s, 16) != 0) {
        close_socket(s);
        throw Error(ErrorCode::IOFailure, "cannot bind/listen on " + addr);
    }

    listen_socket_ = static_cast<std::uint64_t>(s);
    thread_ = std::thread([this] { serve_loop(); });
}

void HttpServer::serve_loop() {
    const socket_t listener = static_cast<socket_t>(listen_socket_);
    for (;;) {
        const socket_t client = ::accept(listener, nullptr, nullptr);
        if (client == kBadSocket) {
            return;   // listener closed => shutting down
        }
        char buf[2048];
        const int n = ::recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            // "GET /path HTTP/1.x" — everything else is a 404.
            std::string path = "/";
            if (std::strncmp(buf, "GET ", 4) == 0) {
                const char* start = buf + 4;
                const char* end = std::strchr(start, ' ');
                if (end) path.assign(start, end);
            }
            HttpResponse resp = handler_ ? handler_(path) : HttpResponse{404, "text/plain", "no handler"};
            std::string out = "HTTP/1.0 " + std::to_string(resp.status) + " " +
                              status_text(resp.status) + "\r\n"
                              "Content-Type: " + resp.content_type + "\r\n"
                              "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
                              "Connection: close\r\n\r\n" + resp.body;
            ::send(client, out.data(), static_cast<int>(out.size()), 0);
        }
        close_socket(client);
    }
}

void HttpServer::stop() {
    if (stopped_) return;
    stopped_ = true;
    close_socket(static_cast<socket_t>(listen_socket_));   // unblocks accept()
    if (thread_.joinable()) thread_.join();
#ifdef _WIN32
    WSACleanup();
#endif
}

HttpServer::~HttpServer() {
    stop();
}

} // namespace lsm
