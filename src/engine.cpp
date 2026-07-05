#include "lsm/engine.hpp"

#include "lsm/errors.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>

namespace lsm {

namespace fs = std::filesystem;

namespace {

std::string utc_now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

Engine::Engine(Config config) : config_(std::move(config)) {}

Engine::~Engine() = default;

EngineRecovery Engine::open() {
    const fs::path sst_dir = config_.resolved_sst_dir();
    std::error_code ec;
    fs::create_directories(config_.data_dir, ec);
    if (!ec) fs::create_directories(sst_dir, ec);
    if (ec) {
        throw Error(ErrorCode::IOFailure,
                    "cannot create data dirs under '" + config_.data_dir + "': " + ec.message());
    }

    EngineRecovery recovery;

    // 3.9: leftover temp files are unpublished garbage from a crashed flush.
    for (const auto& entry : fs::directory_iterator(sst_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tmp") {
            fs::remove(entry.path(), ec);
            recovery.tmp_files_removed += 1;
        }
    }

    manifest_ = Manifest::load_or_create(config_.resolved_manifest_path());

    // 6.7: a .sst not referenced by the manifest is an orphan — either a
    // compaction/flush output whose manifest edit never happened, or a
    // compaction input whose deletion was deferred. Delete it.
    for (const auto& entry : fs::directory_iterator(sst_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sst") continue;
        const std::string name = entry.path().filename().string();
        const bool live = std::any_of(manifest_.tables().begin(), manifest_.tables().end(),
                                      [&](const TableMeta& t) { return t.file_name == name; });
        if (!live) {
            fs::remove(entry.path(), ec);
            std::cerr << "sst: deleted orphan " << name << " (not in manifest)\n";
        }
    }
    version_id_ = manifest_.epoch();
    block_cache_ = std::make_unique<BlockCache>(
        static_cast<std::uint64_t>(config_.block_cache_mb) * 1024 * 1024);
    fd_pool_ = std::make_unique<FdPool>(config_.max_open_files);
    // Manifest is newest-first; keep readers_ in the same order (5.11).
    for (const auto& t : manifest_.tables()) {
        if (!fs::exists(sst_dir / t.file_name)) {
            throw Error(ErrorCode::CorruptionDetected,
                        "manifest references missing SSTable: " + t.file_name);
        }
        auto reader = std::make_shared<TableReader>(sst_dir / t.file_name, t, config_);
        reader->validate_footer();   // quick open check (4.3 A)
        readers_.push_back(std::move(reader));
    }
    recovery.sst_count = manifest_.tables().size();

    active_ = std::make_shared<Memtable>(config_.memtable_size_overhead_bytes_per_entry);
    wal_ = std::make_unique<Wal>(fs::path(config_.data_dir) / "wal",
                                 config_.wal_fsync_every_n,
                                 config_.wal_segment_roll_bytes);

    // Replay applies the same write-path rules as live mutations (2.7),
    // including rotation, so the table set matches what existed pre-crash.
    recovery.wal = wal_->open([this](bool is_del, std::string_view key,
                                     std::string_view value, std::uint64_t seqno,
                                     std::uint64_t /*segment_id*/) {
        apply_mutation(key, value, seqno, is_del, /*replaying=*/true);
    });

    // Flushed data may carry higher seqNos than anything left in the WAL
    // (its segments were deleted); never let the counter run backwards.
    wal_->bump_seqno(manifest_.max_seqno());
    recovery.wal.last_seqno = std::max(recovery.wal.last_seqno, manifest_.max_seqno());

    recovery.memtable_keys = active_->entries();
    recovery.memtable_bytes = active_->bytes();

    // Startup publish (5.6): the first Version readers can use — manifest
    // tables plus the memtables rebuilt from the WAL.
    publish_version("startup");
    recovery.epoch = manifest_.epoch();
    recovery.version_published = current_version_->id;
    return recovery;
}

void Engine::publish_version(const char* reason) {
    auto v = std::make_shared<Version>();
    v->id = ++version_id_;
    v->epoch = manifest_.epoch();
    v->active = active_;
    v->immutables = immutables_;
    v->tables = readers_;
    current_version_ = std::move(v);   // atomic pointer swap: readers holding
                                       // the old Version keep using it (5.5)
    if (config_.publish_log_level == LogLevel::Debug) {
        std::cerr << "publish: version=" << current_version_->id
                  << " epoch=" << current_version_->epoch
                  << " immutables=" << immutables_.size()
                  << " sst=" << readers_.size()
                  << " reason=" << reason << '\n';
    }
}

void Engine::ensure_open() const {
    if (closed_) {
        throw Error(ErrorCode::StoreClosed, "store is closed");
    }
    if (!wal_) {
        throw Error(ErrorCode::StoreClosed, "store is not open (call open() first)");
    }
}

// Backpressure policy (2.6, Option A): if the active table is full and the
// immutable list is at max_immutable_tables, refuse the write. flush-now
// (Section 3) is the way out until background flushing exists (Section 7).
void Engine::ensure_write_capacity() {
    // Optional write stall (6.8): too many SSTables means compaction has
    // fallen far behind; refuse writes loudly until it catches up.
    if (manifest_.tables().size() > config_.l0_stop_writes) {
        std::cerr << "WRITE STALL: sstables=" << manifest_.tables().size()
                  << " > l0_stop_writes=" << config_.l0_stop_writes
                  << "; run compaction-run\n";
        throw Error(ErrorCode::Backpressure,
                    "too many SSTables; writes stalled until compaction runs");
    }
    if (active_->bytes() >= config_.memtable_max_bytes &&
        immutables_.size() >= config_.max_immutable_tables) {
        std::cerr << "backpressure: immutables=" << immutables_.size()
                  << " (max=" << config_.max_immutable_tables << "); blocking writes\n";
        throw Error(ErrorCode::Backpressure,
                    "memtable full and immutable tables at limit; run flush-now");
    }
}

std::uint64_t Engine::put(std::string_view key, std::string_view value) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    ensure_write_capacity();
    const std::uint64_t seqno = wal_->append_put(key, value);   // durable first
    apply_mutation(key, value, seqno, /*tombstone=*/false, /*replaying=*/false);
    return seqno;
}

std::uint64_t Engine::remove(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    ensure_write_capacity();
    // Deleting a non-existent key is fine: we still log a DEL record.
    const std::uint64_t seqno = wal_->append_del(key);
    apply_mutation(key, {}, seqno, /*tombstone=*/true, /*replaying=*/false);
    return seqno;
}

std::optional<std::string> Engine::get(std::string_view key, GetTrace* trace) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    GetTrace local;
    GetTrace& t = trace ? *trace : local;

