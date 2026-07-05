#include "lsm/table_reader.hpp"

#include "lsm/errors.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace lsm {

namespace fs = std::filesystem;

namespace {

// Must match the writer in sstable.cpp exactly.
constexpr std::uint32_t kMagic         = 0x4C535354;  // "LSST"
constexpr std::uint32_t kVersion       = 1;
constexpr std::size_t   kFooterSize    = 8 * 4 + 4 + 4;
constexpr std::uint8_t  kFlagTombstone = 0x01;
constexpr std::size_t   kEntryHeader   = 4 + 4 + 8 + 1;  // key_len, val_len, seqno, flags

std::uint32_t crc32(const unsigned char* data, std::size_t len) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t get_u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::uint64_t get_u64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

std::uint64_t fnv1a(std::string_view s, std::uint64_t seed) {
    std::uint64_t h = 14695981039346656037ull ^ seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

[[noreturn]] void corrupt(const fs::path& path, std::uint64_t offset, const std::string& what) {
    throw Error(ErrorCode::CorruptionDetected,
                path.filename().string() + " @" + std::to_string(offset) + ": " + what);
}

} // namespace

// --- BlockCache -------------------------------------------------------

BlockCache::BlockPtr BlockCache::find(std::uint64_t table_id, std::uint64_t offset) {
    std::lock_guard lock(mutex_);
    const Key key{table_id, offset};
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);   // move to front
    return it->second->second;
}

