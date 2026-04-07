#pragma once

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace live_studio {

class TraceLog {
public:
    void push(std::string stage, std::string detail) {
        lines_.push_back("[" + std::move(stage) + "] " + std::move(detail));
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