    // Grab the current Version once and read only through it (5.7). The
    // shared_ptr keeps everything in it alive even if a publish happens.
    const std::shared_ptr<const Version> v = current_version_;

    // Global read order (4.3 C): active -> immutables (newest->oldest) ->
    // SSTables (newest->oldest). First hit wins; a tombstone means NOT FOUND
    // and stops the search.
    if (const MemEntry* e = v->active->find(key)) {
        t.memtable_hit = true;
        return e->tombstone ? std::nullopt : std::optional<std::string>(e->value);
    }
    for (const auto& imm : v->immutables) {      // stored newest-first
        t.immutables_consulted += 1;
        if (const MemEntry* e = imm->find(key)) {
            return e->tombstone ? std::nullopt : std::optional<std::string>(e->value);
        }
    }
    for (const auto& table : v->tables) {        // stored newest-first
        t.sstables_consulted += 1;
        if (const auto hit = table->get(key, *block_cache_, *fd_pool_, read_stats_, t)) {
            return hit->tombstone ? std::nullopt : std::optional<std::string>(hit->value);
        }
    }
    return std::nullopt;
}

void Engine::apply_mutation(std::string_view key, std::string_view value,
                            std::uint64_t seqno, bool tombstone, bool replaying) {
    active_->apply(key, value, seqno, tombstone);
    maybe_rotate(replaying);
}

void Engine::maybe_rotate(bool replaying) {
    if (active_->bytes() < config_.memtable_max_bytes) return;

    if (immutables_.size() >= config_.max_immutable_tables) {
        // Rotation deferred: the active table keeps growing; the next live
        // write is refused by ensure_write_capacity(). Replay cannot block,
        // so it only warns (once).
        if (replaying && !backpressure_warned_) {
            std::cerr << "backpressure: immutables=" << immutables_.size()
                      << " (max=" << config_.max_immutable_tables
                      << "); rotation deferred during replay\n";
            backpressure_warned_ = true;
        }
        return;
    }

    // A live rotation rolls the WAL so freshly-frozen data stops sharing a
    // segment with new writes — that keeps watermark cleanup prompt. During
    // replay the WAL is not open for writing (and rolling would be wrong).
    if (!replaying) {
        wal_->roll();
    }

    immutables_.insert(immutables_.begin(), std::move(active_));   // newest first
    active_ = std::make_shared<Memtable>(config_.memtable_size_overhead_bytes_per_entry);
    flush_requested_ += 1;

    // Publish the new table set; replay publishes once at the end of open().
    if (!replaying) {
        publish_version("rotation");
    }
}

std::vector<SstBuildResult> Engine::flush_pending() {
    ensure_open();
    std::vector<SstBuildResult> results;
    const fs::path sst_dir = config_.resolved_sst_dir();

    while (!immutables_.empty()) {
        const Memtable& oldest = *immutables_.back();   // newest-first list

        SstBuildResult r = write_sstable(sst_dir, manifest_.next_id(), oldest, config_);

        TableMeta meta;
        meta.id = r.id;
        meta.file_name = r.file_name;
        meta.min_key = r.min_key;
        meta.max_key = r.max_key;
        meta.min_seqno = r.min_seqno;
        meta.max_seqno = r.max_seqno;
        meta.created_at = utc_now_iso8601();
        meta.file_size = r.file_size;
        meta.entries = r.entries;
        meta.bloom_bits_per_key = r.bloom_bits_per_key;
        meta.bloom_hashes = r.bloom_hashes;
        manifest_.add_table(meta);         // atomic manifest update (3.7); epoch += 1
        readers_.insert(readers_.begin(),  // newest first, matching the manifest
                        std::make_shared<TableReader>(sst_dir / r.file_name, meta, config_));
        immutables_.pop_back();            // reclaim RAM (3.4 step 10)
        publish_version("flush");          // readers see the swap atomically (5.5)

        // Watermark cleanup: memtables are strictly seqNo-ordered, so every
        // WAL record at or below the flushed max seqNo is covered by an
        // SSTable (or superseded within one).
        for (const auto& name : wal_->remove_segments_covered(manifest_.max_seqno())) {
            std::cout << "wal: deleted " << name << " (covered by flushed SSTables)\n";
        }

        results.push_back(std::move(r));
    }

    maybe_auto_compact();
    return results;
}

void Engine::maybe_auto_compact() {
    if (compaction_paused()) return;
    // Guard against a picker that can't shrink the set any further.
    std::size_t previous = SIZE_MAX;
    while (manifest_.tables().size() >= config_.l0_compaction_trigger &&
           manifest_.tables().size() < previous) {
        previous = manifest_.tables().size();
        if (!run_compaction(std::nullopt)) break;
    }
}

bool Engine::compaction_paused() const {
    return fs::exists(fs::path(config_.data_dir) / "compaction.paused");
}

void Engine::set_compaction_paused(bool paused) {
    const fs::path flag = fs::path(config_.data_dir) / "compaction.paused";
    std::error_code ec;
    if (paused) {
        std::ofstream(flag.string()).close();
    } else {
        fs::remove(flag, ec);
    }
}

std::optional<CompactionJobResult>
Engine::run_compaction(const std::optional<std::vector<std::uint64_t>>& inputs) {
    ensure_open();
    const fs::path sst_dir = config_.resolved_sst_dir();
    const auto& live = manifest_.tables();   // newest first

    // Choose the input set.
    std::vector<std::uint64_t> ids;
    if (inputs) {
        ids = *inputs;
        if (ids.size() < 2) {
            throw Error(ErrorCode::InvalidArgument, "compaction needs at least 2 input tables");
        }
        for (std::uint64_t id : ids) {
            if (std::none_of(live.begin(), live.end(),
                             [&](const TableMeta& t) { return t.id == id; })) {
                throw Error(ErrorCode::InvalidArgument,
                            "table id " + std::to_string(id) + " is not a live SSTable");
            }
        }
        // The set must be adjacent in newest->oldest order, otherwise the
        // output's seqNo range would interleave with untouched tables and
        // break first-hit-wins reads.
        std::vector<std::size_t> positions;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (std::find(ids.begin(), ids.end(), live[i].id) != ids.end()) {
                positions.push_back(i);
            }
        }
        for (std::size_t i = 1; i < positions.size(); ++i) {
            if (positions[i] != positions[i - 1] + 1) {
                throw Error(ErrorCode::InvalidArgument,
                            "input tables must be adjacent in seqNo order "
                            "(see list-sst; pick a contiguous run)");
            }
        }
        // Normalize to manifest (newest-first) order.
        ids.clear();
        for (std::size_t p : positions) ids.push_back(live[p].id);
    } else {
        const auto pick = pick_compaction(live, config_);
        if (!pick) return std::nullopt;
        ids = pick->ids;
        std::cout << "picker: chose [";
        for (std::size_t i = 0; i < ids.size(); ++i) {
            std::cout << (i ? "," : "") << ids[i];
        }
        std::cout << "] reason=" << pick->reason
                  << " total_bytes=" << pick->total_bytes << '\n';
    }

