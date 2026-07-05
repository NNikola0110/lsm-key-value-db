#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/compaction.hpp"
#include "lsm/config.hpp"
#include "lsm/manifest.hpp"
#include "lsm/memtable.hpp"
#include "lsm/sstable.hpp"
#include "lsm/table_reader.hpp"
#include "lsm/wal.hpp"

namespace lsm {

// An immutable snapshot of everything a reader should see (Section 5.4).
// Published by an atomic pointer swap; shared_ptr refcounts keep a Version
// (and the tables it references) alive while any reader still uses it.
struct Version {
    std::uint64_t id    = 0;   // in-memory publish counter ("version_id")
    std::uint64_t epoch = 0;   // manifest epoch at publish time
    std::shared_ptr<Memtable> active;
    std::vector<std::shared_ptr<Memtable>> immutables;    // newest -> oldest
    std::vector<std::shared_ptr<TableReader>> tables;     // newest -> oldest
};

// What Engine::open() found: WAL replay results plus the rebuilt memtable
// state (Section 2.7 recovery summary) and version info (5.6).
struct EngineRecovery {
    WalRecoveryReport wal;
    std::uint64_t memtable_keys  = 0;   // entries in the active memtable
    std::uint64_t memtable_bytes = 0;
    std::uint64_t sst_count      = 0;   // live tables from the manifest
    std::uint64_t tmp_files_removed = 0; // leftover *.sst.tmp cleaned up (3.9)
    std::uint64_t epoch          = 0;   // manifest epoch after load
    std::uint64_t version_published = 0; // id of the startup Version
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
//
// As of Section 5, every change to the table set (rotation, flush) builds a
// fresh immutable Version and publishes it with a pointer swap; Get grabs
// the current Version once and only reads objects inside it (5.7).
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
    // result per table written; empty when nothing was pending. May kick
    // compaction afterwards if l0_compaction_trigger is exceeded (6.8).
    std::vector<SstBuildResult> flush_pending();

    // Run one compaction job (Section 6). inputs: explicit table ids
    // (--files) or nullopt to consult the picker. Returns nullopt when no
    // set is available. Throws InvalidArgument for bad manual sets.
    std::optional<CompactionJobResult>
    run_compaction(const std::optional<std::vector<std::uint64_t>>& inputs);

    [[nodiscard]] bool compaction_paused() const;
    void set_compaction_paused(bool paused);   // flag file, survives restarts

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] WalStats wal_stats() const;
    [[nodiscard]] MemtableStats memtable_stats() const;
    [[nodiscard]] SstStats sst_stats() const;
    [[nodiscard]] const Manifest& manifest() const { return manifest_; }
    [[nodiscard]] const ReadStats& read_stats() const noexcept { return read_stats_; }
    [[nodiscard]] std::size_t sstables_open() const noexcept { return fd_pool_->open_count(); }
    [[nodiscard]] std::shared_ptr<const Version> current_version() const noexcept {
        return current_version_;
    }

private:
    void ensure_open() const;
    void ensure_write_capacity();          // throws Backpressure when full (2.6)
    void apply_mutation(std::string_view key, std::string_view value,
                        std::uint64_t seqno, bool tombstone, bool replaying);
    void maybe_rotate(bool replaying);
    void publish_version(const char* reason);   // build + atomic swap (5.5)
    void maybe_auto_compact();                  // l0_compaction_trigger (6.8)

    Config config_;
    std::unique_ptr<Wal> wal_;
    std::shared_ptr<Memtable> active_;
    std::vector<std::shared_ptr<Memtable>> immutables_;   // newest first
    Manifest manifest_;
    std::vector<std::shared_ptr<TableReader>> readers_;   // newest first (manifest order)
    std::shared_ptr<const Version> current_version_;
    std::uint64_t version_id_ = 0;
    std::unique_ptr<BlockCache> block_cache_;
    std::unique_ptr<FdPool> fd_pool_;
    ReadStats read_stats_;
    std::uint64_t flush_requested_ = 0;
    bool backpressure_warned_ = false;
    bool closed_ = false;
};

} // namespace lsm
