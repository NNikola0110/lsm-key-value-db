// lsmkv - CLI entry point.
// Section 0: skeleton. Section 1: put/del append to the WAL for real;
// every command that opens the engine prints the recovery report first.

#include "lsm/compaction.hpp"
#include "lsm/config.hpp"
#include "lsm/engine.hpp"
#include "lsm/errors.hpp"
#include "lsm/sstable.hpp"
#include "lsm/wal.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

void print_usage(std::ostream& os) {
    os << "usage: lsmkv <command> [flags]\n"
          "\n"
          "commands:\n"
          "  init                       prepare data/ and data/wal/, print resolved config\n"
          "  put   --key K --value V    WAL append + memtable update\n"
          "  get   --key K              read memtables then SSTables; value or NOT FOUND\n"
          "  probe --key K --repeat N   N in-process gets of one key (cache demo)\n"
          "  probe --absent N           N gets of random absent keys (bloom demo)\n"
          "  sst-info --file F          footer, index, bloom and block-size details\n"
          "  del   --key K              WAL append + memtable tombstone\n"
          "  stats                      print config, WAL/memtable/SSTable/version stats\n"
          "  manifest-info              manifest epoch and live table listing\n"
          "  version-info               current Version: epoch/id, immutables, SSTables\n"
          "  close                      finish appends, sync, exit cleanly\n"
          "  flush-now                  flush pending immutable memtables to SSTables\n"
          "  list-sst                   list live SSTables from the manifest\n"
          "  verify-sst --file F        check footer magic and all block checksums\n"
          "  compaction-run [--files 1,3,4]   run one compaction job (picker or manual)\n"
          "  compaction-stats           backlog, last job summary, paused state\n"
          "  compaction-pause           stop picker/auto compactions (flag file)\n"
          "  compaction-resume          allow compactions again\n"
          "  repl                       interactive session with background workers\n"
          "  stress --threads N --ops M [--read-pct P]   concurrent hammer test\n"
          "  bg-status                  worker states, queues, job totals\n"
          "  shutdown [--fast]          exercise graceful (default) or fast shutdown\n"
          "  wal-verify                 scan WAL segments, print a recovery report (read-only)\n"
          "  wal-truncate --segment S --offset N   truncate a WAL segment (careful!)\n"
          "\n"
          "global flags:\n"
          "  --config PATH              config file (default: config/default.json)\n"
          "  --log-level LEVEL          override log_level (debug prints get traces)\n";
}

// Parse "--flag value" and "--flag=value" tokens into a map.
// A flag with no following value (or followed by another flag) maps to "".
std::unordered_map<std::string, std::string>
parse_flags(const std::vector<std::string>& args, std::size_t start) {
    std::unordered_map<std::string, std::string> flags;
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string& tok = args[i];
        if (!tok.starts_with("--")) {
            continue;
        }
        std::string name = tok.substr(2);
        std::string value;
        if (auto eq = name.find('='); eq != std::string::npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
        } else if (i + 1 < args.size() && !args[i + 1].starts_with("--")) {
            value = args[++i];
        }
        flags[name] = value;
    }
    return flags;
}

fs::path resolve_config_path(const std::unordered_map<std::string, std::string>& flags) {
    if (auto it = flags.find("config"); it != flags.end() && !it->second.empty()) {
        return fs::path(it->second);
    }
    return fs::path("config") / "default.json";
}

lsm::Config load_config(const std::unordered_map<std::string, std::string>& flags) {
    lsm::Config cfg = lsm::Config::load(resolve_config_path(flags));
    // CLI flags are the strongest layer of the priority order (Section 0.4).
    if (auto it = flags.find("log-level"); it != flags.end() && !it->second.empty()) {
        cfg.log_level = lsm::log_level_from_string(it->second);
    }
    return cfg;
}

void print_trace(const lsm::GetTrace& t, bool found) {
    std::cout << "trace: memtable=" << (t.memtable_hit ? "hit" : "miss")
              << " immutables_consulted=" << t.immutables_consulted
              << " sstables_consulted=" << t.sstables_consulted
              << " range_skipped=" << t.range_skipped
              << " blooms_skipped=" << t.blooms_skipped
              << " block_reads=" << t.block_reads
              << " cache_hits=" << t.cache_hits
              << " result=" << (found ? "found" : "not_found") << '\n';
}

