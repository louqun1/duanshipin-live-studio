#include "encode/VideoEncoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
}

namespace live_studio::encode {
namespace {

std::string ffmpegErrorString(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

bool startsWithStartCode(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t* codeSize) {
    if ((offset + 3) <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
        *codeSize = 3;
        return true;
    }
    if ((offset + 4) <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
        data[offset + 3] == 1) {
        *codeSize = 4;
        return true;
    }
    return false;
}

std::vector<std::vector<std::uint8_t>> splitAnnexBNalus(const std::uint8_t* data, std::size_t size) {
    std::vector<std::vector<std::uint8_t>> nalUnits;
    std::size_t cursor = 0;
    while (cursor < size) {
        std::size_t startCodeSize = 0;
        if (!startsWithStartCode(data, size, cursor, &startCodeSize)) {
            ++cursor;
            continue;
        }

        const std::size_t naluStart = cursor + startCodeSize;
        std::size_t next = naluStart;
        while (next < size) {
            std::size_t nextStartCodeSize = 0;
            if (startsWithStartCode(data, size, next, &nextStartCodeSize)) {
                break;
            }
            ++next;
        }

        if (next > naluStart) {
            nalUnits.emplace_back(data + naluStart, data + next);
        }
        cursor = next;
    }
    return nalUnits;
}

std::string describeH264Profile(std::uint8_t profileIdc) {
    switch (profileIdc) {
    case 66:
        return "baseline";
    case 77:
        return "main";
    case 88:
        return "extended";
    case 100:
        return "high";
    default:
        return "profile-" + std::to_string(profileIdc);
    }
}

std::string describeParameterSet(const char* label, const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return std::string(label) + "(0B)";
    }
    const int nalType = bytes.front() & 0x1f;
    return std::string(label) + "(" + std::to_string(bytes.size()) + "B,nal=" + std::to_string(nalType) + ")";
}

Status parseAvccParameterSets(const std::vector<std::uint8_t>& extradata,
                              std::vector<std::uint8_t>* outSps,
                              std::vector<std::uint8_t>* outPps,
                              int* outNalLengthSize) {
    if (outSps == nullptr || outPps == nullptr || outNalLengthSize == nullptr) {
        return Status::InvalidArgument("AVCC parse outputs are null");
    }
    if (extradata.size() < 7 || extradata.front() != 1) {
        return Status::InvalidArgument("H.264 extradata is not AVCC");
    }

    std::size_t cursor = 6;
    const int nalLengthSize = (extradata[4] & 0x03) + 1;
    const int spsCount = extradata[5] & 0x1f;
    if (spsCount <= 0) {
        return Status::InvalidArgument("AVCC extradata does not contain SPS");
    }

    for (int i = 0; i < spsCount; ++i) {
        if ((cursor + 2) > extradata.size()) {
            return Status::InvalidArgument("AVCC extradata ended before SPS length");
        }
        const std::size_t length =
            (static_cast<std::size_t>(extradata[cursor]) << 8) | static_cast<std::size_t>(extradata[cursor + 1]);
        cursor += 2;
        if ((cursor + length) > extradata.size()) {
            return Status::InvalidArgument("AVCC SPS length exceeds extradata size");
        }
        if (outSps->empty()) {
            outSps->assign(extradata.data() + cursor, extradata.data() + cursor + length);
        }
        cursor += length;
    }

    if (cursor >= extradata.size()) {
        return Status::InvalidArgument("AVCC extradata ended before PPS count");
    }

    const int ppsCount = extradata[cursor++];
    if (ppsCount <= 0) {
        return Status::InvalidArgument("AVCC extradata does not contain PPS");
    }

    for (int i = 0; i < ppsCount; ++i) {
        if ((cursor + 2) > extradata.size()) {
            return Status::InvalidArgument("AVCC extradata ended before PPS length");
        }
        const std::size_t length =
            (static_cast<std::size_t>(extradata[cursor]) << 8) | static_cast<std::size_t>(extradata[cursor + 1]);
        cursor += 2;
        if ((cursor + length) > extradata.size()) {
            return Status::InvalidArgument("AVCC PPS length exceeds extradata size");
        }
        if (outPps->empty()) {
            outPps->assign(extradata.data() + cursor, extradata.data() + cursor + length);
        }
        cursor += length;
    }

    *outNalLengthSize = nalLengthSize;
    return Status::Ok();
}

Status parseAnnexBParameterSets(const std::vector<std::uint8_t>& extradata,
                                std::vector<std::uint8_t>* outSps,
                                std::vector<std::uint8_t>* outPps) {
    if (outSps == nullptr || outPps == nullptr) {
        return Status::InvalidArgument("Annex B parse outputs are null");
    }

    const std::vector<std::vector<std::uint8_t>> nalUnits = splitAnnexBNalus(extradata.data(), extradata.size());
    for (const std::vector<std::uint8_t>& nalu : nalUnits) {
        if (nalu.empty()) {
            continue;
        }
        const int nalType = nalu.front() & 0x1f;
        if (nalType == 7 && outSps->empty()) {
            *outSps = nalu;
        } else if (nalType == 8 && outPps->empty()) {
            *outPps = nalu;
        }
    }

    if (outSps->empty() || outPps->empty()) {
        return Status::InvalidArgument("Annex B extradata does not contain SPS/PPS");
    }
    return Status::Ok();
}

Status convertPacketToAnnexB(const AVPacket* packet, int nalLengthSize, std::vector<std::uint8_t>* outPayload) {
    if (packet == nullptr || outPayload == nullptr) {
        return Status::InvalidArgument("packet conversion arguments are null");
    }
    if (packet->data == nullptr || packet->size <= 0) {
        return Status::InvalidArgument("packet is empty");
    }

    const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(packet->data);
    const std::size_t size = static_cast<std::size_t>(packet->size);
    std::size_t startCodeSize = 0;
    if (startsWithStartCode(data, size, 0, &startCodeSize)) {
        outPayload->assign(data, data + size);
        return Status::Ok();
    }

    if (nalLengthSize < 1 || nalLengthSize > 4) {
        outPayload->assign(data, data + size);
        return Status::Ok();
    }

    std::vector<std::uint8_t> converted;
    std::size_t cursor = 0;
    while (cursor < size) {
        if ((cursor + static_cast<std::size_t>(nalLengthSize)) > size) {
            return Status::InternalError("length-prefixed H.264 packet is truncated");
        }

        std::uint32_t nalSize = 0;
        for (int i = 0; i < nalLengthSize; ++i) {
            nalSize = (nalSize << 8) | data[cursor + static_cast<std::size_t>(i)];
        }
        cursor += static_cast<std::size_t>(nalLengthSize);

        if (nalSize == 0 || (cursor + nalSize) > size) {
            return Status::InternalError("invalid NAL length in encoded H.264 packet");
        }

        converted.insert(converted.end(), {0, 0, 0, 1});
        converted.insert(converted.end(), data + cursor, data + cursor + nalSize);
        cursor += nalSize;
    }

    *outPayload = std::move(converted);
    return Status::Ok();
}

std::string summarizeAccessUnitPayload(const std::vector<std::uint8_t>& payload, bool keyFrame) {
    const std::vector<std::vector<std::uint8_t>> nalUnits = splitAnnexBNalus(payload.data(), payload.size());
    int firstVclType = 0;
    for (const std::vector<std::uint8_t>& nalu : nalUnits) {
        if (nalu.empty()) {
            continue;
        }
        const int nalType = nalu.front() & 0x1f;
        if (nalType == 1 || nalType == 5) {
            firstVclType = nalType;
            break;
        }
    }

    const std::string kind = (keyFrame || firstVclType == 5) ? "idr+nalu(real " : "pframe+nalu(real ";
    return kind + std::to_string(payload.size()) + "B," + std::to_string(nalUnits.size()) + "nal)";
}

Status copyVideoFrameToAvFrame(const media::VideoFrame& inputFrame, AVFrame* ffmpegFrame) {
    if (ffmpegFrame == nullptr) {
        return Status::InvalidArgument("ffmpegFrame is null");
    }
    if (!inputFrame.hasPixelPayload || inputFrame.pixelFormat != media::VideoPixelFormat::yuv420p) {
        return Status::InvalidArgument("input frame does not carry a YUV420P payload");
    }
    if ((inputFrame.width % 2) != 0 || (inputFrame.height % 2) != 0) {
        return Status::InvalidArgument("YUV420P input frame requires even width and height");
    }

    const std::size_t yPlaneSize =
        static_cast<std::size_t>(inputFrame.lineStrideY) * static_cast<std::size_t>(inputFrame.height);
    const std::size_t uPlaneSize =
        static_cast<std::size_t>(inputFrame.lineStrideU) * static_cast<std::size_t>(inputFrame.height / 2);
    const std::size_t vPlaneSize =
        static_cast<std::size_t>(inputFrame.lineStrideV) * static_cast<std::size_t>(inputFrame.height / 2);

    if (inputFrame.pixelPayload.size() < (yPlaneSize + uPlaneSize + vPlaneSize)) {
        return Status::InvalidArgument("input YUV420P payload is smaller than expected");
    }

    const int writableResult = av_frame_make_writable(ffmpegFrame);
    if (writableResult < 0) {
        return Status::InternalError("av_frame_make_writable failed: " + ffmpegErrorString(writableResult));
    }

    const std::uint8_t* sourceY = inputFrame.pixelPayload.data();
    const std::uint8_t* sourceU = sourceY + yPlaneSize;
    const std::uint8_t* sourceV = sourceU + uPlaneSize;

    for (int y = 0; y < inputFrame.height; ++y) {
        std::memcpy(ffmpegFrame->data[0] + (static_cast<std::size_t>(y) * ffmpegFrame->linesize[0]),
                    sourceY + (static_cast<std::size_t>(y) * inputFrame.lineStrideY),
                    static_cast<std::size_t>(inputFrame.width));
    }

    for (int y = 0; y < (inputFrame.height / 2); ++y) {
        std::memcpy(ffmpegFrame->data[1] + (static_cast<std::size_t>(y) * ffmpegFrame->linesize[1]),
                    sourceU + (static_cast<std::size_t>(y) * inputFrame.lineStrideU),
                    static_cast<std::size_t>(inputFrame.width / 2));
        std::memcpy(ffmpegFrame->data[2] + (static_cast<std::size_t>(y) * ffmpegFrame->linesize[2]),
                    sourceV + (static_cast<std::size_t>(y) * inputFrame.lineStrideV),
                    static_cast<std::size_t>(inputFrame.width / 2));
    }

    return Status::Ok();
}

}  // namespace

struct VideoEncoder::EncoderContext {
    const AVCodec* codec = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    media::H264ParameterSets parameterSets;
    int nalLengthSize = 0;

