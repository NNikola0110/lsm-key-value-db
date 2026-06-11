#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace lsm {

// Standardized error names (Section 0.5 / errors.md).
// CorruptionDetected and the rest are wired now; most are only thrown in later sections.
enum class ErrorCode {
    InvalidArgument,
    StoreClosed,
    IOFailure,
    CorruptionDetected,
    NotImplemented,
};

constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidArgument:    return "InvalidArgument";
        case ErrorCode::StoreClosed:        return "StoreClosed";
        case ErrorCode::IOFailure:          return "IOFailure";
        case ErrorCode::CorruptionDetected: return "CorruptionDetected";
        case ErrorCode::NotImplemented:     return "NotImplemented";
    }
    return "Unknown";
}

// Single exception type carrying a stable ErrorCode plus a human message.
class Error : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace lsm
