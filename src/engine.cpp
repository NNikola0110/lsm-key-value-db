#include "lsm/engine.hpp"

#include "lsm/errors.hpp"

#include <filesystem>
#include <utility>

namespace lsm {

namespace fs = std::filesystem;

Engine::Engine(Config config) : config_(std::move(config)) {}

Engine::~Engine() = default;

WalRecoveryReport Engine::open() {
    std::error_code ec;
    fs::create_directories(config_.data_dir, ec);
    if (ec) {
        throw Error(ErrorCode::IOFailure,
                    "cannot create data dir '" + config_.data_dir + "': " + ec.message());
    }
    wal_ = std::make_unique<Wal>(fs::path(config_.data_dir) / "wal",
                                 config_.wal_fsync_every_n,
                                 config_.wal_segment_roll_bytes);
    return wal_->open();
}

void Engine::ensure_open() const {
    if (closed_) {
        throw Error(ErrorCode::StoreClosed, "store is closed");
    }
    if (!wal_) {
        throw Error(ErrorCode::StoreClosed, "store is not open (call open() first)");
    }
}

std::uint64_t Engine::put(std::string_view key, std::string_view value) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    return wal_->append_put(key, value);
}

std::optional<std::string> Engine::get(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    throw Error(ErrorCode::NotImplemented, "Get not implemented yet (Section 2: Memtable)");
}

std::uint64_t Engine::remove(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    // Deleting a non-existent key is fine: we still log a DEL record.
    return wal_->append_del(key);
}

void Engine::close() {
    if (closed_) return;
    if (wal_) {
        wal_->close();
    }
    closed_ = true;
}

WalStats Engine::wal_stats() const {
    ensure_open();
    return wal_->stats();
}

} // namespace lsm
