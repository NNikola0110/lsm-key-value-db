# Standard error names (Section 0.5)

Every error surfaced by the engine or CLI uses one of these stable names
(`lsm::ErrorCode` in [errors.hpp](errors.hpp)). The CLI prints them as
`[Name] message`.

| Name | When it is used |
|------|-----------------|
| `InvalidArgument` | Caller passed something illegal: empty key, bad config value (unknown `compression`/`log_level`), malformed flag values. Rejected **before** any disk write. |
| `StoreClosed` | Any API call after `Close()`. |
| `IOFailure` | The OS refused an I/O operation: cannot create the data dir, cannot open/append/sync a WAL segment, cannot open the config file. |
| `CorruptionDetected` | Stored bytes fail validation: invalid config JSON; later sections use it for checksum/format failures that are *not* recoverable tail corruption. (WAL tail corruption is handled silently by truncation during recovery, per Section 1.) |
| `NotImplemented` | The operation belongs to a later section and is deliberately stubbed. Allowed during early sections only. |
| `Backpressure` | (Added in Section 2.6.) A write was refused because the active memtable is full and the immutable list is at `max_immutable_tables`. Once flushing exists (Section 3) this becomes a wait instead of an error. |
