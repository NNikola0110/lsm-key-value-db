#include "lsm/memtable.hpp"

#include <mutex>

namespace lsm {

void Memtable::apply(std::string_view key, std::string_view value,
                     std::uint64_t seqno, bool tombstone) {
    std::unique_lock lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        bytes_ -= entry_size(it->first, it->second.value);
        it->second.value = std::string(value);
        it->second.seqno = seqno;
        it->second.tombstone = tombstone;
        bytes_ += entry_size(it->first, it->second.value);
        return;
    }
    map_.emplace(std::string(key), MemEntry{std::string(value), seqno, tombstone});
    bytes_ += entry_size(key, value);
}

std::optional<MemEntry> Memtable::find(std::string_view key) const {
    std::shared_lock lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

std::uint64_t Memtable::bytes() const {
    std::shared_lock lock(mutex_);
    return bytes_;
}

std::uint64_t Memtable::entries() const {
    std::shared_lock lock(mutex_);
    return map_.size();
}

} // namespace lsm
