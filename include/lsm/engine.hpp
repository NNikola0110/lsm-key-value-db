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
#include "lsm/table_reader.hpp"
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
// As of Section 4, Get reads all the way to disk: active -> immutables
// (newest->oldest) -> SSTables (newest->oldest), first hit wins, tombstones
// mean NOT FOUND. Each SSTable lookup goes Bloom -> sparse index -> one data
// block, with verified blocks kept in an LRU cache.
class Engine {
public:
    explicit Engine(Config config);
    ~Engine();

    // Cleans *.sst.tmp leftovers, loads + validates the manifest, then runs
    // WAL recovery, rebuilding the memtable from replayed records.
    EngineRecovery open();

    std::uint64_t put(std::string_view key, std::string_view value);  // returns seqNo
    // nullopt = NOT FOUND. Fills *trace (if given) for debug logging.
    std::optional<std::string> get(std::string_view key, GetTrace* trace = nullptr);
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
    [[nodiscard]] const ReadStats& read_stats() const noexcept { return read_stats_; }
    [[nodiscard]] std::size_t sstables_open() const noexcept { return fd_pool_->open_count(); }

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
    std::vector<std::unique_ptr<TableReader>> readers_;   // oldest first (manifest order)
    std::unique_ptr<BlockCache> block_cache_;
    std::unique_ptr<FdPool> fd_pool_;
    ReadStats read_stats_;
    std::uint64_t flush_requested_ = 0;
    bool backpressure_warned_ = false;
    bool closed_ = false;
};

} // namespace lsm
