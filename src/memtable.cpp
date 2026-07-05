#include "lsm/memtable.hpp"

namespace lsm {

void Memtable::apply(std::string_view key, std::string_view value,
                     std::uint64_t seqno, bool tombstone) {
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

const MemEntry* Memtable::find(std::string_view key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

} // namespace lsm
