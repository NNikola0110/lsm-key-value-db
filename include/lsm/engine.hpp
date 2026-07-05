#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/config.hpp"
#include "lsm/memtable.hpp"
#include "lsm/wal.hpp"

namespace lsm {

// What Engine::open() found: WAL replay results plus the rebuilt memtable
// state (Section 2.7 recovery summary).
struct EngineRecovery {
    WalRecoveryReport wal;
    std::uint64_t memtable_keys  = 0;   // entries in the active memtable
    std::uint64_t memtable_bytes = 0;
};

struct MemtableStats {
    std::uint64_t active_entries         = 0;
    std::uint64_t active_bytes           = 0;
    std::uint64_t immutables_count       = 0;
    std::uint64_t immutables_bytes_total = 0;
    std::uint64_t flush_requested        = 0;   // pending flushes (Section 3 consumes these)
};

// The storage engine facade. As of Section 2: Put/Delete append durably to
// the WAL, then update the active memtable; Get reads active -> immutables
// (newest->oldest), honoring tombstones. Reads are memory-only until
// Section 4 (SSTable reader). Opening the engine replays the WAL to rebuild
// the memtable, rotating with the same rules as the write path.
//
// Concurrency model (Section 2.8): single writer, no threads yet — every
// public method assumes one caller. Rotation swaps the active table and
// appends the frozen one to the immutable list.
class Engine {
public:
    explicit Engine(Config config);
    ~Engine();

    // Runs WAL recovery, rebuilding the memtable from replayed records.
    // The report should be printed by the caller (Sections 1.8 / 2.7).
    EngineRecovery open();

    std::uint64_t put(std::string_view key, std::string_view value);  // returns seqNo
    std::optional<std::string> get(std::string_view key);             // nullopt = NOT FOUND
    std::uint64_t remove(std::string_view key);                       // Delete; returns seqNo
    void close();

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] WalStats wal_stats() const;
    [[nodiscard]] MemtableStats memtable_stats() const;

private:
    void ensure_open() const;
    void ensure_write_capacity();          // throws Backpressure when full (2.6)
    void apply_mutation(std::string_view key, std::string_view value,
                        std::uint64_t seqno, bool tombstone, bool replaying);
    void maybe_rotate(bool replaying);

    Config config_;
    std::unique_ptr<Wal> wal_;
    std::unique_ptr<Memtable> active_;
    std::vector<std::unique_ptr<Memtable>> immutables_;  // oldest first
    std::uint64_t flush_requested_ = 0;
    bool backpressure_warned_ = false;
    bool closed_ = false;
};

} // namespace lsm
