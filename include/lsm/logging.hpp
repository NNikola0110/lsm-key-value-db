#pragma once

#include <initializer_list>
#include <string>
#include <utility>

#include "lsm/config.hpp"
#include "lsm/errors.hpp"

namespace lsm {

// Structured key=value logging (Section 8.3). Every event line looks like:
//   ts=2026-07-06T12:00:00Z level=info event=flush_publish sst_id=000231 ...
// Field names are part of the grading contract — never rename them.
// Thread-safe: one mutex serializes whole lines (no interleaving).

void set_log_level(LogLevel level);
[[nodiscard]] LogLevel current_log_level();

using LogFields = std::initializer_list<std::pair<const char*, std::string>>;

// Emits to stderr iff level >= the configured log_level.
void log_event(LogLevel level, const char* event, LogFields fields);

// RFC3339 UTC timestamp (shared with the manifest's created_at).
std::string ts_rfc3339();

// Process-wide error counters by class (lsm_errors_total{type=...}, 8.2).
void count_error(ErrorCode code);
std::uint64_t error_count(ErrorCode code);

// Fault injection (Section 9.4): if the environment variable LSMKV_CRASHPOINT
// equals `name`, print one line and hard-exit (simulated power cut). The
// eight annotated points are listed in TESTING.md; names are part of the
// test contract.
void crash_point(const char* name);

} // namespace lsm
