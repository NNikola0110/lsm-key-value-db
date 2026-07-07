# lsmkv

A single-node LSM (Log-Structured Merge) key-value store — Phase A.
Language: **C++23**. Build: **CMake**.

Progress: **Section 1 (WAL & crash recovery)** is implemented. `put`/`del`
append durably to the write-ahead log and survive crashes; `get` stays
`not implemented` until Section 2 (Memtable).

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
build/lsmkv put --key foo --value bar  # append PUT record, prints assigned seqno
build/lsmkv del --key foo              # append DEL record, prints assigned seqno
build/lsmkv get --key foo              # Get(foo) — not implemented yet (Section 2)
build/lsmkv stats                      # config + WAL stats (segments, last seqno, ...)
build/lsmkv close                      # finish appends, sync, exit cleanly
build/lsmkv wal-verify                 # read-only scan, prints a recovery report
build/lsmkv wal-truncate --segment 000003.wal --offset 987654   # manual repair tool
```

Every command that opens the engine first replays the WAL and prints a
one-line recovery report, e.g.:

```
recovery: segments=3 records=10523 truncated=1 last_seqno=10523 status=OK
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

Keys: `data_dir`, `memtable_max_bytes`, `block_size`, `bloom_false_positive`,
`wal_fsync_every_n`, `wal_segment_roll_bytes`, `compression` (`off|snappy|lz4`),
`log_level` (`debug|info|warn|error`).

Resolution priority (later wins): built-in defaults → config file →
env vars (`LSMKV_*`) → CLI flags. A missing config file falls back to defaults;
unknown keys print a warning and are ignored.