void BlockCache::insert(std::uint64_t table_id, std::uint64_t offset, BlockPtr block) {
    std::lock_guard lock(mutex_);
    const Key key{table_id, offset};
    if (map_.contains(key)) return;
    used_ += block->size();
    lru_.emplace_front(key, std::move(block));
    map_[key] = lru_.begin();
    while (used_ > capacity_ && !lru_.empty()) {
        used_ -= lru_.back().second->size();
        map_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

// --- FdPool -----------------------------------------------------------

void FdPool::on_open(TableReader* r) {
    std::vector<TableReader*> victims;
    {
        std::lock_guard lock(mutex_);
        lru_.push_front(r);
        while (lru_.size() > max_open_) {
            victims.push_back(lru_.back());
            lru_.pop_back();
        }
    }
    // Close victims outside the pool lock, and only with try_lock: the
    // caller holds its own reader's io_mutex_, and a blocking close of a
    // busy victim (whose thread may itself be evicting) could deadlock.
    // A busy victim just stays open — the budget is soft for one round.
    for (TableReader* victim : victims) {
        if (!victim->try_close_file()) {
            std::lock_guard lock(mutex_);
            lru_.push_back(victim);   // still open; keep tracking it
        }
    }
}

void FdPool::touch(TableReader* r) {
    std::lock_guard lock(mutex_);
    auto it = std::find(lru_.begin(), lru_.end(), r);
    if (it != lru_.end()) lru_.splice(lru_.begin(), lru_, it);
}

void FdPool::on_close(TableReader* r) {
    std::lock_guard lock(mutex_);
    lru_.remove(r);
}

std::size_t FdPool::open_count() const {
    std::lock_guard lock(mutex_);
    return lru_.size();
}

// --- TableReader ------------------------------------------------------

TableReader::TableReader(fs::path path, TableMeta meta, const Config& cfg)
    : path_(std::move(path)), meta_(std::move(meta)), cfg_(cfg) {}

TableReader::~TableReader() {
    if (pool_) pool_->on_close(this);   // drop any stale LRU entry
    if (file_) std::fclose(file_);
}

void TableReader::close_file() {
    std::lock_guard lock(io_mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool TableReader::try_close_file() {
    std::unique_lock lock(io_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    return true;
}

void TableReader::ensure_open(FdPool& fds) {
    pool_ = &fds;
    if (file_) {
        fds.touch(this);
        return;
    }
    file_ = std::fopen(path_.string().c_str(), "rb");
    if (!file_) {
        throw Error(ErrorCode::IOFailure, "cannot open SSTable: " + path_.string());
    }
    fds.on_open(this);
    if (!footer_loaded_) {
        load_footer();
    }
}

std::vector<unsigned char> TableReader::read_range(std::uint64_t offset, std::uint64_t size) {
    std::vector<unsigned char> buf(size);
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0 ||
        std::fread(buf.data(), 1, buf.size(), file_) != buf.size()) {
        corrupt(path_, offset, "short read");
    }
    return buf;
}

void TableReader::load_footer() {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path_, ec);
    if (ec || size < kFooterSize) {
        corrupt(path_, 0, "file too small for footer");
    }
    const auto footer = read_range(size - kFooterSize, kFooterSize);
    if (get_u32(footer.data() + 36) != kMagic) {
        corrupt(path_, size - kFooterSize, "bad footer magic");
    }
    if (get_u32(footer.data() + 32) != kVersion) {
        corrupt(path_, size - kFooterSize, "unsupported version");
    }
    index_off_ = get_u64(footer.data());
    index_size_ = get_u64(footer.data() + 8);
    filter_off_ = get_u64(footer.data() + 16);
    filter_size_ = get_u64(footer.data() + 24);
    if (index_off_ + index_size_ > size || filter_off_ + filter_size_ > size ||
        index_size_ < 4 || filter_size_ < 4 + 8 + 4 + 8) {
        corrupt(path_, size - kFooterSize, "footer offsets out of range");
    }
    footer_loaded_ = true;
}

void TableReader::validate_footer() {
    const bool was_open = file_ != nullptr;
    if (!was_open) {
        file_ = std::fopen(path_.string().c_str(), "rb");
        if (!file_) {
            throw Error(ErrorCode::IOFailure, "cannot open SSTable: " + path_.string());
        }
    }
    try {
        if (!footer_loaded_) load_footer();
    } catch (...) {
        if (!was_open) close_file();
        throw;
    }
    if (!was_open) close_file();   // quick check only; stay within the FD budget
}

std::vector<TableReader::IndexEntry> TableReader::load_index() {
    auto block = read_range(index_off_, index_size_);
    if (crc32(block.data(), index_size_ - 4) != get_u32(block.data() + index_size_ - 4)) {
        corrupt(path_, index_off_, "index block checksum mismatch");
    }

    std::vector<IndexEntry> index;
    std::size_t pos = 0;
    const std::size_t end = index_size_ - 4;
    while (pos < end) {
        if (pos + 4 > end) corrupt(path_, index_off_ + pos, "truncated index entry");
        const std::uint32_t key_len = get_u32(block.data() + pos);
        pos += 4;
        if (pos + key_len + 12 > end) corrupt(path_, index_off_ + pos, "truncated index entry");
        IndexEntry e;
        e.first_key.assign(reinterpret_cast<const char*>(block.data() + pos), key_len);
        pos += key_len;
        e.offset = get_u64(block.data() + pos);
        pos += 8;
        e.size = get_u32(block.data() + pos);
        pos += 4;
        index.push_back(std::move(e));
    }
    return index;
}

void TableReader::load_bloom() {
    auto block = read_range(filter_off_, filter_size_);
    if (crc32(block.data(), filter_size_ - 4) != get_u32(block.data() + filter_size_ - 4)) {
        corrupt(path_, filter_off_, "filter block checksum mismatch");
    }
    bloom_num_bits_ = get_u64(block.data());
    bloom_num_hashes_ = get_u32(block.data() + 8);
    const std::uint64_t bit_bytes = (bloom_num_bits_ + 7) / 8;
    if (bloom_num_bits_ == 0 || bloom_num_hashes_ == 0 ||
        20 + bit_bytes + 4 != filter_size_) {
        corrupt(path_, filter_off_, "filter block layout invalid");
    }
    bloom_bits_.assign(block.begin() + 20, block.begin() + 20 + bit_bytes);
}

bool TableReader::bloom_may_contain(std::string_view key) const {
    const std::uint64_t h1 = fnv1a(key, 0);
    const std::uint64_t h2 = fnv1a(key, 0x9E3779B97F4A7C15ull) | 1;
    for (std::uint32_t i = 0; i < bloom_num_hashes_; ++i) {
        const std::uint64_t bit = (h1 + i * h2) % bloom_num_bits_;
        if (!(bloom_bits_[bit / 8] & (1u << (bit % 8)))) return false;
    }
    return true;
}

std::optional<TableHit> TableReader::get(std::string_view key, BlockCache& cache,
                                         FdPool& fds, ReadStats& stats, GetTrace& trace) {
    // 1. Range check — cheap, no I/O needed (manifest metadata).
    if (key < meta_.min_key || key > meta_.max_key) {
        trace.range_skipped += 1;
        return std::nullopt;
    }

    // Serialize file I/O and lazy loads on this table (per-reader lock;
    // lookups on other tables run in parallel). Note close_file() also
    // takes this lock, so FdPool eviction cannot yank the file mid-read.
    std::lock_guard io_lock(io_mutex_);

    ensure_open(fds);

    // 2. Bloom check: "definitely not" skips all further work.
    if (bloom_bits_.empty()) load_bloom();
    stats.blooms_checked += 1;
    if (!bloom_may_contain(key)) {
        stats.blooms_negative += 1;
        trace.blooms_skipped += 1;
        return std::nullopt;
    }

    // 3. Index search: last entry whose first_key <= key.
    std::vector<IndexEntry> transient;
    const std::vector<IndexEntry>* index;
    if (cfg_.cache_index_blocks) {
        if (!index_pinned_) {
            index_ = load_index();
            index_pinned_ = true;
        }
        index = &index_;
    } else {
        transient = load_index();
        index = &transient;
    }

    auto it = std::upper_bound(index->begin(), index->end(), key,
                               [](std::string_view k, const IndexEntry& e) {
                                   return k < e.first_key;
                               });
    if (it == index->begin()) return std::nullopt;   // key sorts before every block
    --it;

    // 4. Fetch the data block (cache first).
    BlockCache::BlockPtr block = cache.find(meta_.id, it->offset);
    if (block) {
        stats.cache_hits += 1;
        trace.cache_hits += 1;
    } else {
        stats.cache_misses += 1;
        stats.disk_block_reads += 1;
        trace.block_reads += 1;
        auto raw = read_range(it->offset, it->size);
        if (it->size < 12) corrupt(path_, it->offset, "data block too small");
        if (crc32(raw.data(), it->size - 4) != get_u32(raw.data() + it->size - 4)) {
            corrupt(path_, it->offset, "data block checksum mismatch");
        }
        block = std::make_shared<const std::vector<unsigned char>>(std::move(raw));
        cache.insert(meta_.id, it->offset, block);
    }

    // 5. In-block search: restart points narrow the scan, then walk forward.
    const unsigned char* data = block->data();
    const std::size_t block_size = block->size();
    const std::uint32_t restart_count = get_u32(data + block_size - 8);
    if (restart_count == 0 || 8ull + 4ull * restart_count > block_size) {
        corrupt(path_, it->offset, "restart trailer invalid");
    }
    const std::size_t restarts_off = block_size - 8 - 4ull * restart_count;
    const std::size_t entries_end = restarts_off;

    auto key_at = [&](std::size_t off) -> std::string_view {
        const std::uint32_t klen = get_u32(data + off);
        return {reinterpret_cast<const char*>(data + off + kEntryHeader), klen};
    };

    // Binary search over restart points: greatest restart whose key <= key.
    std::size_t lo = 0, hi = restart_count;   // [lo, hi)
    while (hi - lo > 1) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const std::uint32_t off = get_u32(data + restarts_off + 4 * mid);
        if (key_at(off) <= key) lo = mid; else hi = mid;
    }
    std::size_t pos = get_u32(data + restarts_off + 4 * lo);

    while (pos < entries_end) {
        if (pos + kEntryHeader > entries_end) corrupt(path_, it->offset + pos, "truncated entry");
        const std::uint32_t klen = get_u32(data + pos);
        const std::uint32_t vlen = get_u32(data + pos + 4);
        const std::uint64_t seqno = get_u64(data + pos + 8);
        const std::uint8_t flags = data[pos + 16];
        if (pos + kEntryHeader + klen + vlen > entries_end) {
            corrupt(path_, it->offset + pos, "entry overruns block");
        }
        const std::string_view entry_key(reinterpret_cast<const char*>(data + pos + kEntryHeader),
                                         klen);
        if (entry_key == key) {
            TableHit hit;
            hit.value.assign(reinterpret_cast<const char*>(data + pos + kEntryHeader + klen), vlen);
            hit.seqno = seqno;
            hit.tombstone = (flags & kFlagTombstone) != 0;
            return hit;
        }
        if (entry_key > key) break;   // sorted: we've passed it
        pos += kEntryHeader + klen + vlen;
    }
    return std::nullopt;
}

} // namespace lsm
