#include "lsm/sstable.hpp"

#include "lsm/errors.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace lsm {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMagic       = 0x4C535354;  // "LSST"
constexpr std::uint32_t kVersion     = 1;
constexpr std::size_t   kFooterSize  = 8 * 4 + 4 + 4;  // 4 offsets/sizes + version + magic
constexpr std::uint8_t  kFlagTombstone = 0x01;

// Same CRC-32 (IEEE) as the WAL uses.
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

void put_u32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

void put_u64(std::vector<unsigned char>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
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

// FNV-1a 64-bit; the Bloom filter derives k probes from two hashes
// (h1 + i*h2, the standard double-hashing scheme).
std::uint64_t fnv1a(std::string_view s, std::uint64_t seed) {
    std::uint64_t h = 14695981039346656037ull ^ seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Bloom filter builder (Section 3.5): bitsPerKey ~= -ln(FPR)/(ln 2)^2,
// k ~= ln 2 * bitsPerKey.
struct BloomBuilder {
    std::vector<unsigned char> bits;
    std::uint64_t num_bits = 0;
    std::uint32_t num_hashes = 0;
    double bits_per_key = 0.0;

    BloomBuilder(std::uint64_t num_keys, double fpr) {
        bits_per_key = -std::log(fpr) / (std::log(2.0) * std::log(2.0));
        num_hashes = std::max(1u, static_cast<std::uint32_t>(std::lround(std::log(2.0) * bits_per_key)));
        num_bits = std::max<std::uint64_t>(64, static_cast<std::uint64_t>(
                       std::ceil(static_cast<double>(std::max<std::uint64_t>(num_keys, 1)) * bits_per_key)));
        bits.assign((num_bits + 7) / 8, 0);
    }

    void add(std::string_view key) {
        const std::uint64_t h1 = fnv1a(key, 0);
        const std::uint64_t h2 = fnv1a(key, 0x9E3779B97F4A7C15ull) | 1;  // odd => full cycle
        for (std::uint32_t i = 0; i < num_hashes; ++i) {
            const std::uint64_t bit = (h1 + i * h2) % num_bits;
            bits[bit / 8] |= static_cast<unsigned char>(1u << (bit % 8));
        }
    }
};

void fsync_file(std::FILE* f, const fs::path& path) {
    if (std::fflush(f) != 0) {
        throw Error(ErrorCode::IOFailure, "fflush failed for " + path.string());
    }
#ifdef _WIN32
    if (_commit(_fileno(f)) != 0)
#else
    if (::fsync(::fileno(f)) != 0)
#endif
    {
        throw Error(ErrorCode::IOFailure, "fsync failed for " + path.string());
    }
}

void write_all(std::FILE* f, const std::vector<unsigned char>& buf, const fs::path& path) {
    if (!buf.empty() && std::fwrite(buf.data(), 1, buf.size(), f) != buf.size()) {
        throw Error(ErrorCode::IOFailure, "write failed for " + path.string());
    }
}

std::string sst_file_name(std::uint64_t id) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06llu.sst", static_cast<unsigned long long>(id));
    return buf;
}

} // namespace

