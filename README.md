# lsmkv

A single-node LSM (Log-Structured Merge) key-value store — Phase A.
Language: **C++23**. Build: **CMake**.

Progress: **Section 3 (SSTable writer / flush path)** is implemented.
`put`/`del` append durably to the WAL, then update the memtable; `flush-now`
writes immutable memtables to sorted, immutable SSTables (atomic temp+rename
publish, manifest update, WAL cleanup). `get` still reads **memory only** —
SSTable reads arrive in Section 4, so flushed data is temporarily invisible
to `get` once its WAL segments are cleaned. This is the spec's intended
section ordering.

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
build/lsmkv get --key foo              # prints the value, or NOT FOUND
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
`max_build_buffer_mb`, `wal_fsync_every_n`, `wal_segment_roll_bytes`,
`compression` (`off|snappy|lz4`), `log_level` (`debug|info|warn|error`).

Resolution priority (later wins): built-in defaults → config file →
env vars (`LSMKV_*`) → CLI flags. A missing config file falls back to defaults;
unknown keys print a warning and are ignored.
