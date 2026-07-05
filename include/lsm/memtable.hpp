#pragma once

#include <cstdint>
#include <map>
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
class Memtable {
public:
    explicit Memtable(std::uint32_t overhead_per_entry) : overhead_(overhead_per_entry) {}

    // Insert or overwrite the entry for key; keeps only the latest version.
    void apply(std::string_view key, std::string_view value,
               std::uint64_t seqno, bool tombstone);

    // nullptr if the key has no entry (a tombstone IS an entry).
    [[nodiscard]] const MemEntry* find(std::string_view key) const;

    using Map = std::map<std::string, MemEntry, std::less<>>;
    // Key-ordered view for the flush path (Section 3): iterate ascending.
    [[nodiscard]] const Map& sorted_entries() const noexcept { return map_; }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t entries() const noexcept { return map_.size(); }

private:
    [[nodiscard]] std::uint64_t entry_size(std::string_view key,
                                           std::string_view value) const noexcept {
        return key.size() + value.size() + overhead_;
    }

    Map map_;  // lexicographic key order
    std::uint64_t bytes_ = 0;
    std::uint32_t overhead_;
};

} // namespace lsm
