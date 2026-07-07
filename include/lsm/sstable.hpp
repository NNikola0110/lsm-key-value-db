#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "lsm/config.hpp"
#include "lsm/memtable.hpp"

namespace lsm {

// Result of building one SSTable — everything the manifest needs (Section 3.7).
struct SstBuildResult {
    std::uint64_t id = 0;
    std::string   file_name;
    std::uint64_t file_size = 0;
    std::string   min_key, max_key;
    std::uint64_t min_seqno = 0, max_seqno = 0;
    std::uint64_t entries = 0;
    double        bloom_bits_per_key = 0.0;
    std::uint32_t bloom_hashes = 0;
};

// SSTable on-disk layout (Section 3.5), all integers little-endian:
//
//   data blocks    sorted (key,value) entries, ~block_size bytes each.
//                  entry = key_len u32 | val_len u32 | seqno u64 | flags u8
//                          (bit0 = tombstone) | key | value
//                  trailer = restart offsets u32[] | restart_count u32 | crc32 u32
//                  (restart point every restart_interval entries; no delta
//                  encoding — full keys are stored, restarts enable in-block
//                  binary search in Section 4)
//   index block    entry = first_key_len u32 | first_key | block_offset u64
//                          | block_size u32, then crc32 u32 over the entries
//   filter block   num_bits u64 | num_hashes u32 | num_keys u64 | bit bytes,
//                  then crc32 u32   (Bloom over all keys in the table)
//   footer (44 B)  index_off u64 | index_size u64 | filter_off u64
//                  | filter_size u64 | version u32 | magic u32 ("LSST")
//
// Files are written as <name>.tmp, fsynced, then atomically renamed
// (Section 3.9). The flush keeps only the latest version per key and KEEPS
// tombstones (Section 3.6) — compaction decides when to drop them.

// Build one SSTable from an immutable memtable. Writes 000000<id>.sst
// under sst_dir via temp + rename. Throws lsm::Error on failure.
SstBuildResult write_sstable(const std::filesystem::path& sst_dir, std::uint64_t id,
                             const Memtable& mem, const Config& cfg);

// Verify footer magic/version and every block checksum (data via index,
// index, filter). Returns "OK" or a description of where corruption was found.
std::string verify_sstable(const std::filesystem::path& file);

// Footer/index/Bloom overview for `lsmkv sst-info` (Section 4.7).
struct SstInfo {
    std::uint64_t file_size = 0;
    std::uint32_t version = 0;
    std::uint64_t index_off = 0, index_size = 0;
    std::uint64_t filter_off = 0, filter_size = 0;
    std::uint64_t data_blocks = 0;      // = index entry count
    std::uint32_t block_min = 0, block_max = 0;
    double        block_avg = 0.0;
    std::uint64_t bloom_bits = 0;
    std::uint32_t bloom_hashes = 0;
    std::uint64_t bloom_keys = 0;
};

// Throws CorruptionDetected on bad magic/checksums.
SstInfo inspect_sstable(const std::filesystem::path& file);

} // namespace lsm
