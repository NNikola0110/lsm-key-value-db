#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// What WAL replay found on startup (Section 1.8). Printed as one line:
//   recovery: segments=3 records=10523 truncated=1 last_seqno=10523 status=OK
struct WalRecoveryReport {
    std::uint64_t segments     = 0;
    std::uint64_t records      = 0;
    std::uint64_t truncated    = 0;   // segments whose corrupt tail was cut off
    std::uint64_t last_seqno   = 0;
    std::string   truncated_segment;  // last segment that was truncated, if any
    std::uint64_t truncated_to = 0;   // byte offset it was truncated to

    void print(std::ostream& os) const;
};

struct WalStats {
    std::uint64_t active_segment_id    = 0;
    std::uint64_t active_segment_bytes = 0;
    std::uint64_t total_segments       = 0;
    std::uint64_t last_seqno           = 0;
    std::uint32_t fsync_every_n        = 1;
    std::uint64_t roll_bytes           = 0;
};

// Append-only write-ahead log split into numbered segments (000001.wal, ...)
// under one directory. Single writer, no threads yet (Section 7).
//
// Segment layout:  8-byte header [magic "LSMW" | version u8 | 3 reserved bytes],
// then records. Record framing (all integers little-endian):
//   payload_len u32 | payload | crc32(payload) u32
//   payload = type u8 (1=PUT, 2=DEL) | seqno u64 | key_len u32 | value_len u32
//             | key bytes | value bytes
//
// Roll policy: size-based — a new segment starts when the active one would
// exceed roll_bytes. Sync policy: fsync after every Nth record
// (wal_fsync_every_n; N=1 means every record) and always on close.
class Wal {
public:
    Wal(std::filesystem::path dir, std::uint32_t fsync_every_n, std::uint64_t roll_bytes);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Called once per valid record during replay, oldest record first.
    // is_del=true for DEL records (value is then empty). segment_id is the
    // WAL segment the record was read from (used for the flush/cleanup
    // policy, Section 3.8).
    using ReplayFn = std::function<void(bool is_del, std::string_view key,
                                        std::string_view value, std::uint64_t seqno,
                                        std::uint64_t segment_id)>;

    // Creates the directory if needed, scans segments oldest->newest,
    // truncates corrupt tails, seeds the seqNo counter from the highest seqNo
    // seen, and opens the newest segment for appending. If replay is given it
    // receives every valid record (used to rebuild the memtable, Section 2.7).
    WalRecoveryReport open(const ReplayFn& replay = nullptr);

    // Append one mutation; returns its assigned seqNo. The record is durable
    // per the sync policy before this returns.
    std::uint64_t append_put(std::string_view key, std::string_view value);
    std::uint64_t append_del(std::string_view key);

    void sync();
    void close();  // finish appends, sync, close the active segment

    // Close the active segment and start a fresh one; returns the new
    // segment id. Called on memtable rotation (hybrid WAL-cleanup policy).
    std::uint64_t roll();

    // Watermark cleanup (Section 3.8): delete every non-active segment whose
    // highest seqNo is <= watermark (i.e. fully covered by flushed SSTables;
    // tables are strictly seqNo-ordered, so anything unflushed is above the
    // watermark). Returns the deleted file names.
    std::vector<std::string> remove_segments_covered(std::uint64_t watermark);

    // Raise the seqNo counter to at least `floor` (used after loading the
    // manifest: flushed data may have higher seqNos than the surviving WAL).
    void bump_seqno(std::uint64_t floor) noexcept { last_seqno_ = std::max(last_seqno_, floor); }

    [[nodiscard]] WalStats stats() const;
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }

    // Read-only scan of a WAL directory: reports what recovery would do
    // without changing anything (`lsmkv wal-verify`).
    static WalRecoveryReport verify(const std::filesystem::path& dir);

private:
    std::uint64_t append(std::uint8_t type, std::string_view key, std::string_view value);
    void open_active_segment(std::uint64_t id, bool fresh);
    void roll_segment();

    std::filesystem::path dir_;
    std::uint32_t fsync_every_n_;
    std::uint64_t roll_bytes_;

    std::FILE*    file_ = nullptr;
    std::map<std::uint64_t, std::uint64_t> segment_max_seqno_;  // per closed+active segment
    std::uint64_t active_id_ = 0;
    std::uint64_t active_bytes_ = 0;
    std::uint64_t total_segments_ = 0;
    std::uint64_t last_seqno_ = 0;
    std::uint32_t appends_since_sync_ = 0;
};

} // namespace lsm
