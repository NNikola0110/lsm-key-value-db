#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lsm/sstable.hpp"

namespace lsm {

// Metadata for one live SSTable (Section 3.7).
struct TableMeta {
    std::uint64_t id = 0;
    std::string   file_name;
    std::string   min_key, max_key;
    std::uint64_t min_seqno = 0, max_seqno = 0;
    std::string   created_at;         // ISO 8601 UTC
    std::uint64_t file_size = 0;
    std::uint64_t entries = 0;
    double        bloom_bits_per_key = 0.0;
    std::uint32_t bloom_hashes = 0;
};

// The durable source of truth about which SSTables are live (Section 5.3).
// Tables are kept NEWEST-FIRST both here and in the JSON file — the same
// order readers need (5.11). Updates are atomic: write manifest.json.tmp,
// fsync, rename over the old file. `epoch` increments on every change.
// Keys are stored as JSON strings, so the CLI's UTF-8 keys round-trip; raw
// binary keys would need escaping (out of scope for this course).
class Manifest {
public:
    // Missing file => empty manifest (fresh store). Invalid JSON =>
    // CorruptionDetected naming the file. A leftover .tmp is ignored and
    // removed (5.3 atomicity rules).
    static Manifest load_or_create(std::filesystem::path path);

    // Next SSTable id. Persisted by add_table's save; an id "leaks" if we
    // crash before publishing, which is harmless (the temp file is deleted
    // on startup and the id gets reused).
    [[nodiscard]] std::uint64_t next_id() const noexcept { return next_sst_id_; }

    // Register a freshly published table (prepended: newest first), bump the
    // epoch and atomically rewrite the file.
    void add_table(const TableMeta& meta);

    // Compaction edit (6.7): remove the input tables and add the output (if
    // any) in ONE atomic manifest rewrite. Order stays newest-first by
    // max_seqno. Bumps the epoch once.
    void apply_compaction(const std::vector<std::uint64_t>& removed_ids,
                          const TableMeta* added);

    // Newest -> oldest.
    [[nodiscard]] const std::vector<TableMeta>& tables() const noexcept { return tables_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t max_seqno() const noexcept;
    [[nodiscard]] std::uint64_t total_bytes() const noexcept;

private:
    void save() const;

    std::filesystem::path path_;
    std::uint64_t next_sst_id_ = 1;
    std::uint64_t epoch_ = 0;         // monotonic; +1 per manifest change
    std::vector<TableMeta> tables_;   // newest first
};

} // namespace lsm
