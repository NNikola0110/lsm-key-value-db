# Testing & fault injection (Section 9)

One command runs everything (build + unit + property + chaos drills):

```sh
sh tests/run_all.sh
```

A pass looks like: `14 tests, 0 failures`, then `ALL DRILLS PASSED`, then
`== all phases passed ==`. Exit code 0.

## Suites

**Phase 1 — unit** (`build/Debug/lsmkv_tests`, [tests/unit_tests.cpp](tests/unit_tests.cpp)):

| Suite | Covers (9.2) |
|---|---|
| wal_append_replay | N PUTs + M DELs replay; counts, last_seqno, monotonic seqNos |
| wal_tail_truncation | half-cut record AND appended garbage → truncate to last good offset |
| wal_segment_roll | roll threshold opens a new segment, appends continue |
| memtable_semantics | overwrite-latest, delete/tombstone, byte accounting |
| sstable_roundtrip_and_edges | sorted unique output, tombstones preserved, checksums OK; edge keys: empty value, 1 KB key, UTF-8 key, binary-zero value, 40 shared-prefix keys (restart points) |
| sstable_reader_via_engine | disk reads, bloom-guarded absent keys, newest-wins vs memtable |
| sstable_corruption_detected | flipped block byte → CorruptionDetected, no crash |
| manifest_version | epoch persists; startup publishes Version epoch+1; Version mirrors manifest |
| compaction_merge | 2 tables merge; newest-wins; tombstone kept under grace |

**Phase 2 — property/model-based** (same binary, seeds 1–5): 600 random ops
per seed (put/del/get; hot-key skew; value sizes 0/small/medium) against a
`std::map` oracle. Invariants checked: (1) `get == oracle` after every op,
(2) durability — close+reopen equals oracle (with `wal_fsync_every_n=1`),
(3) seqNos strictly increase, (4) tombstoned keys stay hidden. **On failure
the seed is printed**; reproduce with `lsmkv_tests --seed N`.

**Phase 3 — chaos drills A–E** (`sh tests/drills.sh`), built on the crash-point
injector: setting `LSMKV_CRASHPOINT=<name>` makes the process hard-exit
(`_Exit(137)`, no flushes) at that point. Annotated points:

1. `CP_WAL_AFTER_APPEND_BEFORE_FSYNC`
2. `CP_WAL_AFTER_FSYNC_BEFORE_ACK`
3. `CP_ROTATE_BEFORE_ENQUEUE_IMMUTABLE`
4. `CP_FLUSH_AFTER_DATABLOCKS_BEFORE_INDEX`
5. `CP_FLUSH_AFTER_INDEX_BEFORE_RENAME`
6. `CP_FLUSH_AFTER_RENAME_BEFORE_MANIFEST`
7. `CP_MANIFEST_AFTER_WRITE_BEFORE_RENAME`
8. `CP_COMPACTION_AFTER_OUTPUTS_BEFORE_MANIFEST`

| Drill | Scenario | Must hold after restart |
|---|---|---|
| A | power cut during writes (points 1/2) | wal-verify OK; every acknowledged write present |
| B | crash mid-flush (point 4) | no half-published table in manifest; retry succeeds |
| C | crash after compaction output, before manifest (point 8) | inputs still live; orphan pruned; re-run OK |
| D | garbage appended to WAL tail | tail truncated, recovery reports it, data intact |
| E | flush target unwritable (disk-failure stand-in) | flush fails with IOFailure; manifest unchanged; data stays in WAL; retry succeeds |

## Performance smoke checks (9.6 — directions, not numbers)

- `lsmkv stress --threads 4 --ops 1250` completes with `errors=0`; report ops/sec.
- `lsmkv probe --key K --repeat 1000` → `block_cache_hit_rate` > 0.8.
- `lsmkv probe --absent 1000` → `blooms_negative/blooms_checked` > 0.9 and
  `disk_block_reads` stays in single digits.

## Reproducibility rules (9.11)

- Property failures print the seed; drills are deterministic.
- CI artifacts on failure: wal-verify output, `stats --json`, manifest.json
  (see tests/run_all.sh), plus stderr logs (structured events).
- Tiny-memtable configs can trip `l0_stop_writes` by design — raise it via
  `LSMKV_L0_STOP_WRITES` in stress-style tests.

## Coverage notes (9.7)

No coverage tooling is wired into MSVC here; the priority order from the
spec (branchy WAL replay + publish paths first) is reflected in the suite
list above, and every log event asserted in Section 8 is emitted on the
paths these tests execute.