void print_read_stats(const lsm::Engine& engine) {
    const lsm::ReadStats& r = engine.read_stats();
    const std::uint64_t lookups = r.cache_hits + r.cache_misses;
    std::cout << "read.sstables_open="      << engine.sstables_open() << '\n'
              << "read.blooms_checked="     << r.blooms_checked << '\n'
              << "read.blooms_negative="    << r.blooms_negative << '\n'
              << "read.block_cache_hits="   << r.cache_hits << '\n'
              << "read.block_cache_misses=" << r.cache_misses << '\n'
              << "read.block_cache_hit_rate="
              << (lookups ? static_cast<double>(r.cache_hits) / lookups : 0.0) << '\n'
              << "read.disk_block_reads="   << r.disk_block_reads << '\n';
}

std::string require_flag(const std::unordered_map<std::string, std::string>& flags,
                         const std::string& name, const std::string& cmd) {
    auto it = flags.find(name);
    if (it == flags.end() || it->second.empty()) {
        throw lsm::Error(lsm::ErrorCode::InvalidArgument,
                         "--" + name + " is required for '" + cmd + "'");
    }
    return it->second;
}

// Open the engine (replays the WAL into the memtable) and print the
// recovery reports (Sections 1.8 and 2.7).
void open_engine(lsm::Engine& engine) {
    const lsm::EngineRecovery r = engine.open();
    r.wal.print(std::cout);
    std::cout << "recovery: memtable_keys=" << r.memtable_keys
              << " memtable_bytes=" << r.memtable_bytes
              << " last_seqno=" << r.wal.last_seqno
              << " sst_count=" << r.sst_count;
    if (r.tmp_files_removed > 0) {
        std::cout << " tmp_files_removed=" << r.tmp_files_removed;
    }
    std::cout << '\n';
    std::cout << "startup: manifest_tables=" << r.sst_count
              << " epoch=" << r.epoch
              << " wal_records=" << r.wal.records
              << " version_published=" << r.version_published << '\n';
}

int run_mutation(const std::string& cmd,
                 const std::unordered_map<std::string, std::string>& flags,
                 const lsm::Config& cfg) {
    const std::string key = require_flag(flags, "key", cmd);

    lsm::Engine engine(cfg);
    open_engine(engine);

    if (cmd == "put") {
        auto val_it = flags.find("value");
        const std::string_view value = val_it != flags.end() ? val_it->second : std::string_view{};
        const std::uint64_t seqno = engine.put(key, value);
        std::cout << "ok seqno=" << seqno
                  << " memtable_bytes=" << engine.memtable_stats().active_bytes << '\n';
    } else if (cmd == "del") {
        const std::uint64_t seqno = engine.remove(key);
        std::cout << "ok seqno=" << seqno
                  << " memtable_bytes=" << engine.memtable_stats().active_bytes << '\n';
    } else { // get
        lsm::GetTrace trace;
        const auto value = engine.get(key, &trace);
        if (cfg.log_level == lsm::LogLevel::Debug) {
            print_trace(trace, value.has_value());
        }
        if (value) {
            std::cout << *value << '\n';
        } else {
            std::cout << "NOT FOUND\n";
        }
    }

    engine.close();
    return 0;
}

int run_probe(const std::unordered_map<std::string, std::string>& flags,
              const lsm::Config& cfg) {
    lsm::Engine engine(cfg);
    open_engine(engine);

    if (auto it = flags.find("absent"); it != flags.end() && !it->second.empty()) {
        const std::uint64_t count = std::stoull(it->second);
        std::uint64_t found = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            // Deterministic but wildly unlikely to exist.
            const std::string key = "absent-" + std::to_string(i) + "-x9q7z-" +
                                    std::to_string(i * 2654435761u);
            if (engine.get(key)) found += 1;
        }
        std::cout << "probed " << count << " absent keys, found=" << found << '\n';
    } else {
        const std::string key = require_flag(flags, "key", "probe");
        std::uint64_t repeat = 1;
        if (auto rit = flags.find("repeat"); rit != flags.end() && !rit->second.empty()) {
            repeat = std::stoull(rit->second);
        }
        std::optional<std::string> value;
        for (std::uint64_t i = 0; i < repeat; ++i) {
            value = engine.get(key);
        }
        std::cout << "probed key=" << key << " x" << repeat << " -> "
                  << (value ? *value : "NOT FOUND") << '\n';
    }

    print_read_stats(engine);
    engine.close();
    return 0;
}