    const auto start = std::chrono::steady_clock::now();
    CompactionState state = load_compaction_state(config_.data_dir);
    CompactionJobResult job;
    job.job_id = state.next_job_id;
    job.input_ids = ids;

    // Gather input metadata (newest-first in ids).
    std::vector<TableMeta> input_meta;
    for (std::uint64_t id : ids) {
        auto it = std::find_if(live.begin(), live.end(),
                               [&](const TableMeta& t) { return t.id == id; });
        input_meta.push_back(*it);
        job.bytes_in += it->file_size;
    }

    // Tombstone drop rule (6.6): a tombstone may go only if (a) its source
    // table is older than the grace period and (b) no live table OUTSIDE
    // the set could hold an older version it still hides. Ranges are
    // disjoint, so (b) means every outside table is entirely newer.
    std::uint64_t outside_oldest_min_seqno = UINT64_MAX;
    for (const auto& t : live) {
        if (std::find(ids.begin(), ids.end(), t.id) == ids.end()) {
            outside_oldest_min_seqno = std::min(outside_oldest_min_seqno, t.min_seqno);
        }
    }
    const std::int64_t now = std::time(nullptr);
    auto source_created_at = [&](std::uint64_t seqno) -> std::int64_t {
        for (const auto& t : input_meta) {
            if (seqno >= t.min_seqno && seqno <= t.max_seqno) {
                return parse_iso8601_utc(t.created_at);
            }
        }
        return now;   // unknown => treat as brand new (never dropped)
    };

