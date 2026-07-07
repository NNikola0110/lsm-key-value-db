// lsmkv unit + property tests (Section 9). No framework: each TEST is a
// function; failures print file:line and, for property tests, the seed.
// Run: lsmkv_tests [--seed N]   (default: the 5 fixed CI seeds)

#include "lsm/compaction.hpp"
#include "lsm/config.hpp"
#include "lsm/engine.hpp"
#include "lsm/errors.hpp"
#include "lsm/manifest.hpp"
#include "lsm/memtable.hpp"
#include "lsm/sstable.hpp"
#include "lsm/wal.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace fs = std::filesystem;
using namespace lsm;

namespace {

int g_failures = 0;
std::string g_current;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cout << "FAIL " << g_current << " at " << __FILE__ << ':'       \
                      << __LINE__ << ": " << #cond << '\n';                      \
            ++g_failures;                                                        \
            return;                                                              \
        }                                                                        \
    } while (0)

fs::path fresh_dir(const std::string& name) {
    const fs::path dir = fs::path("build") / "test_data" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

Config test_config(const fs::path& dir) {
    Config cfg;
    cfg.data_dir = dir.string();
    cfg.log_level = LogLevel::Error;   // keep test output readable
    cfg.memtable_max_bytes = 1 << 20;
    return cfg;
}

// ---------- WAL ----------

void test_wal_append_replay() {
    const auto dir = fresh_dir("wal1");
    {
        Wal wal(dir / "wal", 1, 1 << 20);
        wal.open();
        for (int i = 0; i < 10; ++i) wal.append_put("k" + std::to_string(i), "v");
        for (int i = 0; i < 3; ++i) wal.append_del("k" + std::to_string(i));
        wal.close();
    }
    Wal wal(dir / "wal", 1, 1 << 20);
    std::uint64_t puts = 0, dels = 0, last = 0;
    const auto report = wal.open([&](bool is_del, std::string_view, std::string_view,
                                     std::uint64_t seqno, std::uint64_t) {
        (is_del ? dels : puts) += 1;
        CHECK(seqno > last);   // Invariant 3: monotonic
        last = seqno;
    });
    CHECK(report.records == 13 && puts == 10 && dels == 3);
    CHECK(report.last_seqno == 13 && report.truncated == 0);
}

void test_wal_tail_truncation() {
    const auto dir = fresh_dir("wal2");
    {
        Wal wal(dir / "wal", 1, 1 << 20);
        wal.open();
        wal.append_put("a", "1");
        wal.append_put("b", "2");
        wal.close();
    }
    // Cut the last record roughly in half, then also test appended garbage.
    const fs::path seg = dir / "wal" / "000001.wal";
    fs::resize_file(seg, fs::file_size(seg) - 5);
    {
        Wal wal(dir / "wal", 1, 1 << 20);
        const auto report = wal.open();
        CHECK(report.records == 1 && report.truncated == 1 && report.last_seqno == 1);
        wal.close();
    }
    std::ofstream(seg, std::ios::app | std::ios::binary) << "JUNKJUNKJUNK";
    Wal wal(dir / "wal", 1, 1 << 20);
    const auto report = wal.open();
    CHECK(report.records == 1 && report.truncated == 1);
}

void test_wal_segment_roll() {
    const auto dir = fresh_dir("wal3");
    Wal wal(dir / "wal", 1, /*roll_bytes=*/64);
    wal.open();
    for (int i = 0; i < 6; ++i) wal.append_put("key" + std::to_string(i), "vvvvvvvvvv");
    CHECK(wal.stats().total_segments > 1);
    CHECK(wal.stats().last_seqno == 6);
    wal.close();
}

// ---------- Memtable ----------

void test_memtable_semantics() {
    Memtable mt(32);
    mt.apply("k", "v1", 1, false);
    mt.apply("k", "v2", 2, false);
    auto e = mt.find("k");
    CHECK(e && e->value == "v2" && e->seqno == 2);
    mt.apply("k", "", 3, true);   // delete
    e = mt.find("k");
    CHECK(e && e->tombstone);     // tombstone IS an entry (hides older)
    CHECK(!mt.find("absent"));
    CHECK(mt.entries() == 1);
    CHECK(mt.bytes() == 1 + 0 + 32);  // key + empty value + overhead
}

// ---------- SSTable writer + reader ----------

void test_sstable_roundtrip_and_edges() {
    const auto dir = fresh_dir("sst1");
    Config cfg = test_config(dir);
    cfg.block_size = 128;          // force multiple blocks
    cfg.restart_interval = 4;
    fs::create_directories(cfg.resolved_sst_dir());

    Memtable src(32);
    std::string long_key(1024, 'K');
    std::string binary_val("\x00\x01\xFF zero", 8);
    src.apply("empty-value", "", 1, false);
    src.apply(long_key, "long", 2, false);
    src.apply("naïve-ключ", "non-ascii", 3, false);        // UTF-8 key
    src.apply("bin", binary_val, 4, false);
    src.apply("dead", "", 5, true);                        // tombstone kept
    for (int i = 0; i < 40; ++i) {                         // shared prefixes
        src.apply("user:" + std::to_string(100000 + i), "u" + std::to_string(i), 6 + i, false);
    }
    const auto r = write_sstable(cfg.resolved_sst_dir(), 1, src, cfg);
    CHECK(r.entries == 45);
    CHECK(verify_sstable(cfg.resolved_sst_dir() / r.file_name) == "OK");

    // Sorted, unique keys; tombstone preserved (9.2 flush rules).
    std::string prev;
    std::uint64_t seen = 0;
    bool tomb_seen = false;
    scan_sstable(cfg.resolved_sst_dir() / r.file_name,
                 [&](std::string_view k, std::string_view v, std::uint64_t, bool tomb) {
                     if (seen) CHECK(std::string(k) > prev);
                     prev = std::string(k);
                     seen += 1;
                     if (k == "dead") tomb_seen = tomb;
                     if (k == "bin") CHECK(std::string(v) == binary_val);
                 });
    CHECK(seen == 45 && tomb_seen);
}

void test_sstable_reader_via_engine() {
    const auto dir = fresh_dir("sst2");
    Config cfg = test_config(dir);
    cfg.memtable_max_bytes = 120;   // third put trips rotation
    Engine engine(cfg);
    engine.open();
    engine.put("apple", "one");
    engine.put("banana", "two");
    engine.put("cherry", "threeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");  // forces rotation
    engine.flush_pending();
    CHECK(engine.sst_stats().sst_count >= 1);
    // Reads served from disk (memtable no longer holds flushed keys).
    CHECK(engine.get("apple") == std::optional<std::string>("one"));
    CHECK(!engine.get("absent-key"));
    // Newest-wins across table and memtable.
    engine.put("apple", "newer");
    CHECK(engine.get("apple") == std::optional<std::string>("newer"));
    engine.remove("banana");
    CHECK(!engine.get("banana"));
    engine.close();
}

void test_sstable_corruption_detected() {
    const auto dir = fresh_dir("sst3");
    Config cfg = test_config(dir);
    cfg.memtable_max_bytes = 100;
    {
        Engine engine(cfg);
        engine.open();
        engine.put("kk1", "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv");
        engine.put("kk2", "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv");
        engine.flush_pending();
        engine.close();
    }
    // Flip one byte in the first data block.
    fs::path sst;
    for (const auto& e : fs::directory_iterator(cfg.resolved_sst_dir())) sst = e.path();
    std::fstream f(sst, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(10);
    f.put('\xEE');
    f.close();

    Engine engine(cfg);
    engine.open();
    bool threw = false;
    try {
        (void)engine.get("kk1");
    } catch (const Error& e) {
        threw = e.code() == ErrorCode::CorruptionDetected;
    }
    CHECK(threw);
    engine.close();
}

// ---------- Manifest & Version ----------

void test_manifest_version() {
    const auto dir = fresh_dir("man1");
    Config cfg = test_config(dir);
    cfg.memtable_max_bytes = 150;
    std::uint64_t epoch_after_flush = 0;
    {
        Engine engine(cfg);
        const auto rec = engine.open();
        CHECK(rec.version_published == 1);
        engine.put("m1", "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv");
        engine.put("m2", "vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv");
        engine.flush_pending();
        epoch_after_flush = engine.manifest().epoch();
        CHECK(epoch_after_flush >= 1);
        CHECK(engine.current_version()->epoch == epoch_after_flush);
        CHECK(engine.current_version()->tables.size() == engine.manifest().tables().size());
        engine.close();
    }
    // Reload: epoch persisted; startup publishes epoch+1 as version id.
    Engine engine(cfg);
    const auto rec = engine.open();
    CHECK(rec.epoch == epoch_after_flush);
    CHECK(rec.version_published == epoch_after_flush + 1);
    engine.close();
}

// ---------- Compaction ----------

void test_compaction_merge() {
    const auto dir = fresh_dir("comp1");
    Config cfg = test_config(dir);
    cfg.memtable_max_bytes = 80;    // each batch fills one table
    cfg.size_tiered_fan_in = 2;
    cfg.tombstone_grace_seconds = 1u << 30;   // keep tombstones
    Engine engine(cfg);
    engine.open();
    engine.put("c1", "old-valueXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    engine.flush_pending();
    engine.put("c1", "new-value");
    engine.remove("c2");
    engine.put("pad", "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    engine.flush_pending();
    CHECK(engine.sst_stats().sst_count == 2);

    const auto job = engine.run_compaction(std::nullopt);
    CHECK(job.has_value());
    CHECK(engine.sst_stats().sst_count == 1);
    CHECK(job->tombstones_kept == 1);                       // grace not expired
    CHECK(engine.get("c1") == std::optional<std::string>("new-value"));  // newest wins
    CHECK(!engine.get("c2"));
    engine.close();
}

// ---------- Property / model-based (9.3) ----------

void property_test(std::uint64_t seed) {
    g_current = "property seed=" + std::to_string(seed);   // 9.11: print the seed
    const auto dir = fresh_dir("prop" + std::to_string(seed));
    Config cfg = test_config(dir);
    cfg.memtable_max_bytes = 600;      // frequent rotations
    cfg.max_immutable_tables = 1000;   // workers are off; we flush manually
    cfg.size_tiered_fan_in = 3;
    cfg.l0_compaction_trigger = 5;

    std::mt19937_64 rng(seed);
    std::map<std::string, std::optional<std::string>> oracle;   // nullopt = tombstoned
    std::uint64_t last_seqno = 0;

    Engine engine(cfg);
    engine.open();
    for (int op = 0; op < 600; ++op) {
        // Hot-key skew: 1/3 of ops hit an 8-key hot set.
        const bool hot = rng() % 3 == 0;
        const std::string key = (hot ? "hot" : "key") + std::to_string(rng() % (hot ? 8 : 64));
        const int kind = static_cast<int>(rng() % 10);
        if (kind < 6) {   // put (value sizes: empty, small, medium)
            const std::size_t len = static_cast<std::size_t>(rng() % 3 == 0 ? 0 : rng() % 300);
            std::string value(len, static_cast<char>('a' + op % 26));
            const std::uint64_t s = engine.put(key, value);
            CHECK(s > last_seqno);                    // Invariant 3
            last_seqno = s;
            oracle[key] = value;
        } else if (kind < 8) {   // del
            const std::uint64_t s = engine.remove(key);
            CHECK(s > last_seqno);
            last_seqno = s;
            oracle[key] = std::nullopt;               // Invariant 4: stays hidden
        } else {   // get, checked against the oracle (Invariant 1)
            const auto got = engine.get(key);
            const auto it = oracle.find(key);
            const std::optional<std::string> want =
                it == oracle.end() ? std::optional<std::string>{} : it->second;
            CHECK(got == want);
        }
        if (op % 97 == 0) engine.flush_pending();     // interleave bg work
    }
    engine.close();

    // Invariant 2 (durability): close + reopen == crash + replay with
    // wal_fsync_every_n=1 — every acknowledged op must be visible.
    Engine reopened(cfg);
    reopened.open();
    for (const auto& [key, want] : oracle) {
        CHECK(reopened.get(key) == want);
    }
    reopened.close();
}

struct Test {
    const char* name;
    std::function<void()> fn;
};

} // namespace

int main(int argc, char** argv) {
    std::vector<std::uint64_t> seeds{1, 2, 3, 4, 5};   // fixed CI seeds (9.8)
    if (argc == 3 && std::string(argv[1]) == "--seed") {
        seeds = {std::stoull(argv[2])};
    }

    std::vector<Test> tests = {
        {"wal_append_replay", test_wal_append_replay},
        {"wal_tail_truncation", test_wal_tail_truncation},
        {"wal_segment_roll", test_wal_segment_roll},
        {"memtable_semantics", test_memtable_semantics},
        {"sstable_roundtrip_and_edges", test_sstable_roundtrip_and_edges},
        {"sstable_reader_via_engine", test_sstable_reader_via_engine},
        {"sstable_corruption_detected", test_sstable_corruption_detected},
        {"manifest_version", test_manifest_version},
        {"compaction_merge", test_compaction_merge},
    };
    for (std::uint64_t seed : seeds) {
        tests.push_back({"property", [seed] { property_test(seed); }});
    }

    int ran = 0;
    for (auto& t : tests) {
        g_current = t.name;
        const int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cout << "FAIL " << t.name << " (exception): " << e.what() << '\n';
            ++g_failures;
        }
        std::cout << (g_failures == before ? "PASS " : "FAIL ") << t.name << '\n';
        ++ran;
    }
    std::cout << "----\n" << ran << " tests, " << g_failures << " failures";
    if (g_failures) {
        std::cout << "  (property seeds: reproduce with lsmkv_tests --seed <N>)";
    }
    std::cout << '\n';
    return g_failures ? 1 : 0;
}
