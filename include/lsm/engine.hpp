#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lsm/config.hpp"

namespace lsm {

// The storage engine facade. In Section 0 the storage operations are stubs that
// throw ErrorCode::NotImplemented; later sections fill in WAL, memtable, SSTables.
// Close() flips the store to a closed state after which calls raise StoreClosed.
class Engine {
public:
    explicit Engine(Config config);

    void put(std::string_view key, std::string_view value);   // throws NotImplemented
    std::optional<std::string> get(std::string_view key);     // throws NotImplemented
    void remove(std::string_view key);                        // Delete; throws NotImplemented
    void close() noexcept;

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    void ensure_open() const;

    Config config_;
    bool   closed_ = false;
};

} // namespace lsm
