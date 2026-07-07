#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lsm/config.hpp"
#include "lsm/wal.hpp"

namespace lsm {

// The storage engine facade. As of Section 1, Put/Delete append durably to
// the WAL and return the assigned seqNo; Get stays stubbed until Section 2
// (Memtable). Opening the engine replays the WAL (crash recovery).
// Close() flips the store to a closed state after which calls raise StoreClosed.
class Engine {
public:
    explicit Engine(Config config);
    ~Engine();

    // Runs WAL recovery and opens the active segment for appending.
    // The report should be printed by the caller (Section 1.8).
    WalRecoveryReport open();

    std::uint64_t put(std::string_view key, std::string_view value);  // returns seqNo
    std::optional<std::string> get(std::string_view key);             // throws NotImplemented
    std::uint64_t remove(std::string_view key);                       // Delete; returns seqNo
    void close();

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] WalStats wal_stats() const;

private:
    void ensure_open() const;

    Config config_;
    std::unique_ptr<Wal> wal_;
    bool   closed_ = false;
};

} // namespace lsm
