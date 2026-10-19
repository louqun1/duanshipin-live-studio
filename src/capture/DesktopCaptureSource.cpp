#include "capture/DesktopCaptureSource.hpp"

#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace live_studio::capture {
struct DesktopCaptureContext {
    std::thread captureThread;
    std::atomic_bool stopRequested{false};
    std::atomic_bool threadExited{false};
    std::atomic_bool initDone{false};
    std::atomic_bool initFailed{false};
    std::string initErrorMessage;

    std::mutex mutex;
    std::condition_variable condition;
    // Keep only the freshest frames when encoding or preview falls behind.
    static constexpr std::size_t kMaxPendingEvents = 3;
    std::queue<CaptureEvent> events;
    std::atomic_int droppedEvents{0};

    std::atomic_int frameIndex{0};
    std::int64_t epochWallClockNs = 0;
    int negotiatedWidth = 0;
    int negotiatedHeight = 0;
    double negotiatedFps = 0.0;
};

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

void desktopCaptureThreadMain(const media::DesktopCaptureConfig& config, DesktopCaptureContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    avdevice_register_all();

    const AVInputFormat* inputFormat = av_find_input_format("gdigrab");
    if (inputFormat == nullptr) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->initFailed.store(true);
        ctx->initErrorMessage = "gdigrab input format not found";
        ctx->initDone.store(true);
        ctx->condition.notify_all();
        return;
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "framerate", std::to_string(config.fps).c_str(), 0);
    av_dict_set(&options, "draw_mouse", "1", 0);

    AVFormatContext* formatCtx = nullptr;
    int ret = avformat_open_input(&formatCtx, "desktop", inputFormat, &options);
    av_dict_free(&options);

    if (ret < 0) {
        spdlog::error("[DesktopCapture] avformat_open_input failed: {}", ffmpegErrorString(ret));
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->initFailed.store(true);
        ctx->initErrorMessage = "avformat_open_input failed: " + ffmpegErrorString(ret);
        ctx->initDone.store(true);
        ctx->condition.notify_all();
        return;
    }

    ret = avformat_find_stream_info(formatCtx, nullptr);
    if (ret < 0) {
        avformat_close_input(&formatCtx);
        spdlog::error("[DesktopCapture] avformat_find_stream_info failed: {}", ffmpegErrorString(ret));
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->initFailed.store(true);
        ctx->initErrorMessage = "avformat_find_stream_info failed: " + ffmpegErrorString(ret);
        ctx->initDone.store(true);
        ctx->condition.notify_all();
        return;
    }

    // gdigrab outputs raw BGRA — no decoder needed, use sws_scale directly on packet data
    int videoStreamIndex = -1;
    int sourceWidth = 0;
    int sourceHeight = 0;
    // gdigrab captures 32bpp BGRA via Windows GDI.
    // Use AV_PIX_FMT_BGRA as reported (fmt=28); create a proper AVFrame for source.
    AVPixelFormat sourceFormat = AV_PIX_FMT_BGRA;
    for (unsigned int i = 0; i < formatCtx->nb_streams; ++i) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            sourceWidth = formatCtx->streams[i]->codecpar->width;
            sourceHeight = formatCtx->streams[i]->codecpar->height;
            break;
        }
    }

    if (videoStreamIndex < 0 || sourceWidth <= 0 || sourceHeight <= 0) {
        avformat_close_input(&formatCtx);
        spdlog::error("[DesktopCapture] invalid video stream params: idx={} {}x{}",
                      videoStreamIndex, sourceWidth, sourceHeight);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->initFailed.store(true);
        ctx->initErrorMessage = "invalid video stream parameters";
        ctx->initDone.store(true);
        ctx->condition.notify_all();
        return;
    }

    SwsContext* swsCtx = sws_getContext(
        sourceWidth, sourceHeight, sourceFormat,
        sourceWidth, sourceHeight, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (swsCtx == nullptr) {
        avformat_close_input(&formatCtx);
        spdlog::error("[DesktopCapture] sws_getContext failed");
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->initFailed.store(true);
        ctx->initErrorMessage = "sws_getContext failed";
        ctx->initDone.store(true);
        ctx->condition.notify_all();
        return;
    }

    const int yuvSize = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, sourceWidth, sourceHeight, 1);
    const int yLineSize = sourceWidth;
    const int uLineSize = (sourceWidth + 1) / 2;
    const int vLineSize = (sourceWidth + 1) / 2;

    ctx->negotiatedWidth = sourceWidth;
    ctx->negotiatedHeight = sourceHeight;
    ctx->negotiatedFps = static_cast<double>(config.fps);

    AVPacket* packet = av_packet_alloc();
    AVFrame* srcFrame = av_frame_alloc();
    AVFrame* yuvFrame = av_frame_alloc();
    yuvFrame->format = AV_PIX_FMT_YUV420P;
    yuvFrame->width = sourceWidth;
    yuvFrame->height = sourceHeight;
    av_frame_get_buffer(yuvFrame, 0);

    uint8_t* yuvBuffer = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(yuvSize)));

    spdlog::info("[DesktopCapture] gdigrab ready: {}x{} fmt={} fps={}",
                 sourceWidth, sourceHeight, static_cast<int>(sourceFormat), config.fps);

    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->epochWallClockNs = steadyClockNowNs();
        ctx->initDone.store(true);
        ctx->condition.notify_all();
    }

    int readFrameIter = 0;
    int consecutiveReadErrors = 0;
    while (!ctx->stopRequested.load()) {
        spdlog::info("[DesktopCapture] loop iter={} stop={}", readFrameIter, ctx->stopRequested.load());
        ret = av_read_frame(formatCtx, packet);
        spdlog::info("[DesktopCapture] av_read_frame ret={}", ret);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                spdlog::warn("[DesktopCapture] av_read_frame EOF at iter={}", readFrameIter);
                break;
            }
            spdlog::warn("[DesktopCapture] av_read_frame err at iter={}: {} {}",
                         readFrameIter, ret, ffmpegErrorString(ret));
            if (++consecutiveReadErrors >= 10) {
                spdlog::error("[DesktopCapture] stopping after repeated read failures");
                break;
            }
            continue;
        }

        consecutiveReadErrors = 0;
        if (packet->stream_index != videoStreamIndex) {
            av_packet_unref(packet);
            continue;
        }

        // Fill source frame from packet data using av_image_fill_arrays for correct layout
        // gdigrab outputs BMP-formatted data (54-byte BMP header + BGRA pixels).
        // Read pixel data offset from BMP header (bytes 10-13, little-endian).
        uint32_t pixelOffset = 54;  // default: 14 (BITMAPFILEHEADER) + 40 (BITMAPINFOHEADER)
        if (packet->size >= 14) {
            uint32_t headerOffset = 0;
            std::memcpy(&headerOffset, packet->data + 10, sizeof(headerOffset));
            if (headerOffset >= 14 && headerOffset < static_cast<uint32_t>(packet->size)) {
                pixelOffset = headerOffset;
            }
        }
        const int bgraSize = av_image_get_buffer_size(sourceFormat, sourceWidth, sourceHeight, 1);
        if (bgraSize < 0 || pixelOffset > static_cast<uint32_t>(packet->size) ||
            packet->size - static_cast<int>(pixelOffset) < bgraSize) {
            spdlog::warn("[DesktopCapture] short or invalid BMP frame: packet={} offset={} expected={}",
                         packet->size, pixelOffset, bgraSize);
            av_packet_unref(packet);
            continue;
        }
        const uint8_t* pixelData = packet->data + pixelOffset;

        if (av_image_fill_arrays(srcFrame->data, srcFrame->linesize,
                                 pixelData, sourceFormat,
                                 sourceWidth, sourceHeight, 1) < 0) {
            av_packet_unref(packet);
            continue;
        }
        srcFrame->width = sourceWidth;
        srcFrame->height = sourceHeight;
        srcFrame->format = sourceFormat;

        av_image_fill_arrays(yuvFrame->data, yuvFrame->linesize,
                             yuvBuffer, AV_PIX_FMT_YUV420P,
                             sourceWidth, sourceHeight, 1);

        sws_scale(swsCtx,
                  srcFrame->data, srcFrame->linesize,
                  0, sourceHeight,
                  yuvFrame->data, yuvFrame->linesize);

        av_packet_unref(packet);

        CaptureEvent event;
        event.kind = CaptureEventKind::video;

        const int frameIndex = ctx->frameIndex.fetch_add(1);
        const std::int64_t wallClockNs = steadyClockNowNs();
        const std::int64_t ptsMs = wallClockNs > ctx->epochWallClockNs
            ? (wallClockNs - ctx->epochWallClockNs) / 1000000 : 0;

        event.videoFrame.sourceKind = config.sourceKind;
        event.videoFrame.sourceLabel = config.sourceLabel;
        event.videoFrame.frameIndex = frameIndex;
        event.videoFrame.width = sourceWidth;
        event.videoFrame.height = sourceHeight;
        event.videoFrame.fps = config.fps;
        event.videoFrame.ptsMs = ptsMs;
        event.videoFrame.captureWallClockNs = wallClockNs;
        event.videoFrame.hasPixelPayload = true;
        event.videoFrame.pixelFormat = media::VideoPixelFormat::yuv420p;
        event.videoFrame.lineStrideY = yLineSize;
        event.videoFrame.lineStrideU = uLineSize;
        event.videoFrame.lineStrideV = vLineSize;

        event.videoFrame.pixelPayload.resize(static_cast<size_t>(yuvSize));
        std::memcpy(event.videoFrame.pixelPayload.data(), yuvBuffer, static_cast<size_t>(yuvSize));

        if (frameIndex == 0) {
            spdlog::info("[DesktopCapture] first frame: {}x{} pts={}ms",
                         sourceWidth, sourceHeight, ptsMs);
        }

        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            if (ctx->events.size() >= DesktopCaptureContext::kMaxPendingEvents) {
                ctx->events.pop();
                const int dropped = ctx->droppedEvents.fetch_add(1) + 1;
                if (dropped == 1 || dropped % 30 == 0) {
                    spdlog::warn("[DesktopCapture] dropped {} stale capture frames", dropped);
                }
            }
            ctx->events.push(std::move(event));
            ctx->condition.notify_one();
        }

        if (config.frameCount > 0 && frameIndex + 1 >= config.frameCount) {
            break;
        }

        if ((frameIndex % 30) == 0 && frameIndex > 0) {
            spdlog::info("[DesktopCapture] frame#{} pts={}ms", frameIndex, ptsMs);
        }
        ++readFrameIter;
    }

    av_free(yuvBuffer);
    av_frame_free(&yuvFrame);
    av_frame_free(&srcFrame);
    av_packet_free(&packet);
    sws_freeContext(swsCtx);
    avformat_close_input(&formatCtx);

    spdlog::info("[DesktopCapture] gdigrab thread exit, frames={}", ctx->frameIndex.load());
    ctx->threadExited.store(true);
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->condition.notify_all();
    }
}

}  // anonymous namespace

