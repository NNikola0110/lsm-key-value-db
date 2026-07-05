#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace lsm {

// One row in a memtable (Section 2.3). Tombstones are present entries whose
// value is empty and tombstone flag is set — they hide older values.
struct MemEntry {
    std::string   value;
    std::uint64_t seqno     = 0;   // higher wins when comparing across tables
    bool          tombstone = false;
};

// Ordered in-memory table holding the latest version per key (Section 2).
// Size accounting (2.5): key length + value length + a fixed per-entry
// overhead, summed over entries.
//
// Concurrency (7.4): one writer mutates via apply() under an exclusive lock;
// many readers call find()/bytes()/entries() under a shared lock. Once a
// table becomes immutable, apply() is never called again.
class Memtable {
public:
    explicit Memtable(std::uint32_t overhead_per_entry) : overhead_(overhead_per_entry) {}

    // Insert or overwrite the entry for key; keeps only the latest version.
    void apply(std::string_view key, std::string_view value,
               std::uint64_t seqno, bool tombstone);

    // Copies the found entry out (a pointer would dangle once the shared
    // lock is dropped and the writer overwrites the slot). std::nullopt if
    // the key has no entry (a tombstone IS an entry).
    [[nodiscard]] std::optional<MemEntry> find(std::string_view key) const;

    using Map = std::map<std::string, MemEntry, std::less<>>;
    // Key-ordered view for the flush path (Section 3): iterate ascending.
    // Only safe on immutable tables (no concurrent apply).
    [[nodiscard]] const Map& sorted_entries() const noexcept { return map_; }

    [[nodiscard]] std::uint64_t bytes() const;
    [[nodiscard]] std::uint64_t entries() const;

private:
    [[nodiscard]] std::uint64_t entry_size(std::string_view key,
                                           std::string_view value) const noexcept {
        return key.size() + value.size() + overhead_;
    }

    mutable std::shared_mutex mutex_;
    Map map_;  // lexicographic key order
    std::uint64_t bytes_ = 0;
    std::uint32_t overhead_;
};

} // namespace lsm
