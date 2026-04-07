#pragma once

#include <string>
#include <utility>

namespace live_studio {

enum class StatusCode {
    ok,
    invalid_argument,
    not_implemented,
    internal_error,
};

struct Status {
    StatusCode code = StatusCode::ok;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == StatusCode::ok;
    }

    static Status Ok() {
        return {};
    }

    static Status InvalidArgument(std::string text) {
        return {StatusCode::invalid_argument, std::move(text)};
    }

    static Status NotImplemented(std::string text) {
        return {StatusCode::not_implemented, std::move(text)};
    }

    static Status InternalError(std::string text) {
        return {StatusCode::internal_error, std::move(text)};
    }
};

}  // namespace live_studio