SstBuildResult write_sstable(const fs::path& sst_dir, std::uint64_t id,
                             const Memtable& mem, const Config& cfg) {
    if (mem.entries() == 0) {
        throw Error(ErrorCode::InvalidArgument, "refusing to flush an empty memtable");
    }
    if (cfg.compression != Compression::Off) {
        // snappy/lz4 are accepted in config but not linked in this build.
        std::fputs("warning: compression requested but not available; writing uncompressed\n",
                   stderr);
    }

    const std::string name = sst_file_name(id);
    const fs::path tmp_path = sst_dir / (name + ".tmp");
    const fs::path final_path = sst_dir / name;

    std::FILE* f = std::fopen(tmp_path.string().c_str(), "wb");
    if (!f) {
        throw Error(ErrorCode::IOFailure, "cannot create temp SSTable: " + tmp_path.string());
    }

    SstBuildResult result;
    result.id = id;
    result.file_name = name;
    result.entries = mem.entries();
    result.min_seqno = UINT64_MAX;

    BloomBuilder bloom(mem.entries(), cfg.bloom_false_positive);
    result.bloom_bits_per_key = bloom.bits_per_key;
    result.bloom_hashes = bloom.num_hashes;

    try {
        // --- data blocks ---
        struct IndexEntry { std::string first_key; std::uint64_t offset; std::uint32_t size; };
        std::vector<IndexEntry> index;
        std::vector<unsigned char> block;
        std::vector<std::uint32_t> restarts;
        std::string block_first_key;
        std::uint32_t block_entries = 0;
        std::uint64_t file_offset = 0;

        auto finish_block = [&] {
            if (block.empty()) return;
            for (std::uint32_t r : restarts) put_u32(block, r);
            put_u32(block, static_cast<std::uint32_t>(restarts.size()));
            put_u32(block, crc32(block.data(), block.size()));
            write_all(f, block, tmp_path);
            index.push_back({block_first_key, file_offset,
                             static_cast<std::uint32_t>(block.size())});
            file_offset += block.size();
            block.clear();
            restarts.clear();
            block_entries = 0;
        };

        // The memtable already holds only the latest version per key
        // (Section 2.3 design choice), so iterating it in key order yields
        // the required "latest version, tombstones kept" output (3.6).
        for (const auto& [key, entry] : mem.sorted_entries()) {
            if (block.empty()) block_first_key = key;
            if (block_entries % cfg.restart_interval == 0) {
                restarts.push_back(static_cast<std::uint32_t>(block.size()));
            }
            put_u32(block, static_cast<std::uint32_t>(key.size()));
            put_u32(block, static_cast<std::uint32_t>(entry.value.size()));
            put_u64(block, entry.seqno);
            block.push_back(entry.tombstone ? kFlagTombstone : 0);
            block.insert(block.end(), key.begin(), key.end());
            block.insert(block.end(), entry.value.begin(), entry.value.end());
            block_entries += 1;

            bloom.add(key);
            result.max_key = key;
            result.min_seqno = std::min(result.min_seqno, entry.seqno);
            result.max_seqno = std::max(result.max_seqno, entry.seqno);

            if (block.size() >= cfg.block_size) finish_block();
        }
        finish_block();
        result.min_key = mem.sorted_entries().begin()->first;

        // --- index block ---
        const std::uint64_t index_off = file_offset;
        std::vector<unsigned char> index_block;
        for (const auto& e : index) {
            put_u32(index_block, static_cast<std::uint32_t>(e.first_key.size()));
            index_block.insert(index_block.end(), e.first_key.begin(), e.first_key.end());
            put_u64(index_block, e.offset);
            put_u32(index_block, e.size);
        }
        put_u32(index_block, crc32(index_block.data(), index_block.size()));
        write_all(f, index_block, tmp_path);
        file_offset += index_block.size();

        // --- filter (Bloom) block ---
        const std::uint64_t filter_off = file_offset;
        std::vector<unsigned char> filter_block;
        put_u64(filter_block, bloom.num_bits);
        put_u32(filter_block, bloom.num_hashes);
        put_u64(filter_block, result.entries);
        filter_block.insert(filter_block.end(), bloom.bits.begin(), bloom.bits.end());
        put_u32(filter_block, crc32(filter_block.data(), filter_block.size()));
        write_all(f, filter_block, tmp_path);
        file_offset += filter_block.size();

        // --- footer ---
        std::vector<unsigned char> footer;
        put_u64(footer, index_off);
        put_u64(footer, index_block.size());
        put_u64(footer, filter_off);
        put_u64(footer, filter_block.size());
        put_u32(footer, kVersion);
        put_u32(footer, kMagic);
        write_all(f, footer, tmp_path);
        file_offset += footer.size();

        // Never publish until everything is on disk (3.9).
        fsync_file(f, tmp_path);
        std::fclose(f);
        f = nullptr;

        std::error_code ec;
        fs::rename(tmp_path, final_path, ec);   // atomic publish
        if (ec) {
            throw Error(ErrorCode::IOFailure,
                        "cannot rename " + tmp_path.string() + " -> " + final_path.string() +
                            ": " + ec.message());
        }
        result.file_size = file_offset;
    } catch (...) {
        if (f) std::fclose(f);
        std::error_code ec;
        fs::remove(tmp_path, ec);
        throw;
    }

    return result;
}

