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

Engine::~Engine() {
    stop_workers();
}

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
    {
        std::lock_guard st(state_mutex_);
        publish_version("startup");
    }
    recovery.epoch = manifest_.epoch();
    recovery.version_published = current_version_.load()->id;
    return recovery;
}

void Engine::start_background() {
    if (workers_running_) return;
    stop_workers_ = false;
    workers_running_ = true;
    flush_thread_ = std::thread([this] { flush_worker_loop(); });
    compact_thread_ = std::thread([this] { compaction_worker_loop(); });
}

void Engine::stop_workers() {
    if (!workers_running_) return;
    stop_workers_ = true;
    state_cv_.notify_all();
    if (flush_thread_.joinable()) flush_thread_.join();
    if (compact_thread_.joinable()) compact_thread_.join();
    workers_running_ = false;
}

void Engine::flush_worker_loop() {
    while (!stop_workers_) {
        bool had_work = false;
        {
            std::unique_lock st(state_mutex_);
            state_cv_.wait_for(st, std::chrono::milliseconds(config_.bg_tick_ms),
                               [&] { return stop_workers_.load() || !immutables_.empty(); });
            had_work = !immutables_.empty();
        }
        if (stop_workers_ && !had_work) break;
        if (had_work) {
            try {
                flush_one();
            } catch (const Error& e) {
                // 7.8: keep the immutable queued; it is retried next tick.
                std::cerr << "flush: failed (" << to_string(e.code()) << "): "
                          << e.what() << "; will retry\n";
            }
        }
        if (stop_workers_) break;
    }
}

void Engine::compaction_worker_loop() {
    while (!stop_workers_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.bg_tick_ms));
        if (stop_workers_ || cancel_compaction_ || compaction_paused()) continue;

        // Flush beats compaction (7.3): yield while flush work is queued.
        {
            std::lock_guard st(state_mutex_);
            if (!immutables_.empty()) continue;
            const auto pick = pick_compaction(manifest_.tables(), config_);
            // Periodic tick only acts on a genuinely size-similar set; the
            // fallback pick is reserved for the l0 trigger, otherwise the
            // worker would grind every table set down to fan_in-1 files.
            const bool triggered = manifest_.tables().size() >= config_.l0_compaction_trigger;
            if (!pick || (!triggered && pick->reason != "size-similar window")) {
                continue;
            }
        }
        try {
            run_compaction(std::nullopt);
        } catch (const Error& e) {
            // 7.8: abandon the job; inputs remain live. Retried next tick.
            std::cerr << "compact: failed (" << to_string(e.code()) << "): "
                      << e.what() << '\n';
        }
    }
}

// Caller must hold state_mutex_.
void Engine::publish_version(const char* reason) {
    auto v = std::make_shared<Version>();
    v->id = ++version_id_;
    v->epoch = manifest_.epoch();
    v->active = active_;
    v->immutables = immutables_;
    v->tables = readers_;
    const std::uint64_t id = v->id;
    const std::uint64_t epoch = v->epoch;
    current_version_.store(std::move(v));   // atomic swap: readers holding
                                            // the old Version keep using it (5.5)
    bg_.versions_published_total += 1;
    if (config_.publish_log_level == LogLevel::Debug) {
        std::cerr << "publish: version=" << id << " epoch=" << epoch
                  << " immutables=" << immutables_.size()
                  << " sst=" << readers_.size()
                  << " reason=" << reason << '\n';
    }
}

void Engine::ensure_open() const {
    if (closed_ || shutting_down_) {
        throw Error(ErrorCode::StoreClosed, "store is closed");
    }
    if (!wal_) {
        throw Error(ErrorCode::StoreClosed, "store is not open (call open() first)");
    }
}

// Backpressure (2.6 / 7.6). With background workers running, a full
// immutable queue BLOCKS the writer until the FlushWorker frees a slot;
// without workers (one-shot CLI) it throws, as in earlier sections.
void Engine::ensure_write_capacity() {
    std::unique_lock st(state_mutex_);

    // Optional write stall (6.8): too many SSTables means compaction has
    // fallen far behind; refuse writes loudly until it catches up.
    if (manifest_.tables().size() > config_.l0_stop_writes) {
        std::cerr << "WRITE STALL: sstables=" << manifest_.tables().size()
                  << " > l0_stop_writes=" << config_.l0_stop_writes
                  << "; run compaction-run\n";
        throw Error(ErrorCode::Backpressure,
                    "too many SSTables; writes stalled until compaction runs");
    }

    if (active_->bytes() < config_.memtable_max_bytes ||
        immutables_.size() < config_.max_immutable_tables) {
        return;
    }

    std::cerr << "stall: immutables=" << immutables_.size()
              << " (max=" << config_.max_immutable_tables << ") blocking writes\n";
    bg_.write_stalls_total += 1;

    if (!workers_running_) {
        throw Error(ErrorCode::Backpressure,
                    "memtable full and immutable tables at limit; run flush-now");
    }
    const auto t0 = std::chrono::steady_clock::now();
    state_cv_.wait(st, [&] {
        return shutting_down_.load() || stop_workers_.load() ||
               immutables_.size() < config_.max_immutable_tables;
    });
    bg_.stall_time_ms_total += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count());
    if (shutting_down_ || stop_workers_) {
        throw Error(ErrorCode::StoreClosed, "store shutting down during write stall");
    }
}

