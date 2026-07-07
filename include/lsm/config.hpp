#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace lsm {

enum class Compression { Off, Snappy, Lz4 };
enum class LogLevel { Debug, Info, Warn, Error };

std::string_view to_string(Compression compression) noexcept;
std::string_view to_string(LogLevel level) noexcept;
LogLevel log_level_from_string(const std::string& s);   // throws InvalidArgument

// Engine configuration (Section 0.4). All tunables live here with sane defaults.
struct Config {
    std::string   data_dir              = "./data";
    std::uint64_t memtable_max_bytes    = 67108864;   // 64 MiB, target size before flush
    std::uint32_t max_immutable_tables  = 4;          // backpressure threshold (Section 2.6)
    std::uint32_t memtable_size_overhead_bytes_per_entry = 32;  // accounting only (2.5)
    std::uint32_t block_size            = 8192;       // 8 KiB, target SSTable block size
    double        bloom_false_positive  = 0.01;       // target Bloom FP rate
    std::string   sst_dir               = "";         // empty = <data_dir>/sst (Section 3.10)
    std::uint32_t restart_interval      = 16;         // entries per restart point
    std::uint32_t max_build_buffer_mb   = 32;         // advisory cap while building SSTs
    std::uint32_t block_cache_mb        = 64;         // LRU cache for data blocks (4.4)
    bool          cache_index_blocks    = true;       // pin index blocks in memory
    std::uint32_t max_open_files        = 1024;       // FD budget for SSTable readers
    std::uint32_t wal_fsync_every_n     = 1;          // fsync every N writes
    std::uint64_t wal_segment_roll_bytes = 134217728; // 128 MiB, size-based roll
    Compression   compression           = Compression::Off;
    LogLevel      log_level             = LogLevel::Info;
    std::string   manifest_path         = "";         // empty = <data_dir>/manifest.json
    LogLevel      publish_log_level     = LogLevel::Info;  // debug => log every publish

    // Resolution priority (later overrides earlier):
    //   built-in defaults -> config file -> env vars (LSMKV_*).
    // CLI flags are applied by the caller on top of this.
    // Missing file -> defaults. Unknown keys -> warning, ignored.
    static Config load(const std::filesystem::path& config_path);

    void apply_env();                       // LSMKV_* overrides
    void print(std::ostream& os) const;     // resolved values, key=value per line

    [[nodiscard]] std::filesystem::path resolved_sst_dir() const {
        return sst_dir.empty() ? std::filesystem::path(data_dir) / "sst"
                               : std::filesystem::path(sst_dir);
    }

    [[nodiscard]] std::filesystem::path resolved_manifest_path() const {
        return manifest_path.empty() ? std::filesystem::path(data_dir) / "manifest.json"
                                     : std::filesystem::path(manifest_path);
    }
};

} // namespace lsm
