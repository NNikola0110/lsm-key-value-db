#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lsm/config.hpp"
#include "lsm/manifest.hpp"

namespace lsm {

// Process-wide read-path counters (Sections 4.7 / 8.2). Atomic: many
// readers bump these concurrently (Section 7).
struct ReadStats {
    std::atomic<std::uint64_t> reads_total{0};            // lsm_reads_total
    std::atomic<std::uint64_t> hit_memtable_total{0};     // served from active/immutables
    std::atomic<std::uint64_t> sstables_consulted_total{0};
    std::atomic<std::uint64_t> blooms_checked{0};
    std::atomic<std::uint64_t> blooms_negative{0};  // tables skipped thanks to the Bloom
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> cache_misses{0};
    std::atomic<std::uint64_t> disk_block_reads{0};
};

// Per-lookup trace, printed by the CLI at log_level=debug.
struct GetTrace {
    bool          memtable_hit         = false;
    std::uint32_t immutables_consulted = 0;
    std::uint32_t sstables_consulted   = 0;
    std::uint32_t range_skipped        = 0;
    std::uint32_t blooms_skipped       = 0;
    std::uint32_t block_reads          = 0;   // disk reads for this lookup
    std::uint32_t cache_hits           = 0;
};

// LRU cache for verified data blocks, keyed by (table id, block offset).
// Blocks are checksum-validated before insertion, so a cache hit never
// re-reads or re-verifies (Section 4.4). Thread-safe via one internal mutex.
class BlockCache {
public:
    using BlockPtr = std::shared_ptr<const std::vector<unsigned char>>;

    explicit BlockCache(std::uint64_t capacity_bytes) : capacity_(capacity_bytes) {}

    BlockPtr find(std::uint64_t table_id, std::uint64_t offset);
    void insert(std::uint64_t table_id, std::uint64_t offset, BlockPtr block);

private:
    std::mutex mutex_;
    struct Key {
        std::uint64_t table, offset;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::hash<std::uint64_t>()(k.table * 0x9E3779B97F4A7C15ull ^ k.offset);
        }
    };

    std::list<std::pair<Key, BlockPtr>> lru_;   // front = most recently used
    std::unordered_map<Key, decltype(lru_)::iterator, KeyHash> map_;
    std::uint64_t capacity_;
    std::uint64_t used_ = 0;
};

class TableReader;

// Keeps the number of simultaneously open SSTable files within
// max_open_files by closing the least-recently-used reader (Section 4.4).
// Victims are collected under the pool mutex but closed after releasing it,
// so the pool lock never nests inside a reader's own lock (no ABBA).
class FdPool {
public:
    explicit FdPool(std::uint32_t max_open) : max_open_(max_open == 0 ? 1 : max_open) {}
    void on_open(TableReader* r);    // registers r; may evict the LRU reader
    void touch(TableReader* r);
    void on_close(TableReader* r);
    [[nodiscard]] std::size_t open_count() const;

private:
    mutable std::mutex mutex_;
    std::uint32_t max_open_;
    std::list<TableReader*> lru_;    // front = most recently used
};

// What a single-table lookup returned. `tombstone` hides older versions.
struct TableHit {
    std::string   value;
    std::uint64_t seqno = 0;
    bool          tombstone = false;
};

// Read-side handle for one .sst file (Section 4.2 "TableHandle").
// Lazily opens the file and pins the Bloom filter (and, if
// cache_index_blocks, the parsed index) in memory. A per-reader mutex
// serializes file I/O and lazy loads; concurrent gets on different tables
// proceed in parallel (Section 7).
class TableReader {
public:
    TableReader(std::filesystem::path path, TableMeta meta, const Config& cfg);
    ~TableReader();

    TableReader(const TableReader&) = delete;
    TableReader& operator=(const TableReader&) = delete;

    // Quick open check (4.3 A): footer magic/version only; does not keep
    // the file open. Throws CorruptionDetected on mismatch.
    void validate_footer();

    // Bloom -> index -> block scan. nullopt = key definitely not in this
    // table. Throws CorruptionDetected on checksum failures.
    std::optional<TableHit> get(std::string_view key, BlockCache& cache,
                                FdPool& fds, ReadStats& stats, GetTrace& trace);

    [[nodiscard]] const TableMeta& meta() const noexcept { return meta_; }
    [[nodiscard]] bool file_open() const noexcept { return file_ != nullptr; }
    void close_file();               // blocking close (destructor, validate)
    bool try_close_file();           // non-blocking; used by FdPool eviction

private:
    struct IndexEntry {
        std::string   first_key;
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
    };

    void ensure_open(FdPool& fds);
    std::vector<unsigned char> read_range(std::uint64_t offset, std::uint64_t size);
    void load_footer();
    std::vector<IndexEntry> load_index();
    void load_bloom();
    [[nodiscard]] bool bloom_may_contain(std::string_view key) const;

    std::filesystem::path path_;
    TableMeta meta_;
    const Config& cfg_;

    std::mutex io_mutex_;   // guards file_, lazy loads and all reads
    std::FILE* file_ = nullptr;
    FdPool* pool_ = nullptr;   // set on first open; deregisters in destructor
    bool footer_loaded_ = false;
    std::uint64_t index_off_ = 0, index_size_ = 0;
    std::uint64_t filter_off_ = 0, filter_size_ = 0;

    std::vector<IndexEntry> index_;          // pinned iff cache_index_blocks
    bool index_pinned_ = false;

    std::vector<unsigned char> bloom_bits_;  // always pinned once loaded
    std::uint64_t bloom_num_bits_ = 0;
    std::uint32_t bloom_num_hashes_ = 0;
};

} // namespace lsm
