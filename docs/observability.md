# Observability charter (Section 8)

lsmkv must be able to explain itself in under 30 seconds without a debugger:
one `stats` call answers "what state is the engine in", one `bg-status` call
answers "what is background work doing", and two grep-able log lines answer
"what just happened". Every number an operator needs is exposed three ways —
human CLI text, JSON (`stats --json`, `GET /stats`), and Prometheus
(`GET /metrics`) — under names that are **locked**: renaming a metric or a
log field breaks dashboards and graders' greps, so names below are final.

Units are standardized: bytes everywhere, milliseconds in log `dur_ms`
fields, seconds only in (future) histograms. Logs are `key=value` lines on
stderr with `ts` (RFC3339) and `level`; stdout is reserved for command
payloads so scripts can parse it.

## Metrics (locked names)

| Name | Type | Meaning / why the TA looks at it |
|---|---|---|
| lsm_writes_total | counter | accepted put+del; the write QPS baseline |
| lsm_wal_appends_total | counter | WAL records appended (== writes today) |
| lsm_wal_active_segment_bytes | gauge | bytes in the active segment; growth = healthy appends |
| lsm_wal_segments_total | gauge | .wal files on disk; a pile means flush/cleanup is stuck |
| lsm_memtable_active_bytes / _entries | gauge | how full the active table is; nears memtable_max_bytes before rotation |
| lsm_memtables_immutable | gauge | flush queue length; sustained > 0 means flush lags |
| lsm_flush_jobs_total | counter | completed flushes; flat while immutables > 0 = stuck flusher |
| lsm_sst_created_total | counter | tables produced by flush |
| lsm_sst_bytes_written_total | counter | flush write volume |
| lsm_reads_total | counter | get calls |
| lsm_read_hit_memtable_total | counter | served from memory; high = hot working set |
| lsm_read_sstable_consulted_total | counter | tables touched per read (ratio to reads = read amplification) |
| lsm_bloom_checks_total / lsm_bloom_negative_total | counter | negative/checks should be ~1-FPR for absent keys |
| lsm_block_cache_hits_total / _misses_total | counter | hit rate < 0.8 = cache too small |
| lsm_disk_block_reads_total | counter | physical reads; should barely move on absent-key load |
| lsm_compaction_jobs_total | counter | completed compactions |
| lsm_compaction_bytes_in_total / _out_total | counter | out/in = write-amplification ratio |
| lsm_compaction_backlog_bytes | gauge | bytes in the next pickable set; growth = compaction losing |
| lsm_versions_published_total | counter | version churn; one per rotation/flush/compaction |
| lsm_epoch_current | gauge | manifest epoch |
| lsm_sst_live_total | gauge | live tables; read cost grows with it |
| lsm_write_stalls_total | counter | writer blocked on full immutable queue |
| lsm_errors_total{type=...} | counter | by error class; any Corruption > 0 is a red alert |

## Log events (locked field names, Section 8.3)

- `rotate_memtable` — old_active_bytes, immutables_after, reason=size
- `flush_start` / `flush_publish` — imm_id, sst_id, entries, bytes, dur_ms
- `compaction_start` / `compaction_done` — job_id, inputs, bytes_in,
  bytes_out, keys_in, keys_out, tombstones_kept, dur_ms
- `version_publish` — epoch, version_id, sst_added, sst_removed,
  immutables_removed, reason (emitted at publish_log_level=debug)
- `stall_write` — immutables, max_immutables, reason=backpressure
- `wal_truncate_tail` — segment, truncated_to
- `corruption_detected` — file, offset, phase=read, action=fatal

## HTTP endpoints (`lsmkv serve`, Section 8.5)

- `GET /healthz` → 200 "ok" while the process runs
- `GET /readyz` → 200 "ready" when WAL/manifest/workers are up, else 503
- `GET /metrics` → Prometheus text (404 when metrics_enabled=false)
- `GET /stats` → the stats JSON object

## Troubleshooting playbook (8.8)

- **High get latency** → check block_cache_hit_rate; < 0.8 → raise
  block_cache_mb; if fine, check sst_live + compaction_backlog_bytes →
  `compaction-run`.
- **Frequent write stalls** → immutables at max_immutable_tables → flush is
  the bottleneck (check flush job dur_ms / fsync policy) or raise the limit.
- **Large .wal pile** → flush_jobs_total flat? Immutables not flushing; WAL
  watermark cleanup only runs after flushes (Section 3.8).
- **CorruptionDetected** → `sst-info` / `wal-verify`; WAL tail → truncated
  automatically; SST block → quarantine the file, re-run compaction.

## Dashboards (sketch, 8.7)

Health row: epoch, sst_live, immutables, compaction_backlog_bytes.
Write row: rate(lsm_writes_total), write stalls. Read row: get QPS, cache
hit rate, bloom negative ratio. Background row: flush/compaction durations,
bytes in/out, rate(versions_published_total).

## Config toggles (8.9)

- `log_level` (info) — debug adds per-get traces and publish events
- `metrics_enabled` (true) — /metrics on or 404
- `http_listen_addr` (127.0.0.1:9090) — used by `lsmkv serve`
- `stats_sampling_interval_ms` (1000) — reserved for future push metrics
- `doctor_bundle_max_mb` (10) — support bundle size cap