SstInfo inspect_sstable(const fs::path& file) {
    std::FILE* f = std::fopen(file.string().c_str(), "rb");
    if (!f) {
        throw Error(ErrorCode::IOFailure, "cannot open SSTable: " + file.string());
    }

    auto fail = [&](const std::string& what) -> void {
        std::fclose(f);
        throw Error(ErrorCode::CorruptionDetected, file.filename().string() + ": " + what);
    };
    auto read_at = [&](std::uint64_t off, std::vector<unsigned char>& buf) {
        if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0 ||
            std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
            fail("short read at offset " + std::to_string(off));
        }
    };

    SstInfo info;
    std::error_code ec;
    info.file_size = fs::file_size(file, ec);
    if (ec || info.file_size < kFooterSize) fail("file too small for footer");

    std::vector<unsigned char> footer(kFooterSize);
    read_at(info.file_size - kFooterSize, footer);
    if (get_u32(footer.data() + 36) != kMagic) fail("bad footer magic");
    info.version = get_u32(footer.data() + 32);
    info.index_off = get_u64(footer.data());
    info.index_size = get_u64(footer.data() + 8);
    info.filter_off = get_u64(footer.data() + 16);
    info.filter_size = get_u64(footer.data() + 24);
    if (info.index_off + info.index_size > info.file_size ||
        info.filter_off + info.filter_size > info.file_size ||
        info.index_size < 4 || info.filter_size < 24) {
        fail("footer offsets out of range");
    }

    std::vector<unsigned char> index_block(info.index_size);
    read_at(info.index_off, index_block);
    if (crc32(index_block.data(), info.index_size - 4) !=
        get_u32(index_block.data() + info.index_size - 4)) {
        fail("index block checksum mismatch");
    }
    std::uint64_t total = 0;
    std::size_t pos = 0;
    const std::size_t end = info.index_size - 4;
    while (pos < end) {
        if (pos + 4 > end) fail("truncated index entry");
        const std::uint32_t key_len = get_u32(index_block.data() + pos);
        pos += 4;
        if (pos + key_len + 12 > end) fail("truncated index entry");
        pos += key_len + 8;
        const std::uint32_t block_size = get_u32(index_block.data() + pos);
        pos += 4;
        info.data_blocks += 1;
        total += block_size;
        info.block_min = info.block_min == 0 ? block_size : std::min(info.block_min, block_size);
        info.block_max = std::max(info.block_max, block_size);
    }
    info.block_avg = info.data_blocks ? static_cast<double>(total) / info.data_blocks : 0.0;

    std::vector<unsigned char> filter_block(info.filter_size);
    read_at(info.filter_off, filter_block);
    if (crc32(filter_block.data(), info.filter_size - 4) !=
        get_u32(filter_block.data() + info.filter_size - 4)) {
        fail("filter block checksum mismatch");
    }
    info.bloom_bits = get_u64(filter_block.data());
    info.bloom_hashes = get_u32(filter_block.data() + 8);
    info.bloom_keys = get_u64(filter_block.data() + 12);

    std::fclose(f);
    return info;
}

std::string verify_sstable(const fs::path& file) {
    std::FILE* f = std::fopen(file.string().c_str(), "rb");
    if (!f) return "cannot open file";

    auto read_at = [&](std::uint64_t off, std::vector<unsigned char>& buf) -> bool {
        if (std::fseek(f, static_cast<long>(off), SEEK_SET) != 0) return false;
        return std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    };

    std::string verdict = "OK";
    do {
        std::error_code ec;
        const std::uint64_t size = fs::file_size(file, ec);
        if (ec || size < kFooterSize) { verdict = "file too small for footer"; break; }

        std::vector<unsigned char> footer(kFooterSize);
        if (!read_at(size - kFooterSize, footer)) { verdict = "cannot read footer"; break; }
        if (get_u32(footer.data() + 36) != kMagic) { verdict = "bad footer magic"; break; }
        if (get_u32(footer.data() + 32) != kVersion) { verdict = "unsupported version"; break; }

        const std::uint64_t index_off = get_u64(footer.data());
        const std::uint64_t index_size = get_u64(footer.data() + 8);
        const std::uint64_t filter_off = get_u64(footer.data() + 16);
        const std::uint64_t filter_size = get_u64(footer.data() + 24);
        if (index_off + index_size > size || filter_off + filter_size > size ||
            index_size < 4 || filter_size < 4) {
            verdict = "footer offsets out of range";
            break;
        }

        std::vector<unsigned char> index_block(index_size);
        if (!read_at(index_off, index_block)) { verdict = "cannot read index block"; break; }
        if (crc32(index_block.data(), index_size - 4) != get_u32(index_block.data() + index_size - 4)) {
            verdict = "index block checksum mismatch";
            break;
        }

        std::vector<unsigned char> filter_block(filter_size);
        if (!read_at(filter_off, filter_block)) { verdict = "cannot read filter block"; break; }
        if (crc32(filter_block.data(), filter_size - 4) !=
            get_u32(filter_block.data() + filter_size - 4)) {
            verdict = "filter block checksum mismatch";
            break;
        }

        // Walk the index entries and verify each data block's checksum.
        std::size_t pos = 0;
        const std::size_t entries_end = index_size - 4;
        while (pos < entries_end && verdict == "OK") {
            if (pos + 4 > entries_end) { verdict = "truncated index entry"; break; }
            const std::uint32_t key_len = get_u32(index_block.data() + pos);
            pos += 4;
            if (pos + key_len + 12 > entries_end) { verdict = "truncated index entry"; break; }
            pos += key_len;
            const std::uint64_t block_off = get_u64(index_block.data() + pos);
            pos += 8;
            const std::uint32_t block_size = get_u32(index_block.data() + pos);
            pos += 4;

            if (block_off + block_size > index_off || block_size < 4) {
                verdict = "data block offset out of range at file offset " +
                          std::to_string(block_off);
                break;
            }
            std::vector<unsigned char> data_block(block_size);
            if (!read_at(block_off, data_block)) {
                verdict = "cannot read data block at offset " + std::to_string(block_off);
                break;
            }
            if (crc32(data_block.data(), block_size - 4) !=
                get_u32(data_block.data() + block_size - 4)) {
                verdict = "data block checksum mismatch at offset " + std::to_string(block_off);
                break;
            }
        }
    } while (false);

    std::fclose(f);
    return verdict;
}

} // namespace lsm
