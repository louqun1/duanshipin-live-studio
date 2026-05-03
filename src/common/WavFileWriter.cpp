#include "common/WavFileWriter.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace live_studio::common {
namespace {

void writeLe16(std::ostream& output, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    output.write(bytes, sizeof(bytes));
}

void writeLe32(std::ostream& output, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    output.write(bytes, sizeof(bytes));
}

}  // namespace

WavFileWriter::~WavFileWriter() {
    (void)close();
}

Status WavFileWriter::open(const std::string& path, int sampleRate, int channels) {
    if (path.empty()) {
        return Status::InvalidArgument("WAV output path is empty");
    }
    if (sampleRate <= 0 || channels <= 0) {
        return Status::InvalidArgument("WAV writer requires positive sample rate and channels");
    }

    if (output_.is_open()) {
        Status closeStatus = close();
        if (!closeStatus.ok()) {
            return closeStatus;
        }
    }

    std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path()) {
        std::error_code createError;
        std::filesystem::create_directories(outputPath.parent_path(), createError);
        if (createError) {
            return Status::InternalError("failed to create WAV output directory: " + createError.message());
        }
    }

    output_.open(path, std::ios::binary | std::ios::trunc);
    if (!output_.is_open()) {
        return Status::InternalError("failed to open WAV output file: " + path);
    }

    path_ = path;
    sampleRate_ = sampleRate;
    channels_ = channels;
    dataBytesWritten_ = 0;
    return writeHeader();
}

Status WavFileWriter::appendFrame(const media::AudioFrame& frame) {
    if (!output_.is_open()) {
        return Status::InvalidArgument("WAV writer is not open");
    }
    if (!frame.hasSamplePayload || frame.sampleFormat != media::AudioSampleFormat::s16_interleaved) {
        return Status::InvalidArgument("WAV writer expects S16 interleaved audio");
    }
    if (frame.sampleRate != sampleRate_ || frame.channels != channels_) {
        return Status::InvalidArgument("audio frame does not match WAV writer format");
    }

    output_.write(reinterpret_cast<const char*>(frame.samplePayload.data()),
                  static_cast<std::streamsize>(frame.samplePayload.size()));
    if (!output_.good()) {
        return Status::InternalError("failed to append PCM payload to WAV file");
    }

    dataBytesWritten_ += frame.samplePayload.size();
    return Status::Ok();
}

Status WavFileWriter::close() {
    if (!output_.is_open()) {
        return Status::Ok();
    }

    Status finalizeStatus = finalizeHeader();
    output_.close();
    path_.clear();
    sampleRate_ = 0;
    channels_ = 0;
    dataBytesWritten_ = 0;
    return finalizeStatus;
}

Status WavFileWriter::writeHeader() {
    if (!output_.is_open()) {
        return Status::InvalidArgument("WAV writer is not open");
    }

    output_.seekp(0, std::ios::beg);
    output_.write("RIFF", 4);
    writeLe32(output_, 36);
    output_.write("WAVE", 4);
    output_.write("fmt ", 4);
    writeLe32(output_, 16);
    writeLe16(output_, 1);
    writeLe16(output_, static_cast<std::uint16_t>(channels_));
    writeLe32(output_, static_cast<std::uint32_t>(sampleRate_));
    const std::uint32_t byteRate = static_cast<std::uint32_t>(sampleRate_) *
                                   static_cast<std::uint32_t>(channels_) * sizeof(std::int16_t);
    writeLe32(output_, byteRate);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels_ * sizeof(std::int16_t));
    writeLe16(output_, blockAlign);
    writeLe16(output_, 16);
    output_.write("data", 4);
    writeLe32(output_, 0);

    if (!output_.good()) {
        return Status::InternalError("failed to write WAV header");
    }
    return Status::Ok();
}

Status WavFileWriter::finalizeHeader() {
    if (!output_.is_open()) {
        return Status::Ok();
    }

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(dataBytesWritten_, static_cast<std::uint64_t>(0xffffffffu)));
    const std::uint32_t riffSize = dataBytes + 36;

    output_.seekp(4, std::ios::beg);
    writeLe32(output_, riffSize);
    output_.seekp(40, std::ios::beg);
    writeLe32(output_, dataBytes);
    output_.flush();
    if (!output_.good()) {
        return Status::InternalError("failed to finalize WAV header");
    }
    return Status::Ok();
}

}  // namespace live_studio::common
