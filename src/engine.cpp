#include "lsm/engine.hpp"

#include "lsm/errors.hpp"

#include <filesystem>
#include <iostream>
#include <utility>

namespace lsm {

namespace fs = std::filesystem;

Engine::Engine(Config config) : config_(std::move(config)) {}

Engine::~Engine() = default;

EngineRecovery Engine::open() {
    std::error_code ec;
    fs::create_directories(config_.data_dir, ec);
    if (ec) {
        throw Error(ErrorCode::IOFailure,
                    "cannot create data dir '" + config_.data_dir + "': " + ec.message());
    }
    active_ = std::make_unique<Memtable>(config_.memtable_size_overhead_bytes_per_entry);
    wal_ = std::make_unique<Wal>(fs::path(config_.data_dir) / "wal",
                                 config_.wal_fsync_every_n,
                                 config_.wal_segment_roll_bytes);

    // Replay applies the same write-path rules as live mutations (2.7),
    // including rotation, so the table set matches what existed pre-crash.
    EngineRecovery recovery;
    recovery.wal = wal_->open([this](bool is_del, std::string_view key,
                                     std::string_view value, std::uint64_t seqno) {
        apply_mutation(key, value, seqno, is_del, /*replaying=*/true);
    });
    recovery.memtable_keys = active_->entries();
    recovery.memtable_bytes = active_->bytes();
    return recovery;
}

void Engine::ensure_open() const {
    if (closed_) {
        throw Error(ErrorCode::StoreClosed, "store is closed");
    }
    if (!wal_) {
        throw Error(ErrorCode::StoreClosed, "store is not open (call open() first)");
    }
}

// Backpressure policy (2.6, Option A): if the active table is full and the
// immutable list is at max_immutable_tables, refuse the write. There is no
// flusher until Section 3, so "blocking" surfaces as a Backpressure error.
void Engine::ensure_write_capacity() {
    if (active_->bytes() >= config_.memtable_max_bytes &&
        immutables_.size() >= config_.max_immutable_tables) {
        std::cerr << "backpressure: immutables=" << immutables_.size()
                  << " (max=" << config_.max_immutable_tables << "); blocking writes\n";
        throw Error(ErrorCode::Backpressure,
                    "memtable full and immutable tables at limit; "
                    "writes resume once flushing exists (Section 3)");
    }
}

std::uint64_t Engine::put(std::string_view key, std::string_view value) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    ensure_write_capacity();
    const std::uint64_t seqno = wal_->append_put(key, value);   // durable first
    apply_mutation(key, value, seqno, /*tombstone=*/false, /*replaying=*/false);
    return seqno;
}

std::uint64_t Engine::remove(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    ensure_write_capacity();
    // Deleting a non-existent key is fine: we still log a DEL record.
    const std::uint64_t seqno = wal_->append_del(key);
    apply_mutation(key, {}, seqno, /*tombstone=*/true, /*replaying=*/false);
    return seqno;
}

std::optional<std::string> Engine::get(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    // Active first, then immutables newest->oldest; first hit wins,
    // tombstone means NOT FOUND (2.4 B).
    if (const MemEntry* e = active_->find(key)) {
        return e->tombstone ? std::nullopt : std::optional<std::string>(e->value);
    }
    for (auto it = immutables_.rbegin(); it != immutables_.rend(); ++it) {
        if (const MemEntry* e = (*it)->find(key)) {
            return e->tombstone ? std::nullopt : std::optional<std::string>(e->value);
        }
    }
    return std::nullopt;
}

void Engine::apply_mutation(std::string_view key, std::string_view value,
                            std::uint64_t seqno, bool tombstone, bool replaying) {
    active_->apply(key, value, seqno, tombstone);
    maybe_rotate(replaying);
}

void Engine::maybe_rotate(bool replaying) {
    if (active_->bytes() < config_.memtable_max_bytes) return;

    if (immutables_.size() >= config_.max_immutable_tables) {
        // Rotation deferred: the active table keeps growing; the next live
        // write is refused by ensure_write_capacity(). Replay cannot block,
        // so it only warns (once).
        if (replaying && !backpressure_warned_) {
            std::cerr << "backpressure: immutables=" << immutables_.size()
                      << " (max=" << config_.max_immutable_tables
                      << "); rotation deferred during replay\n";
            backpressure_warned_ = true;
        }
        return;
    }

    immutables_.push_back(std::move(active_));
    active_ = std::make_unique<Memtable>(config_.memtable_size_overhead_bytes_per_entry);
    flush_requested_ += 1;   // Section 3 will consume this queue
}

void Engine::close() {
    if (closed_) return;
    if (wal_) {
        wal_->close();
    }
    // Immutables may still be in memory at close; that's acceptable until
    // Section 3 — the WAL has everything and replay rebuilds them.
    closed_ = true;
}

WalStats Engine::wal_stats() const {
    ensure_open();
    return wal_->stats();
}

MemtableStats Engine::memtable_stats() const {
    ensure_open();
    MemtableStats s;
    s.active_entries = active_->entries();
    s.active_bytes = active_->bytes();
    s.immutables_count = immutables_.size();
    for (const auto& t : immutables_) {
        s.immutables_bytes_total += t->bytes();
    }
    s.flush_requested = flush_requested_;
    return s;
}

} // namespace lsm
