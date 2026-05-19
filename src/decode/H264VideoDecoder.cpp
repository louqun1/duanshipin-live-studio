#include "decode/H264VideoDecoder.hpp"

#include <array>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace live_studio::decode {
namespace {

std::string ffmpegErrorString(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

std::int64_t steadyClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Status copyAvFrameToVideoFrame(AVFrame* input,
                               int frameIndex,
                               std::int64_t ptsMs,
                               media::VideoFrame* outFrame) {
    if (input == nullptr || outFrame == nullptr) {
        return Status::InvalidArgument("H264VideoDecoder output arguments are null");
    }
    if (input->width <= 0 || input->height <= 0) {
        return Status::InternalError("decoded H.264 frame reports invalid width/height");
    }

    outFrame->sourceKind = "rtc-h264";
    outFrame->sourceLabel = "rtc-h264-decoded";
    outFrame->frameIndex = frameIndex;
    outFrame->width = input->width;
    outFrame->height = input->height;
    outFrame->fps = 0;
    outFrame->ptsMs = ptsMs;
    outFrame->captureWallClockNs = steadyClockNowNs();
    outFrame->hasPixelPayload = true;
    outFrame->pixelFormat = media::VideoPixelFormat::yuv420p;
    outFrame->lineStrideY = input->width;
    outFrame->lineStrideU = (input->width + 1) / 2;
    outFrame->lineStrideV = (input->width + 1) / 2;

    const int chromaHeight = (input->height + 1) / 2;
    const std::size_t yPlaneSize =
        static_cast<std::size_t>(outFrame->lineStrideY) * static_cast<std::size_t>(input->height);
    const std::size_t uPlaneSize =
        static_cast<std::size_t>(outFrame->lineStrideU) * static_cast<std::size_t>(chromaHeight);
    const std::size_t vPlaneSize =
        static_cast<std::size_t>(outFrame->lineStrideV) * static_cast<std::size_t>(chromaHeight);
    outFrame->pixelPayload.assign(yPlaneSize + uPlaneSize + vPlaneSize, 0);

    std::uint8_t* destinationY = outFrame->pixelPayload.data();
    std::uint8_t* destinationU = destinationY + yPlaneSize;
    std::uint8_t* destinationV = destinationU + uPlaneSize;

    for (int row = 0; row < input->height; ++row) {
        std::memcpy(destinationY + static_cast<std::size_t>(row) * outFrame->lineStrideY,
                    input->data[0] + static_cast<std::size_t>(row) * input->linesize[0],
                    static_cast<std::size_t>(input->width));
    }
    for (int row = 0; row < chromaHeight; ++row) {
        std::memcpy(destinationU + static_cast<std::size_t>(row) * outFrame->lineStrideU,
                    input->data[1] + static_cast<std::size_t>(row) * input->linesize[1],
                    static_cast<std::size_t>(outFrame->lineStrideU));
        std::memcpy(destinationV + static_cast<std::size_t>(row) * outFrame->lineStrideV,
                    input->data[2] + static_cast<std::size_t>(row) * input->linesize[2],
                    static_cast<std::size_t>(outFrame->lineStrideV));
    }

    return Status::Ok();
}

}  // namespace

struct H264VideoDecoder::DecoderContext {
    const AVCodec* codec = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* yuv420Frame = nullptr;
    SwsContext* scaleContext = nullptr;
    int nextFrameIndex = 0;
    std::int64_t nextPtsMs = 0;

    ~DecoderContext() {
        sws_freeContext(scaleContext);
        av_frame_free(&yuv420Frame);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);
    }
};

H264VideoDecoder::H264VideoDecoder() = default;

H264VideoDecoder::~H264VideoDecoder() = default;

Status H264VideoDecoder::configure() {
    av_log_set_level(AV_LOG_ERROR);

    std::unique_ptr<DecoderContext> nextContext = std::make_unique<DecoderContext>();
    nextContext->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (nextContext->codec == nullptr) {
        return Status::InternalError("FFmpeg could not find an H.264 decoder");
    }

    nextContext->codecContext = avcodec_alloc_context3(nextContext->codec);
    if (nextContext->codecContext == nullptr) {
        return Status::InternalError("avcodec_alloc_context3 failed for H.264 decoder");
    }

    nextContext->codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    nextContext->codecContext->codec_id = AV_CODEC_ID_H264;
    nextContext->codecContext->pkt_timebase = AVRational{1, 1000};
    nextContext->codecContext->thread_count = 2;

    const int openResult = avcodec_open2(nextContext->codecContext, nextContext->codec, nullptr);
    if (openResult < 0) {
        return Status::InternalError("avcodec_open2(H.264 decoder) failed: " + ffmpegErrorString(openResult));
    }

    nextContext->packet = av_packet_alloc();
    nextContext->frame = av_frame_alloc();
    nextContext->yuv420Frame = av_frame_alloc();
    if (nextContext->packet == nullptr || nextContext->frame == nullptr || nextContext->yuv420Frame == nullptr) {
        return Status::InternalError("failed to allocate H.264 decoder packet/frame");
    }

    context_ = std::move(nextContext);
    configured_ = true;
    return Status::Ok();
}

