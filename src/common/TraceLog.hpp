#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "spdlog/spdlog.h"

namespace live_studio {

class TraceLog {
public:
    void push(std::string stage, std::string detail) {
        std::string line = "[" + stage + "] " + detail;
        lines_.push_back(line);
        spdlog::info("{}", line);
    }

    [[nodiscard]] const std::vector<std::string>& lines() const noexcept {
        return lines_;
    }

    void print(std::ostream& out) const {
        for (const std::string& line : lines_) {
            out << line << '\n';
        }
    }

private:
    std::vector<std::string> lines_;
};

}  // namespace live_studio
