#include "lsm/config.hpp"

#include "lsm/errors.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <set>
#include <string>

namespace lsm {

using json = nlohmann::json;

std::string_view to_string(Compression compression) noexcept {
    switch (compression) {
        case Compression::Off:    return "off";
        case Compression::Snappy: return "snappy";
        case Compression::Lz4:    return "lz4";
    }
    return "off";
}

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

namespace {

Compression parse_compression(const std::string& s) {
    if (s == "off")    return Compression::Off;
    if (s == "snappy") return Compression::Snappy;
    if (s == "lz4")    return Compression::Lz4;
    throw Error(ErrorCode::InvalidArgument, "invalid compression value: " + s);
}

bool parse_bool(const std::string& s) {
    if (s == "true" || s == "1")  return true;
    if (s == "false" || s == "0") return false;
    throw Error(ErrorCode::InvalidArgument, "invalid boolean value: " + s);
}

} // namespace

LogLevel log_level_from_string(const std::string& s) {
    if (s == "debug") return LogLevel::Debug;
    if (s == "info")  return LogLevel::Info;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    throw Error(ErrorCode::InvalidArgument, "invalid log_level value: " + s);
}

namespace {

std::optional<std::string> env(const char* name) {
    if (const char* v = std::getenv(name)) {
        return std::string(v);
    }
    return std::nullopt;
}

} // namespace

Config Config::load(const std::filesystem::path& config_path) {
    Config cfg; // built-in defaults

    namespace fs = std::filesystem;
    if (fs::exists(config_path)) {
        std::ifstream in(config_path);
        if (!in) {
            throw Error(ErrorCode::IOFailure,
                        "cannot open config file: " + config_path.string());
        }

        json doc;
        try {
            in >> doc;
        } catch (const json::exception& e) {
            throw Error(ErrorCode::CorruptionDetected,
                        "invalid config JSON: " + std::string(e.what()));
        }

        static const std::set<std::string> known = {
            "data_dir", "memtable_max_bytes", "max_immutable_tables",
            "memtable_size_overhead_bytes_per_entry", "block_size", "bloom_false_positive",
            "sst_dir", "restart_interval", "max_build_buffer_mb",
            "block_cache_mb", "cache_index_blocks", "max_open_files",
            "wal_fsync_every_n", "wal_segment_roll_bytes", "compression", "log_level",
            "manifest_path", "publish_log_level",
            "size_tiered_fan_in", "size_tiered_size_ratio", "tombstone_grace_seconds",
            "compaction_max_concurrent", "compaction_io_mb_per_s",
            "l0_compaction_trigger", "l0_stop_writes", "bg_tick_ms", "shutdown_timeout_ms",
            "metrics_enabled", "http_listen_addr", "stats_sampling_interval_ms",
            "doctor_bundle_max_mb",
        };

        for (auto it = doc.begin(); it != doc.end(); ++it) {
            const std::string& key = it.key();
            if (!known.contains(key)) {
                std::cerr << "unknown config key: " << key << '\n';
                continue;
            }
            const auto& v = it.value();
            if (key == "data_dir")                 cfg.data_dir = v.get<std::string>();
            else if (key == "memtable_max_bytes")  cfg.memtable_max_bytes = v.get<std::uint64_t>();
            else if (key == "max_immutable_tables") cfg.max_immutable_tables = v.get<std::uint32_t>();
            else if (key == "memtable_size_overhead_bytes_per_entry")
                cfg.memtable_size_overhead_bytes_per_entry = v.get<std::uint32_t>();
            else if (key == "block_size")          cfg.block_size = v.get<std::uint32_t>();
            else if (key == "bloom_false_positive") cfg.bloom_false_positive = v.get<double>();
            else if (key == "sst_dir")             cfg.sst_dir = v.get<std::string>();
            else if (key == "restart_interval")    cfg.restart_interval = v.get<std::uint32_t>();
            else if (key == "max_build_buffer_mb") cfg.max_build_buffer_mb = v.get<std::uint32_t>();
            else if (key == "block_cache_mb")      cfg.block_cache_mb = v.get<std::uint32_t>();
            else if (key == "cache_index_blocks")  cfg.cache_index_blocks = v.get<bool>();
            else if (key == "max_open_files")      cfg.max_open_files = v.get<std::uint32_t>();
            else if (key == "wal_fsync_every_n")   cfg.wal_fsync_every_n = v.get<std::uint32_t>();
            else if (key == "wal_segment_roll_bytes") cfg.wal_segment_roll_bytes = v.get<std::uint64_t>();
            else if (key == "compression")         cfg.compression = parse_compression(v.get<std::string>());
            else if (key == "log_level")           cfg.log_level = log_level_from_string(v.get<std::string>());
            else if (key == "manifest_path")       cfg.manifest_path = v.get<std::string>();
            else if (key == "publish_log_level")   cfg.publish_log_level = log_level_from_string(v.get<std::string>());
            else if (key == "size_tiered_fan_in")  cfg.size_tiered_fan_in = v.get<std::uint32_t>();
            else if (key == "size_tiered_size_ratio") cfg.size_tiered_size_ratio = v.get<double>();
            else if (key == "tombstone_grace_seconds") cfg.tombstone_grace_seconds = v.get<std::uint32_t>();
            else if (key == "compaction_max_concurrent") cfg.compaction_max_concurrent = v.get<std::uint32_t>();
            else if (key == "compaction_io_mb_per_s") cfg.compaction_io_mb_per_s = v.get<std::uint32_t>();
            else if (key == "l0_compaction_trigger") cfg.l0_compaction_trigger = v.get<std::uint32_t>();
            else if (key == "l0_stop_writes")      cfg.l0_stop_writes = v.get<std::uint32_t>();
            else if (key == "bg_tick_ms")          cfg.bg_tick_ms = v.get<std::uint32_t>();
            else if (key == "shutdown_timeout_ms") cfg.shutdown_timeout_ms = v.get<std::uint32_t>();
            else if (key == "metrics_enabled")     cfg.metrics_enabled = v.get<bool>();
            else if (key == "http_listen_addr")    cfg.http_listen_addr = v.get<std::string>();
            else if (key == "stats_sampling_interval_ms")
                cfg.stats_sampling_interval_ms = v.get<std::uint32_t>();
            else if (key == "doctor_bundle_max_mb") cfg.doctor_bundle_max_mb = v.get<std::uint32_t>();
        }
    }
    // Missing file falls through with defaults; both paths still honor env overrides.

    cfg.apply_env();
    return cfg;
}

void Config::apply_env() {
    if (auto v = env("LSMKV_DATA_DIR"))             data_dir = *v;
    if (auto v = env("LSMKV_MEMTABLE_MAX_BYTES"))   memtable_max_bytes = std::stoull(*v);
    if (auto v = env("LSMKV_MAX_IMMUTABLE_TABLES")) max_immutable_tables = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_MEMTABLE_SIZE_OVERHEAD_BYTES_PER_ENTRY"))
        memtable_size_overhead_bytes_per_entry = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_BLOCK_SIZE"))           block_size = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_BLOOM_FALSE_POSITIVE")) bloom_false_positive = std::stod(*v);
    if (auto v = env("LSMKV_SST_DIR"))              sst_dir = *v;
    if (auto v = env("LSMKV_RESTART_INTERVAL"))     restart_interval = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_MAX_BUILD_BUFFER_MB"))  max_build_buffer_mb = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_BLOCK_CACHE_MB"))       block_cache_mb = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_CACHE_INDEX_BLOCKS"))   cache_index_blocks = parse_bool(*v);
    if (auto v = env("LSMKV_MAX_OPEN_FILES"))       max_open_files = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_WAL_FSYNC_EVERY_N"))    wal_fsync_every_n = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_WAL_SEGMENT_ROLL_BYTES")) wal_segment_roll_bytes = std::stoull(*v);
    if (auto v = env("LSMKV_COMPRESSION"))          compression = parse_compression(*v);
    if (auto v = env("LSMKV_LOG_LEVEL"))            log_level = log_level_from_string(*v);
    if (auto v = env("LSMKV_MANIFEST_PATH"))        manifest_path = *v;
    if (auto v = env("LSMKV_PUBLISH_LOG_LEVEL"))    publish_log_level = log_level_from_string(*v);
    if (auto v = env("LSMKV_SIZE_TIERED_FAN_IN"))   size_tiered_fan_in = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_SIZE_TIERED_SIZE_RATIO")) size_tiered_size_ratio = std::stod(*v);
    if (auto v = env("LSMKV_TOMBSTONE_GRACE_SECONDS")) tombstone_grace_seconds = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_COMPACTION_MAX_CONCURRENT")) compaction_max_concurrent = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_COMPACTION_IO_MB_PER_S")) compaction_io_mb_per_s = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_L0_COMPACTION_TRIGGER")) l0_compaction_trigger = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_L0_STOP_WRITES"))       l0_stop_writes = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_BG_TICK_MS"))           bg_tick_ms = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_SHUTDOWN_TIMEOUT_MS"))  shutdown_timeout_ms = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_METRICS_ENABLED"))      metrics_enabled = parse_bool(*v);
    if (auto v = env("LSMKV_HTTP_LISTEN_ADDR"))     http_listen_addr = *v;
    if (auto v = env("LSMKV_STATS_SAMPLING_INTERVAL_MS"))
        stats_sampling_interval_ms = static_cast<std::uint32_t>(std::stoul(*v));
    if (auto v = env("LSMKV_DOCTOR_BUNDLE_MAX_MB")) doctor_bundle_max_mb = static_cast<std::uint32_t>(std::stoul(*v));
}