Status H264VideoDecoder::drainFrames(std::int64_t ptsMsHint, std::vector<media::VideoFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr) {
        return Status::InvalidArgument("H264VideoDecoder is not configured");
    }

    while (true) {
        av_frame_unref(context_->frame);
        const int receiveResult = avcodec_receive_frame(context_->codecContext, context_->frame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            return Status::Ok();
        }
        if (receiveResult < 0) {
            return Status::InternalError("avcodec_receive_frame failed for H.264 decoder: " +
                                         ffmpegErrorString(receiveResult));
        }

        AVFrame* outputFrame = context_->frame;
        if (context_->frame->format != AV_PIX_FMT_YUV420P) {
            context_->scaleContext = sws_getCachedContext(context_->scaleContext,
                                                          context_->frame->width,
                                                          context_->frame->height,
                                                          static_cast<AVPixelFormat>(context_->frame->format),
                                                          context_->frame->width,
                                                          context_->frame->height,
                                                          AV_PIX_FMT_YUV420P,
                                                          SWS_BILINEAR,
                                                          nullptr,
                                                          nullptr,
                                                          nullptr);
            if (context_->scaleContext == nullptr) {
                return Status::InternalError("sws_getCachedContext failed for H.264 decoder");
            }

            av_frame_unref(context_->yuv420Frame);
            context_->yuv420Frame->format = AV_PIX_FMT_YUV420P;
            context_->yuv420Frame->width = context_->frame->width;
            context_->yuv420Frame->height = context_->frame->height;
            const int allocResult = av_frame_get_buffer(context_->yuv420Frame, 32);
            if (allocResult < 0) {
                return Status::InternalError("av_frame_get_buffer failed for H.264 decoder output: " +
                                             ffmpegErrorString(allocResult));
            }

            const int scaleResult = sws_scale(context_->scaleContext,
                                              context_->frame->data,
                                              context_->frame->linesize,
                                              0,
                                              context_->frame->height,
                                              context_->yuv420Frame->data,
                                              context_->yuv420Frame->linesize);
            if (scaleResult <= 0) {
                return Status::InternalError("sws_scale failed for H.264 decoder output");
            }
            context_->yuv420Frame->pts = context_->frame->pts;
            outputFrame = context_->yuv420Frame;
        }

        const std::int64_t decodedPtsMs =
            outputFrame->best_effort_timestamp != AV_NOPTS_VALUE
                ? outputFrame->best_effort_timestamp
                : (outputFrame->pts != AV_NOPTS_VALUE ? outputFrame->pts : ptsMsHint);
        media::VideoFrame decodedFrame;
        Status convertStatus =
            copyAvFrameToVideoFrame(outputFrame, context_->nextFrameIndex, decodedPtsMs, &decodedFrame);
        if (!convertStatus.ok()) {
            return convertStatus;
        }

        outFrames->push_back(std::move(decodedFrame));
        context_->nextPtsMs = decodedPtsMs;
        ++context_->nextFrameIndex;
    }
}

Status H264VideoDecoder::decodeAccessUnit(const std::uint8_t* data,
                                          std::size_t size,
                                          std::int64_t ptsMsHint,
                                          std::vector<media::VideoFrame>* outFrames) {
    if (data == nullptr || size == 0) {
        return Status::InvalidArgument("H264VideoDecoder access unit is empty");
    }
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->packet == nullptr || context_->codecContext == nullptr) {
        return Status::InvalidArgument("H264VideoDecoder is not configured");
    }

    outFrames->clear();

    av_packet_unref(context_->packet);
    const int packetResult = av_new_packet(context_->packet, static_cast<int>(size));
    if (packetResult < 0) {
        return Status::InternalError("av_new_packet failed for H.264 decoder: " +
                                     ffmpegErrorString(packetResult));
    }
    std::memcpy(context_->packet->data, data, size);
    context_->packet->pts = ptsMsHint;
    context_->packet->dts = ptsMsHint;

    const int sendResult = avcodec_send_packet(context_->codecContext, context_->packet);
    av_packet_unref(context_->packet);
    if (sendResult < 0) {
        return Status::InternalError("avcodec_send_packet failed for H.264 decoder: " +
                                     ffmpegErrorString(sendResult));
    }

    return drainFrames(ptsMsHint, outFrames);
}

Status H264VideoDecoder::flush(std::vector<media::VideoFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr) {
        return Status::InvalidArgument("H264VideoDecoder is not configured");
    }

    outFrames->clear();

    const int flushResult = avcodec_send_packet(context_->codecContext, nullptr);
    if (flushResult < 0 && flushResult != AVERROR_EOF) {
        return Status::InternalError("avcodec_send_packet(null) failed for H.264 decoder: " +
                                     ffmpegErrorString(flushResult));
    }

    return drainFrames(context_->nextPtsMs, outFrames);
}

}  // namespace live_studio::decode
