# lsmkv

A single-node LSM (Log-Structured Merge) key-value store — Phase A.
Language: **C++23**. Build: **CMake**.

This repository is currently at **Section 0** (project skeleton): the CLI runs and
accepts commands; storage operations print `not implemented yet` instead of crashing.

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
build/lsmkv init                       # create data dir, print resolved config OK lines
build/lsmkv put --key foo --value bar  # Put(foo) — not implemented yet
build/lsmkv get --key foo              # Get(foo) — not implemented yet
build/lsmkv del --key foo              # Delete(foo) — not implemented yet
build/lsmkv stats                      # resolved config + engine status: stub
build/lsmkv close                      # closed (stub)
```

`--config PATH` overrides the config file (default: `config/default.json`).
Missing `--key` exits with a non-zero code.

## Layout

```
config/default.json   default tunables (see Section 0.4)
data/                 runtime files live here (created by `init`)
docs/                 design notes
include/lsm/          engine headers + api.md, errors.md
src/                  CLI + engine sources
tests/                test placeholders (Section 9)
```

## Configuration

Keys: `data_dir`, `memtable_max_bytes`, `block_size`, `bloom_false_positive`,
`wal_fsync_every_n`, `compression` (`off|snappy|lz4`), `log_level`
(`debug|info|warn|error`).

Resolution priority (later wins): built-in defaults → config file →
env vars (`LSMKV_*`) → CLI flags. A missing config file falls back to defaults;
unknown keys print a warning and are ignored.