int run_stats(const lsm::Config& cfg) {
    lsm::Engine engine(cfg);
    open_engine(engine);

    engine.config().print(std::cout);
    const lsm::WalStats s = engine.wal_stats();
    std::cout << "wal.active_segment="       << s.active_segment_id << '\n'
              << "wal.active_segment_bytes=" << s.active_segment_bytes << '\n'
              << "wal.total_segments="       << s.total_segments << '\n'
              << "wal.last_seqno="           << s.last_seqno << '\n'
              << "wal.fsync_every_n="        << s.fsync_every_n << '\n'
              << "wal.segment_roll_bytes="   << s.roll_bytes << '\n';
    const lsm::MemtableStats m = engine.memtable_stats();
    std::cout << "memtable.active_entries="         << m.active_entries << '\n'
              << "memtable.active_bytes="           << m.active_bytes << '\n'
              << "memtable.immutables_count="       << m.immutables_count << '\n'
              << "memtable.immutables_bytes_total=" << m.immutables_bytes_total << '\n'
              << "memtable.flush_requested="        << m.flush_requested << '\n';
    const lsm::SstStats t = engine.sst_stats();
    std::cout << "sst.count="       << t.sst_count << '\n'
              << "sst.total_bytes=" << t.sst_total_bytes << '\n'
              << "sst.newest_id="   << t.newest_id << '\n';
    print_read_stats(engine);
    const auto version = engine.current_version();
    std::cout << "version.epoch="  << version->epoch << '\n'
              << "version.id="     << version->id << '\n'
              << "engine status: full single-process read/write path (compaction pending)\n";
    engine.close();
    return 0;
}

int run_wal_truncate(const std::unordered_map<std::string, std::string>& flags,
                     const lsm::Config& cfg) {
    const std::string segment = require_flag(flags, "segment", "wal-truncate");
    const std::string offset_s = require_flag(flags, "offset", "wal-truncate");

    std::uint64_t offset = 0;
    try {
        offset = std::stoull(offset_s);
    } catch (const std::exception&) {
        throw lsm::Error(lsm::ErrorCode::InvalidArgument, "--offset must be a number");
    }

    const fs::path path = fs::path(cfg.data_dir) / "wal" / segment;
    if (!fs::exists(path)) {
        throw lsm::Error(lsm::ErrorCode::InvalidArgument, "no such segment: " + path.string());
    }
    const std::uint64_t size = fs::file_size(path);
    if (offset > size) {
        throw lsm::Error(lsm::ErrorCode::InvalidArgument,
                         "offset " + offset_s + " is beyond segment size " + std::to_string(size));
    }
    fs::resize_file(path, offset);
    std::cout << "truncated " << segment << " from " << size << " to " << offset << " bytes\n";
    return 0;
}

void print_bg_status(lsm::Engine& engine, const lsm::Config& cfg) {
    const lsm::BgStats& b = engine.bg_stats();
    const lsm::MemtableStats m = engine.memtable_stats();
    const auto pick = lsm::pick_compaction(engine.manifest().tables(), cfg);
    std::cout << "workers: running=" << (engine.background_running() ? "yes" : "no (one-shot)")
              << " flush_running=" << b.flush_running
              << " compact_running=" << b.compact_running << '\n'
              << "queues: immutables=" << m.immutables_count
              << " immutable_bytes=" << m.immutables_bytes_total
              << " compaction_backlog_bytes=" << (pick ? pick->total_bytes : 0) << '\n'
              << "totals: flush_jobs=" << b.flush_jobs_total
              << " flush_last_ms=" << b.flush_last_ms
              << " compactions=" << b.compactions_total
              << " compaction_last_ms=" << b.compaction_last_ms
              << " versions_published=" << b.versions_published_total << '\n'
              << "stalls: write_stalls=" << b.write_stalls_total
              << " stall_time_ms=" << b.stall_time_ms_total << '\n'
              << "compaction_paused=" << (engine.compaction_paused() ? "yes" : "no") << '\n';
    const lsm::CompactionState state = lsm::load_compaction_state(cfg.data_dir);
    if (state.last_job) {
        std::cout << "last job (" << state.last_job_finished_at << "): "
                  << lsm::format_job_line(*state.last_job) << '\n';
    }
}