    ~EncoderContext() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codecContext);
    }
};

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() = default;

Status VideoEncoder::configureH264(int width, int height, int fps, int bitrate) {
    if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) {
        return Status::InvalidArgument("encoder parameters must be positive");
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        return Status::InvalidArgument("H.264 YUV420P encoder requires even width and height");
    }

    av_log_set_level(AV_LOG_ERROR);

    std::unique_ptr<EncoderContext> nextContext = std::make_unique<EncoderContext>();
    nextContext->codec = avcodec_find_encoder_by_name("libx264");
    if (nextContext->codec == nullptr) {
        nextContext->codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (nextContext->codec == nullptr) {
        return Status::InternalError("FFmpeg could not find an H.264 encoder");
    }

    nextContext->codecContext = avcodec_alloc_context3(nextContext->codec);
    if (nextContext->codecContext == nullptr) {
        return Status::InternalError("avcodec_alloc_context3 failed for H.264 encoder");
    }

    nextContext->codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    nextContext->codecContext->codec_id = AV_CODEC_ID_H264;
    nextContext->codecContext->width = width;
    nextContext->codecContext->height = height;
    nextContext->codecContext->bit_rate = bitrate;
    nextContext->codecContext->gop_size = fps;
    nextContext->codecContext->max_b_frames = 0;
    nextContext->codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    nextContext->codecContext->time_base = AVRational{1, fps};
    nextContext->codecContext->framerate = AVRational{fps, 1};
    nextContext->codecContext->thread_count = 1;
    nextContext->codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "preset", "veryfast", 0);
    av_dict_set(&options, "tune", "zerolatency", 0);
    av_dict_set(&options, "profile", "baseline", 0);

    const int openResult = avcodec_open2(nextContext->codecContext, nextContext->codec, &options);
    av_dict_free(&options);
    if (openResult < 0) {
        return Status::InternalError("avcodec_open2(H.264) failed: " + ffmpegErrorString(openResult));
    }

    nextContext->frame = av_frame_alloc();
    if (nextContext->frame == nullptr) {
        return Status::InternalError("av_frame_alloc failed for encoder input frame");
    }
    nextContext->frame->format = nextContext->codecContext->pix_fmt;
    nextContext->frame->width = width;
    nextContext->frame->height = height;

    const int bufferResult = av_frame_get_buffer(nextContext->frame, 32);
    if (bufferResult < 0) {
        return Status::InternalError("av_frame_get_buffer failed: " + ffmpegErrorString(bufferResult));
    }

    nextContext->packet = av_packet_alloc();
    if (nextContext->packet == nullptr) {
        return Status::InternalError("av_packet_alloc failed for encoded packet");
    }

    if (nextContext->codecContext->extradata == nullptr || nextContext->codecContext->extradata_size <= 0) {
        return Status::InternalError("H.264 encoder did not expose SPS/PPS extradata");
    }

    nextContext->parameterSets.extradataBytes.assign(
        nextContext->codecContext->extradata,
        nextContext->codecContext->extradata + nextContext->codecContext->extradata_size);

    Status parseStatus = parseAvccParameterSets(nextContext->parameterSets.extradataBytes,
                                                &nextContext->parameterSets.spsBytes,
                                                &nextContext->parameterSets.ppsBytes,
                                                &nextContext->nalLengthSize);
    if (!parseStatus.ok()) {
        nextContext->nalLengthSize = 0;
        parseStatus = parseAnnexBParameterSets(nextContext->parameterSets.extradataBytes,
                                               &nextContext->parameterSets.spsBytes,
                                               &nextContext->parameterSets.ppsBytes);
        if (!parseStatus.ok()) {
            return Status::InternalError("unable to parse H.264 SPS/PPS from encoder extradata: " + parseStatus.message);
        }
    }

    if (!nextContext->parameterSets.spsBytes.empty() && nextContext->parameterSets.spsBytes.size() > 1) {
        nextContext->parameterSets.profile = describeH264Profile(nextContext->parameterSets.spsBytes[1]);
    }
    nextContext->parameterSets.spsLabel = describeParameterSet("sps", nextContext->parameterSets.spsBytes);
    nextContext->parameterSets.ppsLabel = describeParameterSet("pps", nextContext->parameterSets.ppsBytes);

    context_ = std::move(nextContext);
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;
    configured_ = true;
    return Status::Ok();
}