std::uint64_t Engine::put(std::string_view key, std::string_view value) {
    std::lock_guard writer(write_mutex_);   // single-writer rule (7.4)
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
    std::lock_guard writer(write_mutex_);   // single-writer rule (7.4)
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
    const std::shared_ptr<const Version> v = current_version_.load();

    // Global read order (4.3 C): active -> immutables (newest->oldest) ->
    // SSTables (newest->oldest). First hit wins; a tombstone means NOT FOUND
    // and stops the search.
    if (const auto e = v->active->find(key)) {
        t.memtable_hit = true;
        return e->tombstone ? std::nullopt : std::optional<std::string>(e->value);
    }
    for (const auto& imm : v->immutables) {      // stored newest-first
        t.immutables_consulted += 1;
        if (const auto e = imm->find(key)) {
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

    std::lock_guard st(state_mutex_);
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
        std::cerr << "rotate: active->immutable size=" << immutables_.front()->bytes()
                  << " immutables=" << immutables_.size() << '\n';
        publish_version("rotation");
        state_cv_.notify_all();   // wake the FlushWorker
    }
}

// Flush the oldest immutable memtable, if any. Serialized against
// compaction by creation_mutex_; safe to call from the FlushWorker and the
// CLI at the same time. Returns false when nothing was pending.
bool Engine::flush_one() {
    std::lock_guard creation(creation_mutex_);
    const fs::path sst_dir = config_.resolved_sst_dir();
    const auto t0 = std::chrono::steady_clock::now();

    std::shared_ptr<Memtable> target;
    std::uint64_t out_id = 0;
    {
        std::lock_guard st(state_mutex_);
        if (immutables_.empty()) return false;
        target = immutables_.back();      // oldest; stays visible to readers
        out_id = manifest_.next_id();
    }

    bg_.flush_running = true;
    std::cerr << "flush: start bytes=" << target->bytes() << '\n';
    SstBuildResult r;
    try {
        r = write_sstable(sst_dir, out_id, *target, config_);
    } catch (...) {
        bg_.flush_running = false;
        throw;
    }

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

    {
        std::lock_guard st(state_mutex_);
        manifest_.add_table(meta);         // atomic manifest update (3.7); epoch += 1
        readers_.insert(readers_.begin(),  // newest first, matching the manifest
                        std::make_shared<TableReader>(sst_dir / r.file_name, meta, config_));
        immutables_.pop_back();            // reclaim RAM (3.4 step 10)
        publish_version("flush");          // readers see the swap atomically (5.5)
    }
    state_cv_.notify_all();                // free a stalled writer (7.6)

    // Watermark WAL cleanup. The Wal is internally synchronized, so this
    // never contends with a (possibly stalled) writer holding write_mutex_
    // — taking write_mutex_ here would deadlock: the stalled writer waits
    // for THIS worker to free an immutable slot.
    std::uint64_t watermark;
    {
        std::lock_guard st(state_mutex_);
        watermark = manifest_.max_seqno();
    }
    for (const auto& name : wal_->remove_segments_covered(watermark)) {
        std::cout << "wal: deleted " << name << " (covered by flushed SSTables)\n";
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    bg_.flush_jobs_total += 1;
    bg_.flush_last_ms = static_cast<std::uint64_t>(ms);
    bg_.flush_running = false;
    std::cerr << "flush: publish sst=" << r.file_name << " bytes=" << r.file_size
              << " dur=" << ms << "ms\n";
    return true;
}

std::vector<SstBuildResult> Engine::flush_pending() {
    ensure_open();
    std::vector<SstBuildResult> results;
    while (true) {
        std::uint64_t before;
        {
            std::lock_guard st(state_mutex_);
            before = immutables_.size();
        }
        if (before == 0) break;
        if (!flush_one()) break;
        // Report the table that just landed (front of manifest).
        SstBuildResult r;
        {
            std::lock_guard st(state_mutex_);
            const TableMeta& t = manifest_.tables().front();
            r.id = t.id;
            r.file_name = t.file_name;
            r.file_size = t.file_size;
            r.min_key = t.min_key;
            r.max_key = t.max_key;
            r.min_seqno = t.min_seqno;
            r.max_seqno = t.max_seqno;
            r.entries = t.entries;
            r.bloom_bits_per_key = t.bloom_bits_per_key;
            r.bloom_hashes = t.bloom_hashes;
        }
        results.push_back(std::move(r));
    }

    maybe_auto_compact();
    return results;
}

void Engine::maybe_auto_compact() {
    if (compaction_paused()) return;
    auto table_count = [&] {
        std::lock_guard st(state_mutex_);
        return manifest_.tables().size();
    };
    // Guard against a picker that can't shrink the set any further.
    std::size_t previous = SIZE_MAX;
    while (table_count() >= config_.l0_compaction_trigger && table_count() < previous) {
        previous = table_count();
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
    // Serialized against flushes and other compactions (7.4 in-progress
    // rule: a file can never be in two jobs).
    std::lock_guard creation(creation_mutex_);
    bg_.compact_running = true;
    struct RunningGuard {
        std::atomic<bool>& flag;
        ~RunningGuard() { flag = false; }
    } running_guard{bg_.compact_running};

    const fs::path sst_dir = config_.resolved_sst_dir();
    std::vector<TableMeta> live;   // snapshot, newest first
    {
        std::lock_guard st(state_mutex_);
        live = manifest_.tables();
    }

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

    // Cancel checkpoint (7.7 fast shutdown): nothing published yet, inputs
    // stay live; the job is simply abandoned.
    if (cancel_compaction_) {
        std::cerr << "compact: job=" << job.job_id << " cancelled before output write\n";
        return std::nullopt;
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
        std::uint64_t out_id;
        {
            std::lock_guard st(state_mutex_);
            out_id = manifest_.next_id();
        }
        SstBuildResult r = write_sstable(sst_dir, out_id, final_table, config_);
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
    // Final cancel checkpoint before anything becomes visible.
    if (cancel_compaction_) {
        if (added) {
            std::error_code del_ec;
            fs::remove(sst_dir / out_meta.file_name, del_ec);   // unpublished orphan
        }
        std::cerr << "compact: job=" << job.job_id << " cancelled before manifest edit\n";
        return std::nullopt;
    }

    std::vector<std::shared_ptr<TableReader>> removed;
    {
        std::lock_guard st(state_mutex_);
        manifest_.apply_compaction(ids, added);

        // Rebuild the reader list from the manifest, reusing untouched handles.
        std::vector<std::shared_ptr<TableReader>> new_readers;
        for (const auto& t : manifest_.tables()) {
            auto it = std::find_if(readers_.begin(), readers_.end(),
                                   [&](const auto& r) { return r->meta().id == t.id; });
            if (it != readers_.end()) {
                new_readers.push_back(*it);
            } else {
                new_readers.push_back(
                    std::make_shared<TableReader>(sst_dir / t.file_name, t, config_));
            }
        }
        for (const auto& r : readers_) {
            if (std::find(ids.begin(), ids.end(), r->meta().id) != ids.end()) {
                removed.push_back(r);
            }
        }
        readers_ = std::move(new_readers);
        publish_version("compaction");
    }

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

    bg_.compactions_total += 1;
    bg_.compaction_last_ms = job.duration_ms;
    std::cout << format_job_line(job) << '\n';
    return job;
}

void Engine::close(bool graceful) {
    if (closed_) return;
    shutting_down_ = true;             // new writes now fail with StoreClosed
    state_cv_.notify_all();            // release any stalled writer

    if (workers_running_) {
        if (graceful) {
            // 7.7 graceful: drain every queued flush; let the running
            // compaction finish (creation_mutex_ waits for it), no new jobs.
            while (flush_one()) {}
        } else {
            // 7.7 fast: cancel compaction at its next checkpoint; leave
            // queued immutables in RAM — the WAL still covers them.
            cancel_compaction_ = true;
        }
        stop_workers();
    }
    // With workers off (plain one-shot commands), queued immutables simply
    // stay in RAM/WAL as in earlier sections — replay rebuilds them.

    if (wal_) {
        wal_->close();                 // finish appends, fsync, close segment
    }
    closed_ = true;
}

WalStats Engine::wal_stats() const {
    ensure_open();
    return wal_->stats();
}

MemtableStats Engine::memtable_stats() const {
    ensure_open();
    std::lock_guard st(state_mutex_);
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
    std::lock_guard st(state_mutex_);
    SstStats s;
    s.sst_count = manifest_.tables().size();
    s.sst_total_bytes = manifest_.total_bytes();
    if (!manifest_.tables().empty()) {
        s.newest_id = manifest_.tables().front().id;   // newest first
    }
    return s;
}

} // namespace lsm