int run_repl(const lsm::Config& cfg) {
    lsm::Engine engine(cfg);
    open_engine(engine);
    engine.start_background();
    std::cout << "repl ready (put K V | get K | del K | stats | bg-status | flush | compact"
                 " | pause | resume | quit [--fast])\n";

    std::string line;
    bool graceful = true;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd, a, b;
        is >> cmd >> a >> b;
        try {
            if (cmd.empty()) continue;
            if (cmd == "quit" || cmd == "exit") {
                graceful = (a != "--fast");
                break;
            }
            if (cmd == "put")       std::cout << "ok seqno=" << engine.put(a, b) << '\n';
            else if (cmd == "del")  std::cout << "ok seqno=" << engine.remove(a) << '\n';
            else if (cmd == "get") {
                const auto v = engine.get(a);
                std::cout << (v ? *v : "NOT FOUND") << '\n';
            } else if (cmd == "stats") {
                const lsm::MemtableStats m = engine.memtable_stats();
                const lsm::SstStats t = engine.sst_stats();
                std::cout << "active_bytes=" << m.active_bytes
                          << " immutables=" << m.immutables_count
                          << " sst_count=" << t.sst_count
                          << " version=" << engine.current_version()->id << '\n';
            } else if (cmd == "bg-status") print_bg_status(engine, cfg);
            else if (cmd == "flush")   { for (const auto& r : engine.flush_pending())
                                             std::cout << "flushed " << r.file_name << '\n'; }
            else if (cmd == "compact") { if (!engine.run_compaction(std::nullopt))
                                             std::cout << "no compaction set available\n"; }
            else if (cmd == "pause")   engine.set_compaction_paused(true);
            else if (cmd == "resume")  engine.set_compaction_paused(false);
            else std::cout << "unknown repl command: " << cmd << '\n';
        } catch (const lsm::Error& e) {
            std::cout << '[' << lsm::to_string(e.code()) << "] " << e.what() << '\n';
        }
    }
    std::cout << (graceful ? "shutting down (graceful)...\n" : "shutting down (fast)...\n");
    engine.close(graceful);
    std::cout << "closed ✓\n";
    return 0;
}

