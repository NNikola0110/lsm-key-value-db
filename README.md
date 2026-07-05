# lsmkv

A single-node LSM (Log-Structured Merge) key-value store — Phase A.
Language: **C++23**. Build: **CMake**.

Progress: **Section 7 (concurrency & scheduling)** is implemented. The engine
is now multi-threaded: a mutex-serialized writer, lock-free readers (atomic
Version snapshots), a background **FlushWorker** and **CompactionWorker**,
blocking write backpressure, and graceful/fast shutdown. Long-running modes:
`lsmkv repl` (interactive) and `lsmkv stress` (concurrent hammer test).
One-shot commands keep workers off and stay deterministic.
Next up: observability & tooling (8).

## Build

Requires a C++23 compiler, CMake ≥ 3.24, and internet on first configure
(CMake fetches `nlohmann/json`).

```sh
cmake -S . -B build
cmake --build build
```

The executable is produced at `build/lsmkv` (or `build/Debug/lsmkv.exe` with MSVC).

## Run

```sh
build/lsmkv init                       # create data/ + data/wal/, print resolved config
build/lsmkv put --key foo --value bar  # WAL append + memtable; prints: ok seqno=N memtable_bytes=B
build/lsmkv del --key foo              # WAL append + tombstone; prints: ok seqno=N memtable_bytes=B
build/lsmkv get --key foo              # memtables then SSTables; value or NOT FOUND
build/lsmkv get --key foo --log-level debug   # adds a per-lookup trace line
build/lsmkv probe --key foo --repeat 1000     # hot-key loop: shows cache hits
build/lsmkv probe --absent 1000               # absent keys: shows bloom skips
build/lsmkv sst-info --file 000001.sst        # footer, index, blocks, bloom details
build/lsmkv manifest-info                     # epoch + live table listing (newest first)
build/lsmkv version-info                      # current Version: id/epoch, immutables, SSTs
build/lsmkv compaction-run                    # run one job (picker chooses inputs)
build/lsmkv compaction-run --files 3,2,1      # compact an explicit adjacent set
build/lsmkv compaction-stats                  # backlog, next pick, last job summary
build/lsmkv compaction-pause                  # flag file blocks picker + auto runs
build/lsmkv compaction-resume
build/lsmkv repl                              # interactive session, workers running
build/lsmkv stress --threads 4 --ops 3000 --read-pct 40   # concurrent load test
build/lsmkv bg-status                         # worker states, queues, job totals
build/lsmkv shutdown            # graceful: drain flushes, finish compaction
build/lsmkv shutdown --fast     # cancel compaction, leave immutables to the WAL
build/lsmkv stats                      # config + WAL stats + memtable stats
build/lsmkv close                      # finish appends, sync, exit cleanly
build/lsmkv flush-now                  # flush pending immutables to data/sst/
build/lsmkv list-sst                   # live SSTables: id, size, key/seqno ranges, bloom
build/lsmkv verify-sst --file 000001.sst   # footer magic + all block checksums
build/lsmkv wal-verify                 # read-only scan, prints a recovery report
build/lsmkv wal-truncate --segment 000003.wal --offset 987654   # manual repair tool
```

Every command that opens the engine first replays the WAL — rebuilding the
memtable — and prints two recovery lines, e.g.:

```
recovery: segments=3 records=10523 truncated=1 last_seqno=10523 status=OK
recovery: memtable_keys=8712 memtable_bytes=1204233 last_seqno=10523
```

`--config PATH` overrides the config file (default: `config/default.json`).
Missing `--key` exits with a non-zero code.

## WAL policies (Section 1)

- **Sync policy:** `wal_fsync_every_n` — fsync after every Nth record (default
  1 = after every record) and always on clean close. With N>1, a hard crash may
  lose up to the last N−1 acknowledged-but-unsynced records; that trade-off is
  the point of the knob.