    // Merge: apply entries oldest table -> newest into a scratch memtable;
    // Memtable::apply overwrites, so the highest seqNo per key wins (6.5).
    Memtable merged(config_.memtable_size_overhead_bytes_per_entry);
    for (auto it = input_meta.rbegin(); it != input_meta.rend(); ++it) {
        scan_sstable(sst_dir / it->file_name,
                     [&](std::string_view key, std::string_view value,
                         std::uint64_t seqno, bool tombstone) {
                         job.keys_in += 1;
                         merged.apply(key, value, seqno, tombstone);
                     });
    }

    Memtable final_table(config_.memtable_size_overhead_bytes_per_entry);
    for (const auto& [key, entry] : merged.sorted_entries()) {
        if (entry.tombstone) {
            const bool aged = now - source_created_at(entry.seqno) >
                              static_cast<std::int64_t>(config_.tombstone_grace_seconds);
            const bool nothing_older_outside = outside_oldest_min_seqno > entry.seqno;
            if (aged && nothing_older_outside) {
                job.tombstones_dropped += 1;
                continue;
            }
            job.tombstones_kept += 1;
        }
        final_table.apply(key, entry.value, entry.seqno, entry.tombstone);
    }
    job.keys_out = final_table.entries();
    job.keys_dropped = job.keys_in - job.keys_out;