int run_stress(const std::unordered_map<std::string, std::string>& flags,
               const lsm::Config& cfg) {
    const std::uint64_t threads = flags.contains("threads") && !flags.at("threads").empty()
                                      ? std::stoull(flags.at("threads")) : 4;
    const std::uint64_t ops = flags.contains("ops") && !flags.at("ops").empty()
                                  ? std::stoull(flags.at("ops")) : 2000;
    const std::uint64_t read_pct = flags.contains("read-pct") && !flags.at("read-pct").empty()
                                       ? std::stoull(flags.at("read-pct")) : 50;

    lsm::Engine engine(cfg);
    open_engine(engine);
    engine.start_background();

    std::atomic<std::uint64_t> writes{0}, reads{0}, hits{0}, errors{0};
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (std::uint64_t t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(t * 7919 + 17);
            for (std::uint64_t i = 0; i < ops; ++i) {
                const std::uint64_t k = rng() % (threads * ops / 4 + 1);
                const std::string key = "s" + std::to_string(k);
                try {
                    if (rng() % 100 < read_pct) {
                        if (engine.get(key)) hits += 1;
                        reads += 1;
                    } else {
                        engine.put(key, "stress-value-" + std::to_string(i));
                        writes += 1;
                    }
                } catch (const lsm::Error&) {
                    errors += 1;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    std::cout << "stress: threads=" << threads << " ops_per_thread=" << ops
              << " writes=" << writes << " reads=" << reads << " read_hits=" << hits
              << " errors=" << errors << " dur=" << ms << "ms"
              << " ops_per_sec=" << (ms > 0 ? (writes + reads) * 1000 / ms : 0) << '\n';
    print_bg_status(engine, cfg);
    engine.close(true);
    std::cout << "closed ✓ (graceful)\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        print_usage(std::cerr);
        return 2;
    }

    const std::string cmd = args[0];
    const auto flags = parse_flags(args, 1);

    try {
        const lsm::Config cfg = load_config(flags);

        if (cmd == "init") {
            lsm::Engine engine(cfg);
            open_engine(engine);          // creates data/ and data/wal/
            engine.close();
            std::cout << "config loaded ✓, data dir ready ✓, wal dir ready ✓, manifest placeholder ✓\n";
            cfg.print(std::cout);
            return 0;
        }

        if (cmd == "put" || cmd == "get" || cmd == "del") {
            return run_mutation(cmd, flags, cfg);
        }

        if (cmd == "probe") {
            return run_probe(flags, cfg);
        }

        if (cmd == "stats") {
            return run_stats(cfg);
        }

        if (cmd == "close") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            engine.close();
            std::cout << "closed ✓ (wal synced)\n";
            return 0;
        }

        if (cmd == "flush-now") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            const auto results = engine.flush_pending();
            if (results.empty()) {
                std::cout << "nothing to flush (no immutable memtables)\n";
            }
            for (const auto& r : results) {
                std::cout << "flushed " << r.file_name << " bytes=" << r.file_size
                          << " entries=" << r.entries
                          << " seqno=[" << r.min_seqno << ".." << r.max_seqno << "]\n";
            }
            engine.close();
            return 0;
        }

        if (cmd == "manifest-info") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            const lsm::Manifest& m = engine.manifest();
            std::cout << "manifest: epoch=" << m.epoch()
                      << " tables=" << m.tables().size()
                      << " next_sst_id=" << m.next_id()
                      << " total_bytes=" << m.total_bytes() << '\n';
            for (const auto& t : m.tables()) {   // newest first
                std::cout << "  " << t.file_name << " size=" << t.file_size
                          << " keys=[" << t.min_key << ".." << t.max_key << "]"
                          << " seqno=[" << t.min_seqno << ".." << t.max_seqno << "]\n";
            }
            engine.close();
            return 0;
        }

        if (cmd == "version-info") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            const auto v = engine.current_version();
            std::cout << "version: id=" << v->id << " epoch=" << v->epoch
                      << " active_bytes=" << v->active->bytes()
                      << " immutables=" << v->immutables.size()
                      << " sstables=" << v->tables.size() << '\n';
            std::size_t i = 0;
            for (const auto& imm : v->immutables) {   // newest first
                std::cout << "  immutable[" << i++ << "] entries=" << imm->entries()
                          << " bytes=" << imm->bytes() << '\n';
            }
            for (const auto& table : v->tables) {     // newest first
                const auto& meta = table->meta();
                std::cout << "  sst " << meta.file_name
                          << " seqno=[" << meta.min_seqno << ".." << meta.max_seqno << "]\n";
            }
            engine.close();
            return 0;
        }

        if (cmd == "list-sst") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            const auto& tables = engine.manifest().tables();
            if (tables.empty()) {
                std::cout << "no live SSTables\n";
            }
            for (const auto& t : tables) {
                std::cout << t.file_name << " size=" << t.file_size
                          << " entries=" << t.entries
                          << " keys=[" << t.min_key << ".." << t.max_key << "]"
                          << " seqno=[" << t.min_seqno << ".." << t.max_seqno << "]"
                          << " bloom=" << t.bloom_bits_per_key << "bits/key,k="
                          << t.bloom_hashes
                          << " created=" << t.created_at << '\n';
            }
            engine.close();
            return 0;
        }

        if (cmd == "verify-sst" || cmd == "sst-info") {
            const std::string file = require_flag(flags, "file", cmd);
            fs::path target(file);
            if (!target.has_parent_path()) {
                target = cfg.resolved_sst_dir() / target;
            }
            if (cmd == "verify-sst") {
                const std::string verdict = lsm::verify_sstable(target);
                std::cout << target.filename().string() << ": " << verdict << '\n';
                return verdict == "OK" ? 0 : 1;
            }
            const lsm::SstInfo info = lsm::inspect_sstable(target);
            std::cout << target.filename().string() << ":\n"
                      << "  file_size="    << info.file_size
                      << " version="      << info.version << '\n'
                      << "  index: offset=" << info.index_off << " size=" << info.index_size
                      << " entries="      << info.data_blocks << '\n'
                      << "  blocks: count=" << info.data_blocks
                      << " min="          << info.block_min
                      << " avg="          << info.block_avg
                      << " max="          << info.block_max << '\n'
                      << "  bloom: offset=" << info.filter_off << " size=" << info.filter_size
                      << " bits="         << info.bloom_bits
                      << " k="            << info.bloom_hashes
                      << " keys="         << info.bloom_keys
                      << " bits_per_key="
                      << (info.bloom_keys
                              ? static_cast<double>(info.bloom_bits) / info.bloom_keys
                              : 0.0)
                      << '\n';
            return 0;
        }

        if (cmd == "compaction-run") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            if (engine.compaction_paused()) {
                std::cout << "compaction is paused (data/compaction.paused); "
                             "run compaction-resume first\n";
                engine.close();
                return 1;
            }
            std::optional<std::vector<std::uint64_t>> ids;
            if (auto it = flags.find("files"); it != flags.end() && !it->second.empty()) {
                ids.emplace();
                std::stringstream ss(it->second);
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    // Accept "000101", "101" or "000101.sst".
                    if (tok.size() > 4 && tok.ends_with(".sst")) tok.resize(tok.size() - 4);
                    ids->push_back(std::stoull(tok));
                }
            }
            if (!engine.run_compaction(ids)) {
                std::cout << "no compaction set available (need >= "
                          << cfg.size_tiered_fan_in << " tables)\n";
            }
            engine.close();
            return 0;
        }

        if (cmd == "compaction-stats") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            const lsm::CompactionState state = lsm::load_compaction_state(cfg.data_dir);
            const auto pick = lsm::pick_compaction(engine.manifest().tables(), cfg);
            std::cout << "compaction: paused=" << (engine.compaction_paused() ? "yes" : "no")
                      << " running=0 tables=" << engine.manifest().tables().size()
                      << " backlog_bytes=" << (pick ? pick->total_bytes : 0) << '\n';
            if (pick) {
                std::cout << "next pick: [";
                for (std::size_t i = 0; i < pick->ids.size(); ++i) {
                    std::cout << (i ? "," : "") << pick->ids[i];
                }
                std::cout << "] reason=" << pick->reason << '\n';
            }
            if (state.last_job) {
                std::cout << "last job (" << state.last_job_finished_at << "): "
                          << lsm::format_job_line(*state.last_job) << '\n';
            } else {
                std::cout << "last job: none\n";
            }
            engine.close();
            return 0;
        }

        if (cmd == "compaction-pause" || cmd == "compaction-resume") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            engine.set_compaction_paused(cmd == "compaction-pause");
            std::cout << (cmd == "compaction-pause" ? "compaction paused\n"
                                                    : "compaction resumed\n");
            engine.close();
            return 0;
        }

        if (cmd == "repl") {
            return run_repl(cfg);
        }

        if (cmd == "stress") {
            return run_stress(flags, cfg);
        }

        if (cmd == "bg-status") {
            lsm::Engine engine(cfg);
            open_engine(engine);
            print_bg_status(engine, cfg);
            engine.close();
            return 0;
        }

        if (cmd == "shutdown") {
            const bool fast = flags.contains("fast");
            lsm::Engine engine(cfg);
            open_engine(engine);
            engine.start_background();
            engine.close(!fast);
            std::cout << "closed ✓ (" << (fast ? "fast" : "graceful") << ")\n";
            return 0;
        }

        if (cmd == "wal-verify") {
            const auto report = lsm::Wal::verify(fs::path(cfg.data_dir) / "wal");
            report.print(std::cout);
            return 0;
        }

        if (cmd == "wal-truncate") {
            return run_wal_truncate(flags, cfg);
        }

        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            print_usage(std::cout);
            return 0;
        }

        std::cerr << "unknown command: " << cmd << "\n\n";
        print_usage(std::cerr);
        return 2;

    } catch (const lsm::Error& e) {
        std::cerr << '[' << lsm::to_string(e.code()) << "] " << e.what() << '\n';
        return e.code() == lsm::ErrorCode::InvalidArgument ? 2 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