- **Roll policy:** size-based — a new segment (`000001.wal`, `000002.wal`, …)
  starts once the active one would exceed `wal_segment_roll_bytes`
  (default 128 MiB).
- **Recovery:** segments are scanned oldest→newest; a partial or garbled record
  is treated as tail corruption — the file is truncated to the last good byte
  and startup continues. The seqNo counter resumes at `max(seqNo)+1`.
- **Record framing** (little-endian):
  `payload_len u32 | payload | crc32(payload) u32` where
  `payload = type u8 (1=PUT, 2=DEL) | seqno u64 | key_len u32 | value_len u32 | key | value`.
  Each segment starts with an 8-byte header: magic `LSMW`, version `1`,
  3 reserved bytes.

## Memtable policies (Section 2)

- **Size accounting:** approximate entry size = key length + value length +
  `memtable_size_overhead_bytes_per_entry` (default 32).
- **Rotation:** when the active memtable reaches `memtable_max_bytes`
  (default 64 MiB) it is frozen into the immutable list and a fresh active
  table is created; a flush request is queued for Section 3.
- **Backpressure (Option A — block):** if rotation would exceed
  `max_immutable_tables` (default 4), it prints
  `backpressure: immutables=N (max=M); blocking writes` and refuses the write
  with a `Backpressure` error. Since the CLI is one-shot and no flusher exists
  yet, "blocking" surfaces as a refused write; Section 3 turns this into a
  real wait.
- **Recovery:** WAL replay applies the same write-path rules as live
  mutations (including rotation), so the table set after a restart matches
  what existed before the crash. Replay itself never blocks — it warns once
  and defers rotation if the immutable limit is hit.
- **Concurrency model:** single writer, many readers by design; the current
  one-shot CLI is single-threaded, so no locks exist yet (Section 7 adds
  scheduling).

## SSTable & flush policies (Section 3)

- **File format** (documented in [sstable.hpp](include/lsm/sstable.hpp)):
  data blocks (~`block_size`, restart point every `restart_interval` entries,
  per-block CRC32) → sparse index block → Bloom filter block → 40-byte footer
  with offsets, version, and magic `LSST`. No delta encoding — full keys are
  stored; restart points enable in-block binary search later.
- **Flush** keeps only the latest version per key and **keeps tombstones**
  (compaction, Section 6, decides when to drop them). Files are written as
  `.sst.tmp`, fsynced, then atomically renamed; the manifest
  (`data/manifest.json`) is rewritten via the same tmp+fsync+rename dance.
  Leftover `.tmp` files are deleted on startup; a manifest entry whose file
  is missing halts with `CorruptionDetected`.
- **WAL cleanup — watermark policy (3.8):** the manifest's highest flushed
  seqNo is the persisted watermark; any non-active WAL segment whose max
  seqNo ≤ watermark is fully covered by SSTables and is deleted after a
  flush. Memtables are strictly seqNo-ordered, which makes this exact. A live
  rotation also rolls the WAL segment so frozen data stops sharing a segment
  with new writes, keeping cleanup prompt.
- **seqNo continuity:** after cleanup the WAL may hold no records, so the
  seqNo counter is seeded from max(WAL replay, manifest watermark) — it never
  runs backwards.
- **Compression:** `snappy`/`lz4` are accepted in config but not linked into
  this build; the writer warns and writes uncompressed.

## Versioning (Section 5)

- **Manifest** (`data/manifest.json`, override with `manifest_path`): the
  durable source of truth. Carries `manifest_version`, a monotonic `epoch`
  (+1 per change), `next_sst_id`, and the table list **newest→oldest** — the
  same order the read path needs. Updates stay atomic (tmp+fsync+rename);
  a leftover `manifest.json.tmp` is ignored and removed on startup.
