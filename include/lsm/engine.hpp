#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/config.hpp"
#include "lsm/manifest.hpp"
#include "lsm/memtable.hpp"
#include "lsm/sstable.hpp"
#include "lsm/wal.hpp"

namespace lsm {

// What Engine::open() found: WAL replay results plus the rebuilt memtable
// state (Section 2.7 recovery summary).
struct EngineRecovery {
    WalRecoveryReport wal;
    std::uint64_t memtable_keys  = 0;   // entries in the active memtable
    std::uint64_t memtable_bytes = 0;
    std::uint64_t sst_count      = 0;   // live tables from the manifest
    std::uint64_t tmp_files_removed = 0; // leftover *.sst.tmp cleaned up (3.9)
};

struct MemtableStats {
    std::uint64_t active_entries         = 0;
    std::uint64_t active_bytes           = 0;
    std::uint64_t immutables_count       = 0;
    std::uint64_t immutables_bytes_total = 0;
    std::uint64_t flush_requested        = 0;   // rotations so far (cumulative)
};

struct SstStats {
    std::uint64_t sst_count       = 0;
    std::uint64_t sst_total_bytes = 0;
    std::uint64_t newest_id       = 0;   // 0 = none
};

// The storage engine facade. As of Section 3: Put/Delete append durably to
// the WAL, then update the active memtable; a live rotation also rolls the
// WAL segment so cleanup boundaries stay tight. flush_pending writes
// immutables to SSTables (temp + fsync + atomic rename + manifest), then
// deletes WAL segments via the watermark policy (3.8): a segment whose max
// seqNo <= the manifest's max flushed seqNo is fully covered by SSTables.
//
// Reads are still memory-only (active -> immutables, newest->oldest);
// SSTable reads arrive in Section 4. NOTE the consequence: once data is
// flushed and its WAL segments are deleted, Get cannot see it until then.
class Engine {
public:
    explicit Engine(Config config);
    ~Engine();

    // Cleans *.sst.tmp leftovers, loads + validates the manifest, then runs
    // WAL recovery, rebuilding the memtable from replayed records.
    EngineRecovery open();

    std::uint64_t put(std::string_view key, std::string_view value);  // returns seqNo
    std::optional<std::string> get(std::string_view key);             // nullopt = NOT FOUND
    std::uint64_t remove(std::string_view key);                       // Delete; returns seqNo
    void close();

    // Flush all pending immutables, oldest first (Section 3.4). Returns one
    // result per table written; empty when nothing was pending.
    std::vector<SstBuildResult> flush_pending();

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] WalStats wal_stats() const;
    [[nodiscard]] MemtableStats memtable_stats() const;
    [[nodiscard]] SstStats sst_stats() const;
    [[nodiscard]] const Manifest& manifest() const { return manifest_; }

private:
    void ensure_open() const;
    void ensure_write_capacity();          // throws Backpressure when full (2.6)
    void apply_mutation(std::string_view key, std::string_view value,
                        std::uint64_t seqno, bool tombstone, bool replaying);
    void maybe_rotate(bool replaying);

    Config config_;
    std::unique_ptr<Wal> wal_;
    std::unique_ptr<Memtable> active_;
    std::vector<std::unique_ptr<Memtable>> immutables_;   // oldest first
    Manifest manifest_;
    std::uint64_t flush_requested_ = 0;
    bool backpressure_warned_ = false;
    bool closed_ = false;
};

} // namespace lsm