void Config::print(std::ostream& os) const {
    os << "data_dir="              << data_dir                  << '\n'
       << "memtable_max_bytes="    << memtable_max_bytes        << '\n'
       << "max_immutable_tables="  << max_immutable_tables      << '\n'
       << "memtable_size_overhead_bytes_per_entry=" << memtable_size_overhead_bytes_per_entry << '\n'
       << "block_size="            << block_size                << '\n'
       << "bloom_false_positive="  << bloom_false_positive      << '\n'
       << "sst_dir="               << resolved_sst_dir().string() << '\n'
       << "restart_interval="      << restart_interval          << '\n'
       << "max_build_buffer_mb="   << max_build_buffer_mb       << '\n'
       << "block_cache_mb="        << block_cache_mb            << '\n'
       << "cache_index_blocks="    << (cache_index_blocks ? "true" : "false") << '\n'
       << "max_open_files="        << max_open_files            << '\n'
       << "wal_fsync_every_n="     << wal_fsync_every_n         << '\n'
       << "wal_segment_roll_bytes=" << wal_segment_roll_bytes   << '\n'
       << "compression="           << to_string(compression)    << '\n'
       << "log_level="             << to_string(log_level)      << '\n'
       << "manifest_path="         << resolved_manifest_path().string() << '\n'
       << "publish_log_level="     << to_string(publish_log_level) << '\n'
       << "size_tiered_fan_in="    << size_tiered_fan_in        << '\n'
       << "size_tiered_size_ratio=" << size_tiered_size_ratio   << '\n'
       << "tombstone_grace_seconds=" << tombstone_grace_seconds << '\n'
       << "compaction_max_concurrent=" << compaction_max_concurrent << '\n'
       << "compaction_io_mb_per_s=" << compaction_io_mb_per_s   << '\n'
       << "l0_compaction_trigger=" << l0_compaction_trigger     << '\n'
       << "l0_stop_writes="        << l0_stop_writes            << '\n'
       << "bg_tick_ms="            << bg_tick_ms                << '\n'
       << "shutdown_timeout_ms="   << shutdown_timeout_ms       << '\n'
       << "metrics_enabled="       << (metrics_enabled ? "true" : "false") << '\n'
       << "http_listen_addr="      << http_listen_addr          << '\n'
       << "stats_sampling_interval_ms=" << stats_sampling_interval_ms << '\n'
       << "doctor_bundle_max_mb="  << doctor_bundle_max_mb      << '\n';
}

} // namespace lsm