- **Version** (in memory): an immutable snapshot of {active memtable,
  immutables newest→oldest, SSTable handles newest→oldest, epoch, id}.
  Rotation and flush build a new Version and publish it with one pointer
  swap; `get` grabs the current Version once and reads only through it.
  `shared_ptr` refcounts keep an old Version (and every table it references)
  alive until its last reader lets go — that is the refcount rule from 5.5.
  (The CLI is single-threaded today, so the swap is trivially atomic; the
  discipline is what Section 7's background threads will rely on.)
- **Startup (5.6):** load manifest → open+validate table handles → replay WAL
  into the memtable → publish the first Version. Printed as
  `startup: manifest_tables=N epoch=E wal_records=R version_published=V`
  (version id = epoch + 1 at startup). Orphan `.sst` files not in the
  manifest are ignored, never half-visible. A corrupt manifest fails fast
  with `CorruptionDetected` naming the file.
- Set `publish_log_level=debug` (or `LSMKV_PUBLISH_LOG_LEVEL=debug`) to log
  a one-liner for every publish.

## Concurrency & scheduling (Section 7)

- **Thread model (7.2/7.4):** writes (`put`/`del`) are serialized by a single
  write mutex — WAL append, memtable update, rotation. Readers never take
  the write lock: `get` atomically loads the current Version and reads only
  inside it (memtables use a shared_mutex; each SSTable reader serializes
  its own file I/O; the block cache and FD pool have internal locks).
- **Workers:** the FlushWorker drains the immutable queue (woken instantly
  on rotation, else every `bg_tick_ms`); the CompactionWorker ticks every
  `bg_tick_ms` and yields whenever flush work is queued ("flush beats
  compaction"). A periodic tick only acts on size-similar picks; the
  fallback pick requires the `l0_compaction_trigger`. SSTable creation is
  serialized by one lock, so a file can never be in two jobs.
- **Backpressure (7.6):** with workers running, a full immutable queue
  *blocks* the writer (`stall: immutables=N (max=M) blocking writes`) until
  a flush frees a slot; stall counts/time appear in `bg-status`. Without
  workers (one-shot commands) it throws `Backpressure` as before. The
  `l0_stop_writes` stall still rejects with an error in both modes.
- **Shutdown (7.7):** `graceful` drains every queued flush and lets a
  running compaction finish; `fast` cancels compaction at its next
  checkpoint (inputs stay live, unpublished output deleted) and leaves
  queued immutables in RAM — the WAL covers them and replay rebuilds them.
- **Failure policy (7.8):** a failed flush keeps its immutable queued and
  retries next tick; a failed compaction abandons the job with inputs live.
- **Lock-ordering rule** (learned the hard way — the first version
  deadlocked): the WAL is internally synchronized and background workers
  never take the write mutex; a stalled writer *holds* the write mutex
  while waiting for the FlushWorker, so any worker that needed it would
  deadlock the engine. FD-pool eviction closes victims with try-lock only,
  for the same reason.
- One-shot CLI commands leave workers **off** — deterministic, exactly the
  Section 2–6 behavior. `repl`, `stress`, and `shutdown` run them.

## Compaction (Section 6)

- **Picker (deterministic):** tables are considered newest→oldest (manifest
  order) and only **adjacent windows** of `size_tiered_fan_in` tables are
  candidates. A window qualifies when its largest file is within
  `size_tiered_size_ratio` (2.0) of its smallest; the qualifying window with
  the fewest bytes wins, else the smallest window is picked as a fallback.
  *Why adjacency (a deliberate deviation from the spec's pure size-sort):*
  merging non-adjacent tables would give the output a seqNo range that
  interleaves with untouched tables, breaking the read path's
  first-hit-wins rule. Adjacent merges keep all live seqNo ranges disjoint,
  so ordering tables by `max_seqno` stays correct forever. (The manifest is
  therefore sorted by `max_seqno`, not id — after compaction, ids no longer
  track data age.) `compaction-run --files` enforces the same adjacency.
- **Merge:** inputs are streamed oldest→newest into a scratch memtable, so
  the highest seqNo per key wins; only the latest version survives. The
  output is written with the exact Section 3 safety dance (tmp → fsync →
  rename → one atomic manifest edit) and published as a new Version.
- **Tombstones:** dropped only if (a) the source table is older than
  `tombstone_grace_seconds` (24 h default; table `created_at` is the age
  proxy since entries carry no timestamps) and (b) every live table outside
  the input set is entirely newer — so nothing older can be resurrected.
- **File deletion:** inputs are deleted when no Version references them
  (refcount rule); anything missed becomes an orphan that startup deletes
  (a `.sst` not in the manifest — also exactly what a crash between rename
  and manifest edit leaves behind).
- **Steering:** flushes auto-kick compaction while live tables ≥
  `l0_compaction_trigger`; writes stall with a loud warning above
  `l0_stop_writes`; `compaction_io_mb_per_s` (0 = off) enforces a coarse
  minimum job duration; pause/resume is a flag file
  (`data/compaction.paused`) so it survives restarts. Job stats persist in
  `data/compaction_stats.json` for `compaction-stats`.

## Read path (Section 4)

- **Lookup order:** active memtable → immutables (newest→oldest) → SSTables
  (newest→oldest). First hit wins; a tombstone means NOT FOUND and stops the
  search. Per table: range check (manifest min/max key) → Bloom check →
  index binary search → one data-block read → restart-point search in-block.
- **Block cache:** LRU over checksum-verified data blocks, capacity
  `block_cache_mb` (64 MiB default). Index blocks are pinned per table when
  `cache_index_blocks=true`; Bloom filters are always pinned once loaded.
- **FD budget:** at most `max_open_files` SSTables keep an open handle; the
  least-recently-used reader is closed when the budget is exceeded.
- **Corruption:** a failed block checksum or bad footer raises
  `CorruptionDetected` naming the file and offset; the request fails, the
  process does not crash.
- **Stats/tracing:** `stats` prints `read.*` counters (bloom checks/negatives,
  cache hits/misses, disk reads); `get --log-level debug` prints a per-lookup
  trace. Counters are per-process (the CLI is one-shot), so use `probe` to
  observe cache/bloom behavior over many lookups in one process.

## Layout

```
config/default.json   default tunables (see Sections 0.4 / 1.6 / 2.10 / 3.10)
data/                 runtime files: wal/ segments, sst/ tables, manifest.json
docs/                 design notes
include/lsm/          engine headers + api.md, errors.md
src/                  CLI + engine sources (wal, memtable, sstable, manifest)
tests/                test placeholders (Section 9)
```

## Configuration

Keys: `data_dir`, `memtable_max_bytes`, `max_immutable_tables`,
`memtable_size_overhead_bytes_per_entry`, `block_size`, `bloom_false_positive`,
`sst_dir` (empty = `<data_dir>/sst`), `restart_interval`,
`max_build_buffer_mb`, `block_cache_mb`, `cache_index_blocks`,
`max_open_files`, `wal_fsync_every_n`, `wal_segment_roll_bytes`,
`compression` (`off|snappy|lz4`), `log_level` (`debug|info|warn|error`),
`manifest_path` (empty = `<data_dir>/manifest.json`), `publish_log_level`,
`size_tiered_fan_in` (4 tables per merge), `size_tiered_size_ratio` (2.0 max
spread in a picked set), `tombstone_grace_seconds` (86400),
`compaction_max_concurrent` (1; the CLI runs jobs synchronously),
`compaction_io_mb_per_s` (0 = unthrottled), `l0_compaction_trigger` (8,
auto-compact threshold), `l0_stop_writes` (20, write-stall threshold),
`bg_tick_ms` (500, worker wake-up period), `shutdown_timeout_ms` (5000,
fast-shutdown grace period).

Resolution priority (later wins): built-in defaults → config file →
env vars (`LSMKV_*`) → CLI flags (currently `--log-level`). A missing config
file falls back to defaults; unknown keys print a warning and are ignored.
