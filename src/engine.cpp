#include "lsm/engine.hpp"

#include "lsm/errors.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
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
    return results;
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