Status VideoEncoder::buildSequenceHeader(media::H264ParameterSets* outSets) const {
    if (outSets == nullptr) {
        return Status::InvalidArgument("outSets is null");
    }
    if (!configured_ || context_ == nullptr) {
        return Status::InvalidArgument("encoder is not configured");
    }

    *outSets = context_->parameterSets;
    return Status::Ok();
}

Status VideoEncoder::encodeFrame(const media::VideoFrame& frame, media::EncodedVideoAccessUnit* outUnit) {
    if (outUnit == nullptr) {
        return Status::InvalidArgument("outUnit is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr ||
        context_->packet == nullptr) {
        return Status::InvalidArgument("encoder is not configured");
    }
    if (frame.width != width_ || frame.height != height_ || frame.fps != fps_) {
        return Status::InvalidArgument("input frame does not match encoder configuration");
    }

    Status copyStatus = copyVideoFrameToAvFrame(frame, context_->frame);
    if (!copyStatus.ok()) {
        return copyStatus;
    }

    context_->frame->pts = av_rescale_q(frame.ptsMs, AVRational{1, 1000}, context_->codecContext->time_base);
    av_packet_unref(context_->packet);

    const int sendResult = avcodec_send_frame(context_->codecContext, context_->frame);
    if (sendResult < 0) {
        return Status::InternalError("avcodec_send_frame failed: " + ffmpegErrorString(sendResult));
    }

    const int receiveResult = avcodec_receive_packet(context_->codecContext, context_->packet);
    if (receiveResult == AVERROR(EAGAIN)) {
        return Status::InternalError("H.264 encoder produced no packet for the submitted frame");
    }
    if (receiveResult == AVERROR_EOF) {
        return Status::InternalError("H.264 encoder unexpectedly entered EOF while encoding");
    }
    if (receiveResult < 0) {
        return Status::InternalError("avcodec_receive_packet failed: " + ffmpegErrorString(receiveResult));
    }

    outUnit->frameIndex = frame.frameIndex;
    outUnit->keyFrame = (context_->packet->flags & AV_PKT_FLAG_KEY) != 0;
    outUnit->ptsMs =
        context_->packet->pts == AV_NOPTS_VALUE
            ? frame.ptsMs
            : av_rescale_q(context_->packet->pts, context_->codecContext->time_base, AVRational{1, 1000});
    outUnit->dtsMs =
        context_->packet->dts == AV_NOPTS_VALUE
            ? outUnit->ptsMs
            : av_rescale_q(context_->packet->dts, context_->codecContext->time_base, AVRational{1, 1000});
    outUnit->codec = "h264";

    Status payloadStatus = convertPacketToAnnexB(context_->packet, context_->nalLengthSize, &outUnit->annexBPayload);
    if (!payloadStatus.ok()) {
        av_packet_unref(context_->packet);
        return payloadStatus;
    }
    outUnit->nalSummary = summarizeAccessUnitPayload(outUnit->annexBPayload, outUnit->keyFrame);

    av_packet_unref(context_->packet);
    return Status::Ok();
}

Status VideoEncoder::pushRawFrame(std::int64_t ptsMs) {
    if (ptsMs < 0) {
        return Status::InvalidArgument("ptsMs must be >= 0");
    }
    return Status::NotImplemented("TODO(user): feed raw YUV420P frames through encodeFrame()");
}

}  // namespace live_studio::encode