DesktopCaptureSource::DesktopCaptureSource() = default;

DesktopCaptureSource::~DesktopCaptureSource() {
    stop();
}

Status DesktopCaptureSource::configure(const media::DesktopCaptureConfig& config) {
    if (config.width <= 0 || config.height <= 0 || config.fps <= 0) {
        return Status::InvalidArgument("desktop capture config requires positive width, height, and fps");
    }

    config_ = config;
    return Status::Ok();
}

Status DesktopCaptureSource::start() {
    if (config_.width <= 0 || config_.height <= 0 || config_.fps <= 0) {
        return Status::InvalidArgument("desktop capture settings are incomplete");
    }

    context_ = std::make_unique<DesktopCaptureContext>();
    media::DesktopCaptureConfig threadConfig = config_;
    context_->captureThread = std::thread(desktopCaptureThreadMain, threadConfig, context_.get());

    {
        std::unique_lock<std::mutex> lock(context_->mutex);
        context_->condition.wait(lock, [&]() {
            return context_->initDone.load();
        });
    }

    if (context_->initFailed.load()) {
        const std::string errorMessage = context_->initErrorMessage;
        stop();
        return Status::InternalError("desktop capture init failed: " + errorMessage);
    }

    config_.width = context_->negotiatedWidth;
    config_.height = context_->negotiatedHeight;
    started_ = true;
    return Status::Ok();
}

Status DesktopCaptureSource::readNext(CaptureEvent* outEvent) {
    if (outEvent == nullptr) {
        return Status::InvalidArgument("outEvent is null");
    }
    if (!started_ || context_ == nullptr) {
        return Status::InvalidArgument("desktop capture source is not started");
    }

    std::unique_lock<std::mutex> lock(context_->mutex);
    context_->condition.wait(lock, [&]() {
        return !context_->events.empty() || context_->threadExited.load();
    });

    if (!context_->events.empty()) {
        *outEvent = std::move(context_->events.front());
        context_->events.pop();
        return Status::Ok();
    }

    return Status::InternalError("desktop capture ended unexpectedly");
}

void DesktopCaptureSource::stop() noexcept {
    if (context_ != nullptr) {
        context_->stopRequested.store(true);
        {
            std::lock_guard<std::mutex> lock(context_->mutex);
            context_->condition.notify_all();
        }
        if (context_->captureThread.joinable()) {
            context_->captureThread.join();
        }
    }
    context_.reset();
    started_ = false;
}

int DesktopCaptureSource::pendingEventCount() const noexcept {
    if (context_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(context_->mutex);
    return static_cast<int>(context_->events.size());
}

}  // namespace live_studio::capture