    // Write the output (same safety dance as flushing, 6.7) and apply ONE
    // manifest edit. An all-dropped merge publishes a remove-only edit.
    const TableMeta* added = nullptr;
    TableMeta out_meta;
    if (final_table.entries() > 0) {
        SstBuildResult r = write_sstable(sst_dir, manifest_.next_id(), final_table, config_);
        out_meta.id = r.id;
        out_meta.file_name = r.file_name;
        out_meta.min_key = r.min_key;
        out_meta.max_key = r.max_key;
        out_meta.min_seqno = r.min_seqno;
        out_meta.max_seqno = r.max_seqno;
        out_meta.created_at = utc_now_iso8601();
        out_meta.file_size = r.file_size;
        out_meta.entries = r.entries;
        out_meta.bloom_bits_per_key = r.bloom_bits_per_key;
        out_meta.bloom_hashes = r.bloom_hashes;
        added = &out_meta;
        job.bytes_out = r.file_size;
        job.output_file = r.file_name;
    }
    manifest_.apply_compaction(ids, added);

    // Rebuild the reader list from the manifest, reusing untouched handles.
    std::vector<std::shared_ptr<TableReader>> new_readers;
    std::vector<std::shared_ptr<TableReader>> removed;
    for (const auto& t : manifest_.tables()) {
        auto it = std::find_if(readers_.begin(), readers_.end(),
                               [&](const auto& r) { return r->meta().id == t.id; });
        if (it != readers_.end()) {
            new_readers.push_back(*it);
        } else {
            new_readers.push_back(std::make_shared<TableReader>(sst_dir / t.file_name, t, config_));
        }
    }
    for (const auto& r : readers_) {
        if (std::find(ids.begin(), ids.end(), r->meta().id) != ids.end()) {
            removed.push_back(r);
        }
    }
    readers_ = std::move(new_readers);
    publish_version("compaction");

    // Delete inputs only when no Version references them any more (6.7).
    // Single-threaded today, so after the publish the refcount is ours alone.
    for (auto& r : removed) {
        const std::string name = r->meta().file_name;
        if (r.use_count() == 1) {
            r.reset();   // closes the file handle
            std::error_code del_ec;
            fs::remove(sst_dir / name, del_ec);
            std::cout << "sst: deleted " << name << " (compacted)\n";
        } else {
            std::cout << "sst: deletion of " << name
                      << " deferred (still referenced); startup will reclaim it\n";
        }
    }

    // Coarse IO throttle (6.8): enforce a minimum duration for the job.
    if (config_.compaction_io_mb_per_s > 0) {
        const double budget_ms = static_cast<double>(job.bytes_in + job.bytes_out) /
                                 (static_cast<double>(config_.compaction_io_mb_per_s) * 1024 * 1024) *
                                 1000.0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed < budget_ms) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<std::int64_t>(budget_ms) - elapsed));
        }
    }

    job.duration_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());

    state.next_job_id += 1;
    state.last_job = job;
    state.last_job_finished_at = utc_now_iso8601();
    save_compaction_state(config_.data_dir, state);

    std::cout << format_job_line(job) << '\n';
    return job;
}

void Engine::close() {
    if (closed_) return;
    if (wal_) {
        wal_->close();
    }
    // Unflushed immutables may remain at close; the WAL still covers them
    // (their segments are only deleted after a successful flush).
    closed_ = true;
}

WalStats Engine::wal_stats() const {
    ensure_open();
    return wal_->stats();
}

MemtableStats Engine::memtable_stats() const {
    ensure_open();
    MemtableStats s;
    s.active_entries = active_->entries();
    s.active_bytes = active_->bytes();
    s.immutables_count = immutables_.size();
    for (const auto& imm : immutables_) {
        s.immutables_bytes_total += imm->bytes();
    }
    s.flush_requested = flush_requested_;
    return s;
}

SstStats Engine::sst_stats() const {
    SstStats s;
    s.sst_count = manifest_.tables().size();
    s.sst_total_bytes = manifest_.total_bytes();
    if (!manifest_.tables().empty()) {
        s.newest_id = manifest_.tables().front().id;   // newest first
    }
    return s;
}

} // namespace lsm
