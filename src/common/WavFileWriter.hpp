#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "common/MediaTypes.hpp"
#include "common/Status.hpp"

namespace live_studio::common {

class WavFileWriter {
public:
    WavFileWriter() = default;
    ~WavFileWriter();

    WavFileWriter(const WavFileWriter&) = delete;
    WavFileWriter& operator=(const WavFileWriter&) = delete;

    Status open(const std::string& path, int sampleRate, int channels);
    Status appendFrame(const media::AudioFrame& frame);
    Status close();

    [[nodiscard]] bool isOpen() const noexcept {
        return output_.is_open();
    }

    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::uint64_t dataBytesWritten() const noexcept {
        return dataBytesWritten_;
    }

private:
    Status writeHeader();
    Status finalizeHeader();

    std::ofstream output_;
    std::string path_;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::uint64_t dataBytesWritten_ = 0;
};

}  // namespace live_studio::common
