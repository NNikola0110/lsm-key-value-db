#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace lsm {

// Minimal HTTP/1.0 GET server for the observability endpoints (Section 8.5):
// /healthz, /readyz, /metrics, /stats. One accept loop on a background
// thread; each request is answered and the connection closed. Not a general
// web server — just enough for curl and Prometheus scrapes.
struct HttpResponse {
    int status = 200;                     // 200 or 503 (8.5)
    std::string content_type = "text/plain";
    std::string body;
};

using HttpHandler = std::function<HttpResponse(const std::string& path)>;

class HttpServer {
public:
    // addr: "host:port" (e.g. "127.0.0.1:9090"). Throws lsm::Error(IOFailure)
    // if the socket cannot be bound.
    HttpServer(const std::string& addr, HttpHandler handler);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void stop();

private:
    void serve_loop();

    HttpHandler handler_;
    std::uint64_t listen_socket_ = ~0ull;   // SOCKET on Windows, fd elsewhere
    std::thread thread_;
    bool stopped_ = false;
};

} // namespace lsm
