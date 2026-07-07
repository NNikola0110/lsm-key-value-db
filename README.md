# lsmkv

A single-node LSM (Log-Structured Merge) key-value store — Phase A.
Language: **C++23**. Build: **CMake**.

Progress: **Section 2 (Memtable)** is implemented. `put`/`del` append durably
to the write-ahead log, then update an ordered in-memory memtable; `get` reads
from memory (active → immutables, newest→oldest, tombstones honored). Reads
from disk arrive with Sections 3–4 (SSTables).

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

## Layout

```
config/default.json   default tunables (see Section 0.4 / 1.6)
data/                 runtime files (data/wal/ holds WAL segments)
docs/                 design notes
include/lsm/          engine headers + api.md, errors.md
src/                  CLI + engine sources (wal.cpp is the Section 1 core)
tests/                test placeholders (Section 9)
```

## Configuration

Keys: `data_dir`, `memtable_max_bytes`, `max_immutable_tables`,
`memtable_size_overhead_bytes_per_entry`, `block_size`, `bloom_false_positive`,
`wal_fsync_every_n`, `wal_segment_roll_bytes`, `compression` (`off|snappy|lz4`),
`log_level` (`debug|info|warn|error`).

Resolution priority (later wins): built-in defaults → config file →
env vars (`LSMKV_*`) → CLI flags. A missing config file falls back to defaults;
unknown keys print a warning and are ignored.
