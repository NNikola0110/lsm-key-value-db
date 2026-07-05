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

// Engine configuration (Section 0.4). All tunables live here with sane defaults.
struct Config {
    std::string   data_dir              = "./data";
    std::uint64_t memtable_max_bytes    = 67108864;   // 64 MiB, target size before flush
    std::uint32_t max_immutable_tables  = 4;          // backpressure threshold (Section 2.6)
    std::uint32_t memtable_size_overhead_bytes_per_entry = 32;  // accounting only (2.5)
    std::uint32_t block_size            = 8192;       // 8 KiB, target SSTable block size
    double        bloom_false_positive  = 0.01;       // target Bloom FP rate
    std::uint32_t wal_fsync_every_n     = 1;          // fsync every N writes
    std::uint64_t wal_segment_roll_bytes = 134217728; // 128 MiB, size-based roll
    Compression   compression           = Compression::Off;
    LogLevel      log_level             = LogLevel::Info;

    // Resolution priority (later overrides earlier):
    //   built-in defaults -> config file -> env vars (LSMKV_*).
    // CLI flags are applied by the caller on top of this.
    // Missing file -> defaults. Unknown keys -> warning, ignored.
    static Config load(const std::filesystem::path& config_path);

    void apply_env();                       // LSMKV_* overrides
    void print(std::ostream& os) const;     // resolved values, key=value per line
};

} // namespace lsm
