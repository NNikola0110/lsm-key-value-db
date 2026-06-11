#include "lsm/engine.hpp"

#include "lsm/errors.hpp"

#include <utility>

namespace lsm {

Engine::Engine(Config config) : config_(std::move(config)) {}

void Engine::ensure_open() const {
    if (closed_) {
        throw Error(ErrorCode::StoreClosed, "store is closed");
    }
}

void Engine::put(std::string_view key, std::string_view /*value*/) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    throw Error(ErrorCode::NotImplemented, "Put not implemented yet");
}

std::optional<std::string> Engine::get(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    throw Error(ErrorCode::NotImplemented, "Get not implemented yet");
}

void Engine::remove(std::string_view key) {
    ensure_open();
    if (key.empty()) {
        throw Error(ErrorCode::InvalidArgument, "empty key is not allowed");
    }
    throw Error(ErrorCode::NotImplemented, "Delete not implemented yet");
}

void Engine::close() noexcept {
    closed_ = true;
}

} // namespace lsm
