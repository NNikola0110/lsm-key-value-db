#include "lsm/logging.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>

namespace lsm {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_log_mutex;
std::array<std::atomic<std::uint64_t>, 6> g_error_counts{};
} // namespace

void set_log_level(LogLevel level) { g_level = static_cast<int>(level); }
LogLevel current_log_level() { return static_cast<LogLevel>(g_level.load()); }

std::string ts_rfc3339() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void log_event(LogLevel level, const char* event, LogFields fields) {
    if (static_cast<int>(level) < g_level.load()) return;
    std::ostringstream os;
    os << "ts=" << ts_rfc3339() << " level=" << to_string(level) << " event=" << event;
    for (const auto& [k, v] : fields) {
        os << ' ' << k << '=' << v;
    }
    os << '\n';
    std::lock_guard lock(g_log_mutex);   // whole lines, never interleaved
    std::cerr << os.str();
}

void count_error(ErrorCode code) {
    const auto i = static_cast<std::size_t>(code);
    if (i < g_error_counts.size()) g_error_counts[i] += 1;
}

std::uint64_t error_count(ErrorCode code) {
    const auto i = static_cast<std::size_t>(code);
    return i < g_error_counts.size() ? g_error_counts[i].load() : 0;
}

void crash_point(const char* name) {
    static const char* target = std::getenv("LSMKV_CRASHPOINT");
    if (target && std::strcmp(target, name) == 0) {
        std::fprintf(stderr, "CRASHPOINT %s hit: simulating power cut\n", name);
        std::fflush(nullptr);
        std::_Exit(137);   // no destructors, no flushes — like a power cut
    }
}

} // namespace lsm
