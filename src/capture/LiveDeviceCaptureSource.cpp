#include "capture/LiveDeviceCaptureSource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <audioclient.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace live_studio::capture {
namespace {

template <typename T>
void safeRelease(T** value) {
    if (value != nullptr && *value != nullptr) {
        (*value)->Release();
        *value = nullptr;
    }
}

std::string ffmpegErrorString(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

std::string trim(const std::string& text) {
    std::size_t first = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf) {
        first = 3;
    }
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return text.substr(first, last - first);
}

std::int64_t steadyClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Status parsePositiveInt(const std::string& key, const std::string& value, int* outValue) {
    if (outValue == nullptr) {
        return Status::InvalidArgument("outValue is null");
    }

    std::size_t parsedLength = 0;
    int parsedValue = 0;
    try {
        parsedValue = std::stoi(value, &parsedLength);
    } catch (...) {
        return Status::InvalidArgument("capture config key '" + key + "' must be an integer");
    }

    if (parsedLength != value.size() || parsedValue <= 0) {
        return Status::InvalidArgument("capture config key '" + key + "' must be a positive integer");
    }

    *outValue = parsedValue;
    return Status::Ok();
}

Status loadLiveDeviceConfig(const std::string& configPath, media::LiveDeviceCaptureConfig* outConfig) {
    if (outConfig == nullptr) {
        return Status::InvalidArgument("outConfig is null");
    }

    std::ifstream input(configPath);
    if (!input.is_open()) {
        return Status::InvalidArgument("cannot open capture config file: " + configPath);
    }

    media::LiveDeviceCaptureConfig config;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;

        const std::string trimmedLine = trim(line);
        if (trimmedLine.empty() || trimmedLine[0] == '#') {
            continue;
        }

        const std::size_t separator = trimmedLine.find('=');
        if (separator == std::string::npos) {
            return Status::InvalidArgument("capture config line " + std::to_string(lineNumber) +
                                           " must use key=value format");
        }

        const std::string key = trim(trimmedLine.substr(0, separator));
        const std::string value = trim(trimmedLine.substr(separator + 1));
        if (key.empty()) {
            return Status::InvalidArgument("capture config line " + std::to_string(lineNumber) + " has an empty key");
        }

        Status status = Status::Ok();
        if (key == "source_label") {
            config.sourceLabel = value;
        } else if (key == "video_device") {
            config.videoDeviceId = value;
        } else if (key == "audio_device") {
            config.audioDeviceId = value;
        } else if (key == "width") {
            status = parsePositiveInt(key, value, &config.width);
        } else if (key == "height") {
            status = parsePositiveInt(key, value, &config.height);
        } else if (key == "fps") {
            status = parsePositiveInt(key, value, &config.fps);
        } else if (key == "sample_rate") {
            status = parsePositiveInt(key, value, &config.sampleRate);
        } else if (key == "channels") {
            status = parsePositiveInt(key, value, &config.channels);
        } else if (key == "frame_count") {
            status = parsePositiveInt(key, value, &config.frameCount);
        } else {
            return Status::InvalidArgument("unsupported capture config key '" + key + "'");
        }

        if (!status.ok()) {
            return status;
        }
    }

    if (config.videoDeviceId.empty() && config.audioDeviceId.empty()) {
        return Status::InvalidArgument("capture config must define video_device, audio_device, or both");
    }
    if (!config.videoDeviceId.empty() && (config.width <= 0 || config.height <= 0 || config.fps <= 0)) {
        return Status::InvalidArgument("capture config must define positive width, height, and fps when video_device is set");
    }
    if (!config.audioDeviceId.empty() && (config.sampleRate <= 0 || config.channels <= 0)) {
        return Status::InvalidArgument(
            "capture config must define positive sample_rate and channels when audio_device is set");
    }
    if (config.frameCount <= 0) {
        return Status::InvalidArgument("capture config must define positive frame_count");
    }

    *outConfig = std::move(config);
    return Status::Ok();
}

#if defined(_WIN32)

std::string hresultToString(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
    return std::string(buffer);
}

Status hresultStatus(const std::string& what, HRESULT hr) {
    return Status::InternalError(what + " failed: " + hresultToString(hr));
}

std::wstring lowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring convertMultiByte(UINT codePage, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int wideLength = MultiByteToWideChar(codePage, 0, value.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(wideLength - 1), L'\0');
    if (MultiByteToWideChar(codePage, 0, value.c_str(), -1, wide.data(), wideLength) <= 0) {
        return {};
    }
    return wide;
}

std::vector<std::wstring> buildWideStringCandidates(const std::string& original) {
    std::vector<std::wstring> candidates;
    if (original.empty()) {
        return candidates;
    }

    std::wstring utf8 = convertMultiByte(CP_UTF8, original);
    if (!utf8.empty()) {
        candidates.push_back(std::move(utf8));
    }

    std::wstring ansi = convertMultiByte(CP_ACP, original);
    if (!ansi.empty() &&
        std::none_of(candidates.begin(), candidates.end(), [&](const std::wstring& existing) { return existing == ansi; })) {
        candidates.push_back(std::move(ansi));
    }

    return candidates;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(utf8Length - 1), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, utf8.data(), utf8Length, nullptr, nullptr) <= 0) {
        return {};
    }
    return utf8;
}

std::string joinQuotedStrings(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "none";
    }

    std::string joined;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += "\"";
        joined += values[i];
        joined += "\"";
    }
    return joined;
}

bool matchesRequestedDeviceName(const std::string& requestedName, const std::wstring& deviceName) {
    if (requestedName.empty()) {
        return false;
    }
    if (requestedName == "default") {
        return true;
    }

    const std::wstring loweredDeviceName = lowerWide(deviceName);
    const std::vector<std::wstring> candidates = buildWideStringCandidates(requestedName);
    for (const std::wstring& candidate : candidates) {
        if (lowerWide(candidate) == loweredDeviceName) {
            return true;
        }
    }
    return false;
}

struct ScopedComApartment {
    HRESULT hr = E_FAIL;

    ScopedComApartment() {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    ~ScopedComApartment() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

struct MjpegDecoderState {
    const AVCodec* codec = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;

    ~MjpegDecoderState() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codecContext);
    }
};

Status ensureMjpegDecoder(MjpegDecoderState* state) {
    if (state == nullptr) {
        return Status::InvalidArgument("state is null");
    }
    if (state->codecContext != nullptr && state->frame != nullptr && state->packet != nullptr) {
        return Status::Ok();
    }

    state->codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (state->codec == nullptr) {
        return Status::InternalError("FFmpeg could not find an MJPEG decoder for Media Foundation capture");
    }

    state->codecContext = avcodec_alloc_context3(state->codec);
    state->frame = av_frame_alloc();
    state->packet = av_packet_alloc();
    if (state->codecContext == nullptr || state->frame == nullptr || state->packet == nullptr) {
        return Status::InternalError("failed to allocate MJPEG decoder objects");
    }

    const int openResult = avcodec_open2(state->codecContext, state->codec, nullptr);
    if (openResult < 0) {
        return Status::InternalError("avcodec_open2(MJPEG) failed: " + ffmpegErrorString(openResult));
    }

    return Status::Ok();
}

bool isGuidEqual(const GUID& lhs, const GUID& rhs) {
    return IsEqualGUID(lhs, rhs) != 0;
}

bool isSupportedVideoSubtype(const GUID& subtype) {
    return isGuidEqual(subtype, MFVideoFormat_MJPG) || isGuidEqual(subtype, MFVideoFormat_NV12) ||
           isGuidEqual(subtype, MFVideoFormat_YUY2) || isGuidEqual(subtype, MFVideoFormat_UYVY) ||
           isGuidEqual(subtype, MFVideoFormat_RGB32) || isGuidEqual(subtype, MFVideoFormat_RGB24);
}

int videoSubtypePreference(const GUID& subtype) {
    if (isGuidEqual(subtype, MFVideoFormat_NV12)) {
        return 0;
    }
    if (isGuidEqual(subtype, MFVideoFormat_YUY2)) {
        return 1;
    }
    if (isGuidEqual(subtype, MFVideoFormat_UYVY)) {
        return 2;
    }
    if (isGuidEqual(subtype, MFVideoFormat_RGB32)) {
        return 3;
    }
    if (isGuidEqual(subtype, MFVideoFormat_RGB24)) {
        return 4;
    }
    if (isGuidEqual(subtype, MFVideoFormat_MJPG)) {
        return 5;
    }
    return 100;
}

Status getMediaTypeFrameRate(IMFMediaType* mediaType, double* outFps) {
    if (mediaType == nullptr || outFps == nullptr) {
        return Status::InvalidArgument("media type fps outputs are null");
    }

    UINT32 numerator = 0;
    UINT32 denominator = 0;
    HRESULT hr = MFGetAttributeRatio(mediaType, MF_MT_FRAME_RATE, &numerator, &denominator);
    if (FAILED(hr) || numerator == 0 || denominator == 0) {
        *outFps = 0.0;
        return Status::Ok();
    }

    *outFps = static_cast<double>(numerator) / static_cast<double>(denominator);
    return Status::Ok();
}

Status computeMediaTypeStride(const GUID& subtype, UINT32 width, LONG* outStride) {
    if (outStride == nullptr) {
        return Status::InvalidArgument("outStride is null");
    }

    if (isGuidEqual(subtype, MFVideoFormat_NV12)) {
        *outStride = static_cast<LONG>(width);
        return Status::Ok();
    }
    if (isGuidEqual(subtype, MFVideoFormat_YUY2) || isGuidEqual(subtype, MFVideoFormat_UYVY)) {
        *outStride = static_cast<LONG>(width * 2);
        return Status::Ok();
    }
    if (isGuidEqual(subtype, MFVideoFormat_RGB32)) {
        *outStride = static_cast<LONG>(width * 4);
        return Status::Ok();
    }
    if (isGuidEqual(subtype, MFVideoFormat_RGB24)) {
        *outStride = static_cast<LONG>(((width * 3) + 3) & ~3u);
        return Status::Ok();
    }
    if (isGuidEqual(subtype, MFVideoFormat_MJPG)) {
        *outStride = 0;
        return Status::Ok();
    }

    return Status::InvalidArgument("unsupported Media Foundation video subtype");
}

struct VideoSelection {
    IMFMediaType* mediaType = nullptr;
    GUID subtype = GUID{};
    UINT32 width = 0;
    UINT32 height = 0;
    int fps = 0;
    LONG stride = 0;

    ~VideoSelection() {
        safeRelease(&mediaType);
    }
};

Status chooseBestVideoMediaType(IMFSourceReader* reader,
                                int requestedWidth,
                                int requestedHeight,
                                int requestedFps,
                                VideoSelection* outSelection) {
    if (reader == nullptr || outSelection == nullptr) {
        return Status::InvalidArgument("video selection arguments are null");
    }

    std::vector<std::string> offeredDescriptions;
    std::int64_t bestScore = std::numeric_limits<std::int64_t>::max();

    for (DWORD index = 0;; ++index) {
        IMFMediaType* nativeType = nullptr;
        const HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, &nativeType);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr)) {
            return hresultStatus("IMFSourceReader::GetNativeMediaType", hr);
        }

        GUID majorType = GUID{};
        GUID subtype = GUID{};
        UINT32 width = 0;
        UINT32 height = 0;
        double fpsValue = 0.0;
        LONG stride = 0;

        const HRESULT majorHr = nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
        const HRESULT subtypeHr = nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
        const HRESULT sizeHr = MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &width, &height);
        Status fpsStatus = getMediaTypeFrameRate(nativeType, &fpsValue);
        Status strideStatus = computeMediaTypeStride(subtype, width, &stride);
        if (!fpsStatus.ok()) {
            safeRelease(&nativeType);
            return fpsStatus;
        }

        if (FAILED(majorHr) || FAILED(subtypeHr) || FAILED(sizeHr) || !isGuidEqual(majorType, MFMediaType_Video) ||
            !isSupportedVideoSubtype(subtype) || !strideStatus.ok()) {
            safeRelease(&nativeType);
            continue;
        }

        const int fps = fpsValue > 0.0 ? static_cast<int>(std::lround(fpsValue)) : 0;
        offeredDescriptions.push_back(std::to_string(width) + "x" + std::to_string(height) + "@" +
                                      std::to_string(fps) + " subtypeScore=" +
                                      std::to_string(videoSubtypePreference(subtype)));

        const std::int64_t sizePenalty = static_cast<std::int64_t>(std::abs(static_cast<int>(width) - requestedWidth)) *
                                             1000000LL +
                                         static_cast<std::int64_t>(std::abs(static_cast<int>(height) - requestedHeight)) *
                                             1000LL;
        const std::int64_t fpsPenalty =
            static_cast<std::int64_t>(std::abs(fps - requestedFps)) * 100LL;
        const std::int64_t subtypePenalty = static_cast<std::int64_t>(videoSubtypePreference(subtype));
        const std::int64_t score = sizePenalty + fpsPenalty + subtypePenalty;

        if (score < bestScore) {
            bestScore = score;
            safeRelease(&outSelection->mediaType);
            outSelection->mediaType = nativeType;
            outSelection->subtype = subtype;
            outSelection->width = width;
            outSelection->height = height;
            outSelection->fps = fps;
            outSelection->stride = stride;
        } else {
            safeRelease(&nativeType);
        }
    }

    if (outSelection->mediaType == nullptr) {
        std::string offered = offeredDescriptions.empty() ? "none" : offeredDescriptions.front();
        for (std::size_t i = 1; i < offeredDescriptions.size(); ++i) {
            offered += ", " + offeredDescriptions[i];
        }
        return Status::InvalidArgument("Media Foundation camera does not expose a usable native media type; offered=" +
                                       offered);
    }

    return Status::Ok();
}

struct OutputVideoFrameView {
    std::array<std::uint8_t*, 4> data{};
    std::array<int, 4> linesize{};
};

void initializeVideoFrame(const media::LiveDeviceCaptureConfig& config,
                          int actualWidth,
                          int actualHeight,
                          int actualFps,
                          int frameIndex,
                          std::int64_t ptsMs,
                          media::VideoFrame* outFrame,
                          OutputVideoFrameView* outView) {
    outFrame->sourceKind = "live-device";
    outFrame->sourceLabel = config.sourceLabel;
    outFrame->frameIndex = frameIndex;
    outFrame->width = actualWidth;
    outFrame->height = actualHeight;
    outFrame->fps = actualFps;
    outFrame->ptsMs = ptsMs;
    outFrame->captureWallClockNs = steadyClockNowNs();
    outFrame->hasPixelPayload = true;
    outFrame->pixelFormat = media::VideoPixelFormat::yuv420p;
    outFrame->lineStrideY = actualWidth;
    outFrame->lineStrideU = actualWidth / 2;
    outFrame->lineStrideV = actualWidth / 2;

    const std::size_t yPlaneSize =
        static_cast<std::size_t>(outFrame->lineStrideY) * static_cast<std::size_t>(actualHeight);
    const std::size_t uPlaneSize =
        static_cast<std::size_t>(outFrame->lineStrideU) * static_cast<std::size_t>(actualHeight / 2);
    const std::size_t vPlaneSize =
        static_cast<std::size_t>(outFrame->lineStrideV) * static_cast<std::size_t>(actualHeight / 2);
    outFrame->pixelPayload.assign(yPlaneSize + uPlaneSize + vPlaneSize, 0);

    outView->data.fill(nullptr);
    outView->linesize.fill(0);
    outView->data[0] = outFrame->pixelPayload.data();
    outView->data[1] = outFrame->pixelPayload.data() + yPlaneSize;
    outView->data[2] = outView->data[1] + uPlaneSize;
    outView->linesize[0] = outFrame->lineStrideY;
    outView->linesize[1] = outFrame->lineStrideU;
    outView->linesize[2] = outFrame->lineStrideV;
}

Status scaleVideoPlanesToYuv420(const media::LiveDeviceCaptureConfig& config,
                                int actualWidth,
                                int actualHeight,
                                int actualFps,
                                int frameIndex,
                                std::int64_t ptsMs,
                                int srcWidth,
                                int srcHeight,
                                AVPixelFormat srcPixelFormat,
                                const std::uint8_t* const srcData[4],
                                const int srcLinesize[4],
                                SwsContext** inOutScaleContext,
                                media::VideoFrame* outFrame) {
    if (outFrame == nullptr || inOutScaleContext == nullptr) {
        return Status::InvalidArgument("video scale outputs are null");
    }

    OutputVideoFrameView outputView;
    initializeVideoFrame(config, actualWidth, actualHeight, actualFps, frameIndex, ptsMs, outFrame, &outputView);

    *inOutScaleContext = sws_getCachedContext(*inOutScaleContext,
                                              srcWidth,
                                              srcHeight,
                                              srcPixelFormat,
                                              actualWidth,
                                              actualHeight,
                                              AV_PIX_FMT_YUV420P,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
    if (*inOutScaleContext == nullptr) {
        return Status::InternalError("sws_getCachedContext failed for Media Foundation video capture");
    }

    const int scaledHeight = sws_scale(*inOutScaleContext,
                                       srcData,
                                       srcLinesize,
                                       0,
                                       srcHeight,
                                       outputView.data.data(),
                                       outputView.linesize.data());
    if (scaledHeight != actualHeight) {
        return Status::InternalError("sws_scale did not produce the expected Media Foundation output height");
    }

    return Status::Ok();
}

Status convertRawVideoSample(const media::LiveDeviceCaptureConfig& config,
                             const VideoSelection& selection,
                             int frameIndex,
                             std::int64_t ptsMs,
                             const std::uint8_t* sampleBytes,
                             std::size_t sampleSize,
                             SwsContext** inOutScaleContext,
                             media::VideoFrame* outFrame) {
    if (sampleBytes == nullptr || outFrame == nullptr || inOutScaleContext == nullptr) {
        return Status::InvalidArgument("raw video conversion outputs are null");
    }

    AVPixelFormat srcPixelFormat = AV_PIX_FMT_NONE;
    std::array<const std::uint8_t*, 4> srcData{};
    std::array<int, 4> srcLinesize{};

    if (isGuidEqual(selection.subtype, MFVideoFormat_NV12)) {
        const std::size_t expectedSize =
            static_cast<std::size_t>(selection.stride) * static_cast<std::size_t>(selection.height + (selection.height / 2));
        if (sampleSize < expectedSize) {
            return Status::InternalError("Media Foundation NV12 sample is smaller than expected");
        }
        srcPixelFormat = AV_PIX_FMT_NV12;
        srcData[0] = sampleBytes;
        srcData[1] = sampleBytes + (static_cast<std::size_t>(selection.stride) * static_cast<std::size_t>(selection.height));
        srcLinesize[0] = selection.stride;
        srcLinesize[1] = selection.stride;
    } else if (isGuidEqual(selection.subtype, MFVideoFormat_YUY2)) {
        srcPixelFormat = AV_PIX_FMT_YUYV422;
        srcData[0] = sampleBytes;
        srcLinesize[0] = selection.stride;
    } else if (isGuidEqual(selection.subtype, MFVideoFormat_UYVY)) {
        srcPixelFormat = AV_PIX_FMT_UYVY422;
        srcData[0] = sampleBytes;
        srcLinesize[0] = selection.stride;
    } else if (isGuidEqual(selection.subtype, MFVideoFormat_RGB32)) {
        srcPixelFormat = AV_PIX_FMT_BGRA;
        if (selection.stride >= 0) {
            srcData[0] = sampleBytes;
            srcLinesize[0] = selection.stride;
        } else {
            srcData[0] = sampleBytes +
                         (static_cast<std::size_t>(selection.height - 1) *
                          static_cast<std::size_t>(std::abs(selection.stride)));
            srcLinesize[0] = selection.stride;
        }
    } else if (isGuidEqual(selection.subtype, MFVideoFormat_RGB24)) {
        srcPixelFormat = AV_PIX_FMT_BGR24;
        if (selection.stride >= 0) {
            srcData[0] = sampleBytes;
            srcLinesize[0] = selection.stride;
        } else {
            srcData[0] = sampleBytes +
                         (static_cast<std::size_t>(selection.height - 1) *
                          static_cast<std::size_t>(std::abs(selection.stride)));
            srcLinesize[0] = selection.stride;
        }
    } else {
        return Status::InvalidArgument("unsupported raw Media Foundation subtype");
    }

    return scaleVideoPlanesToYuv420(config,
                                    static_cast<int>(selection.width),
                                    static_cast<int>(selection.height),
                                    selection.fps,
                                    frameIndex,
                                    ptsMs,
                                    static_cast<int>(selection.width),
                                    static_cast<int>(selection.height),
                                    srcPixelFormat,
                                    srcData.data(),
                                    srcLinesize.data(),
                                    inOutScaleContext,
                                    outFrame);
}

Status decodeMjpegVideoSample(const media::LiveDeviceCaptureConfig& config,
                              const VideoSelection& selection,
                              int frameIndex,
                              std::int64_t ptsMs,
                              const std::uint8_t* sampleBytes,
                              std::size_t sampleSize,
                              MjpegDecoderState* decoderState,
                              SwsContext** inOutScaleContext,
                              media::VideoFrame* outFrame) {
    if (sampleBytes == nullptr || decoderState == nullptr || outFrame == nullptr || inOutScaleContext == nullptr) {
        return Status::InvalidArgument("MJPEG video conversion outputs are null");
    }

    Status status = ensureMjpegDecoder(decoderState);
    if (!status.ok()) {
        return status;
    }

    av_packet_unref(decoderState->packet);
    const int packetResult = av_new_packet(decoderState->packet, static_cast<int>(sampleSize));
    if (packetResult < 0) {
        return Status::InternalError("av_new_packet failed for Media Foundation MJPEG sample: " +
                                     ffmpegErrorString(packetResult));
    }
    std::memcpy(decoderState->packet->data, sampleBytes, sampleSize);

    const int sendResult = avcodec_send_packet(decoderState->codecContext, decoderState->packet);
    if (sendResult < 0) {
        av_packet_unref(decoderState->packet);
        return Status::InternalError("avcodec_send_packet failed for Media Foundation MJPEG sample: " +
                                     ffmpegErrorString(sendResult));
    }

    const int receiveResult = avcodec_receive_frame(decoderState->codecContext, decoderState->frame);
    av_packet_unref(decoderState->packet);
    if (receiveResult < 0) {
        return Status::InternalError("avcodec_receive_frame failed for Media Foundation MJPEG sample: " +
                                     ffmpegErrorString(receiveResult));
    }

    std::array<const std::uint8_t*, 4> srcData{};
    std::array<int, 4> srcLinesize{};
    for (int i = 0; i < 4; ++i) {
        srcData[static_cast<std::size_t>(i)] = decoderState->frame->data[i];
        srcLinesize[static_cast<std::size_t>(i)] = decoderState->frame->linesize[i];
    }

    return scaleVideoPlanesToYuv420(config,
                                    static_cast<int>(selection.width),
                                    static_cast<int>(selection.height),
                                    selection.fps,
                                    frameIndex,
                                    ptsMs,
                                    decoderState->frame->width,
                                    decoderState->frame->height,
                                    static_cast<AVPixelFormat>(decoderState->frame->format),
                                    srcData.data(),
                                    srcLinesize.data(),
                                    inOutScaleContext,
                                    outFrame);
}

Status sampleToVideoFrame(const media::LiveDeviceCaptureConfig& config,
                          const VideoSelection& selection,
                          int frameIndex,
                          std::int64_t ptsMs,
                          IMFSample* sample,
                          MjpegDecoderState* decoderState,
                          SwsContext** inOutScaleContext,
                          media::VideoFrame* outFrame) {
    if (sample == nullptr) {
        return Status::InvalidArgument("sample is null");
    }

    IMFMediaBuffer* buffer = nullptr;
    BYTE* sampleBytes = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;

    const HRESULT bufferHr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(bufferHr)) {
        return hresultStatus("IMFSample::ConvertToContiguousBuffer", bufferHr);
    }

    const HRESULT lockHr = buffer->Lock(&sampleBytes, &maxLength, &currentLength);
    if (FAILED(lockHr)) {
        safeRelease(&buffer);
        return hresultStatus("IMFMediaBuffer::Lock", lockHr);
    }

    Status status = Status::Ok();
    if (isGuidEqual(selection.subtype, MFVideoFormat_MJPG)) {
        status = decodeMjpegVideoSample(config,
                                        selection,
                                        frameIndex,
                                        ptsMs,
                                        sampleBytes,
                                        currentLength,
                                        decoderState,
                                        inOutScaleContext,
                                        outFrame);
    } else {
        status = convertRawVideoSample(config,
                                       selection,
                                       frameIndex,
                                       ptsMs,
                                       sampleBytes,
                                       currentLength,
                                       inOutScaleContext,
                                       outFrame);
    }

    buffer->Unlock();
    safeRelease(&buffer);
    return status;
}

enum class WasapiSampleKind {
    pcm16,
    pcm32,
    float32,
};

struct WasapiFormatInfo {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    WasapiSampleKind kind = WasapiSampleKind::pcm16;
};

Status describeWasapiFormat(const WAVEFORMATEX* waveFormat, WasapiFormatInfo* outInfo) {
    if (waveFormat == nullptr || outInfo == nullptr) {
        return Status::InvalidArgument("WASAPI format outputs are null");
    }

    GUID subFormat = GUID{};
    WORD validBitsPerSample = waveFormat->wBitsPerSample;

    if (waveFormat->wFormatTag == WAVE_FORMAT_PCM) {
        subFormat = KSDATAFORMAT_SUBTYPE_PCM;
    } else if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    } else if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(waveFormat);
        subFormat = extensible->SubFormat;
        validBitsPerSample = extensible->Samples.wValidBitsPerSample;
    } else {
        return Status::InvalidArgument("unsupported WASAPI wave format tag");
    }

    outInfo->sampleRate = static_cast<int>(waveFormat->nSamplesPerSec);
    outInfo->channels = static_cast<int>(waveFormat->nChannels);
    outInfo->bitsPerSample = static_cast<int>(validBitsPerSample);

    if (isGuidEqual(subFormat, KSDATAFORMAT_SUBTYPE_PCM) && validBitsPerSample == 16) {
        outInfo->kind = WasapiSampleKind::pcm16;
        return Status::Ok();
    }
    if (isGuidEqual(subFormat, KSDATAFORMAT_SUBTYPE_PCM) && validBitsPerSample == 32) {
        outInfo->kind = WasapiSampleKind::pcm32;
        return Status::Ok();
    }
    if (isGuidEqual(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && validBitsPerSample == 32) {
        outInfo->kind = WasapiSampleKind::float32;
        return Status::Ok();
    }

    return Status::InvalidArgument("unsupported WASAPI sample format");
}

std::int16_t clampFloatToS16(float value) {
    const float clamped = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
}

Status wasapiPacketToAudioFrame(const media::LiveDeviceCaptureConfig& config,
                                const WasapiFormatInfo& formatInfo,
                                int frameIndex,
                                std::int64_t ptsMs,
                                const BYTE* packetData,
                                UINT32 frameCount,
                                DWORD flags,
                                media::AudioFrame* outFrame) {
    if (outFrame == nullptr) {
        return Status::InvalidArgument("outFrame is null");
    }

    outFrame->sourceKind = "live-device";
    outFrame->sourceLabel = config.sourceLabel;
    outFrame->frameIndex = frameIndex;
    outFrame->sampleRate = formatInfo.sampleRate;
    outFrame->channels = formatInfo.channels;
    outFrame->samplesPerChannel = static_cast<int>(frameCount);
    outFrame->ptsMs = ptsMs;
    outFrame->captureWallClockNs = steadyClockNowNs();
    outFrame->hasSamplePayload = true;
    outFrame->sampleFormat = media::AudioSampleFormat::s16_interleaved;
    outFrame->samplePayload.assign(static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(formatInfo.channels) *
                                       sizeof(std::int16_t),
                                   0);

    std::int16_t* output = reinterpret_cast<std::int16_t*>(outFrame->samplePayload.data());
    const std::size_t sampleCount = static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(formatInfo.channels);

    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || packetData == nullptr) {
        std::fill(output, output + sampleCount, static_cast<std::int16_t>(0));
        return Status::Ok();
    }

    switch (formatInfo.kind) {
    case WasapiSampleKind::pcm16: {
        std::memcpy(output, packetData, sampleCount * sizeof(std::int16_t));
        return Status::Ok();
    }
    case WasapiSampleKind::pcm32: {
        const std::int32_t* input = reinterpret_cast<const std::int32_t*>(packetData);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            output[i] = static_cast<std::int16_t>(input[i] >> 16);
        }
        return Status::Ok();
    }
    case WasapiSampleKind::float32: {
        const float* input = reinterpret_cast<const float*>(packetData);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            output[i] = clampFloatToS16(input[i]);
        }
        return Status::Ok();
    }
    }

    return Status::InvalidArgument("unsupported WASAPI conversion kind");
}

struct VideoCaptureThreadState {
    IMFMediaSource* mediaSource = nullptr;
    IMFSourceReader* sourceReader = nullptr;
    SwsContext* scaleContext = nullptr;
    MjpegDecoderState mjpegDecoder;
    VideoSelection selection;
    std::string selectedDeviceName;
    std::int64_t firstSampleTime = -1;
    int nextFrameIndex = 0;

    ~VideoCaptureThreadState() {
        sws_freeContext(scaleContext);
        safeRelease(&sourceReader);
        safeRelease(&mediaSource);
    }
};

struct AudioCaptureThreadState {
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    WasapiFormatInfo formatInfo;
    std::string selectedDeviceName;
    std::int64_t framesCaptured = 0;
    int nextFrameIndex = 0;

    ~AudioCaptureThreadState() {
        if (audioClient != nullptr) {
            audioClient->Stop();
        }
        if (mixFormat != nullptr) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }
        safeRelease(&captureClient);
        safeRelease(&audioClient);
        safeRelease(&device);
    }
};

Status selectVideoDeviceActivate(const std::string& requestedDeviceId,
                                 IMFActivate** outSelectedActivate,
                                 std::string* outSelectedDeviceName,
                                 std::vector<std::string>* outAvailableNames) {
    if (outSelectedActivate == nullptr || outSelectedDeviceName == nullptr || outAvailableNames == nullptr) {
        return Status::InvalidArgument("video device selection outputs are null");
    }

    *outSelectedActivate = nullptr;
    outSelectedDeviceName->clear();
    outAvailableNames->clear();

    IMFAttributes* enumAttributes = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;

    const HRESULT attributesHr = MFCreateAttributes(&enumAttributes, 1);
    if (FAILED(attributesHr)) {
        return hresultStatus("MFCreateAttributes(video enum)", attributesHr);
    }

    HRESULT hr = enumAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
        safeRelease(&enumAttributes);
        return hresultStatus("IMFAttributes::SetGUID(video source type)", hr);
    }

    hr = MFEnumDeviceSources(enumAttributes, &devices, &deviceCount);
    safeRelease(&enumAttributes);
    if (FAILED(hr)) {
        return hresultStatus("MFEnumDeviceSources(video)", hr);
    }

    IMFActivate* selectedActivate = nullptr;
    std::string selectedDeviceName;
    for (UINT32 i = 0; i < deviceCount; ++i) {
        WCHAR* friendlyName = nullptr;
        UINT32 friendlyNameLength = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                     &friendlyName,
                                                     &friendlyNameLength)) &&
            friendlyName != nullptr) {
            const std::wstring wideName(friendlyName, friendlyNameLength);
            const std::string utf8Name = wideToUtf8(wideName);
            outAvailableNames->push_back(utf8Name);
            if (selectedActivate == nullptr && matchesRequestedDeviceName(requestedDeviceId, wideName)) {
                selectedActivate = devices[i];
                selectedActivate->AddRef();
                selectedDeviceName = utf8Name;
            }
            CoTaskMemFree(friendlyName);
        }
    }

    for (UINT32 i = 0; i < deviceCount; ++i) {
        safeRelease(&devices[i]);
    }
    CoTaskMemFree(devices);

    if (selectedActivate == nullptr) {
        return Status::InvalidArgument("camera device not found for Media Foundation: requested=\"" +
                                       requestedDeviceId + "\" offered=" + joinQuotedStrings(*outAvailableNames));
    }

    *outSelectedActivate = selectedActivate;
    *outSelectedDeviceName = std::move(selectedDeviceName);
    return Status::Ok();
}

Status initializeVideoSourceReader(IMFActivate* selectedActivate,
                                   const media::LiveDeviceCaptureConfig& config,
                                   VideoCaptureThreadState* outState) {
    if (selectedActivate == nullptr || outState == nullptr) {
        return Status::InvalidArgument("video source reader arguments are null");
    }

    HRESULT hr = selectedActivate->ActivateObject(IID_IMFMediaSource, reinterpret_cast<void**>(&outState->mediaSource));
    if (FAILED(hr)) {
        return hresultStatus("IMFActivate::ActivateObject(camera)", hr);
    }

    IMFAttributes* readerAttributes = nullptr;
    hr = MFCreateAttributes(&readerAttributes, 1);
    if (FAILED(hr)) {
        return hresultStatus("MFCreateAttributes(source reader)", hr);
    }

    hr = readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    if (FAILED(hr)) {
        safeRelease(&readerAttributes);
        return hresultStatus("IMFAttributes::SetUINT32(MF_READWRITE_DISABLE_CONVERTERS)", hr);
    }

    hr = MFCreateSourceReaderFromMediaSource(outState->mediaSource, readerAttributes, &outState->sourceReader);
    safeRelease(&readerAttributes);
    if (FAILED(hr)) {
        return hresultStatus("MFCreateSourceReaderFromMediaSource", hr);
    }

    hr = outState->sourceReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(hr)) {
        return hresultStatus("IMFSourceReader::SetStreamSelection(all streams)", hr);
    }

    hr = outState->sourceReader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr)) {
        return hresultStatus("IMFSourceReader::SetStreamSelection(first video stream)", hr);
    }

    Status selectionStatus =
        chooseBestVideoMediaType(outState->sourceReader, config.width, config.height, config.fps, &outState->selection);
    if (!selectionStatus.ok()) {
        return selectionStatus;
    }

    hr = outState->sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                     nullptr,
                                                     outState->selection.mediaType);
    if (FAILED(hr)) {
        return hresultStatus("IMFSourceReader::SetCurrentMediaType(video)", hr);
    }

    return Status::Ok();
}

Status createSourceReaderForCamera(const media::LiveDeviceCaptureConfig& config,
                                   VideoCaptureThreadState* outState) {
    if (outState == nullptr) {
        return Status::InvalidArgument("outState is null");
    }

    const HRESULT startupHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startupHr)) {
        return hresultStatus("MFStartup", startupHr);
    }

    IMFActivate* selectedActivate = nullptr;
    std::vector<std::string> availableDeviceNames;
    Status status =
        selectVideoDeviceActivate(config.videoDeviceId, &selectedActivate, &outState->selectedDeviceName, &availableDeviceNames);
    if (!status.ok()) {
        MFShutdown();
        return status;
    }

    status = initializeVideoSourceReader(selectedActivate, config, outState);
    safeRelease(&selectedActivate);
    if (!status.ok()) {
        MFShutdown();
        return status;
    }

    return Status::Ok();
}

Status tryGetAudioDeviceFriendlyName(IMMDevice* device, std::string* outName) {
    if (device == nullptr || outName == nullptr) {
        return Status::InvalidArgument("audio device friendly-name arguments are null");
    }

    IPropertyStore* propertyStore = nullptr;
    PROPVARIANT friendlyName;
    PropVariantInit(&friendlyName);

    const HRESULT storeHr = device->OpenPropertyStore(STGM_READ, &propertyStore);
    if (FAILED(storeHr)) {
        return hresultStatus("IMMDevice::OpenPropertyStore", storeHr);
    }

    const HRESULT valueHr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
    safeRelease(&propertyStore);
    if (FAILED(valueHr)) {
        PropVariantClear(&friendlyName);
        return hresultStatus("IPropertyStore::GetValue(PKEY_Device_FriendlyName)", valueHr);
    }

    if (friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal != nullptr) {
        *outName = wideToUtf8(friendlyName.pwszVal);
    } else {
        outName->clear();
    }
    PropVariantClear(&friendlyName);
    return Status::Ok();
}

Status selectAudioDevice(const std::string& requestedDeviceId,
                         IMMDevice** outDevice,
                         std::string* outSelectedDeviceName,
                         std::vector<std::string>* outAvailableNames) {
    if (outDevice == nullptr || outSelectedDeviceName == nullptr || outAvailableNames == nullptr) {
        return Status::InvalidArgument("audio device selection outputs are null");
    }

    *outDevice = nullptr;
    outSelectedDeviceName->clear();
    outAvailableNames->clear();

    IMMDeviceEnumerator* enumerator = nullptr;
    const HRESULT enumHr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator),
                         nullptr,
                         CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(&enumerator));
    if (FAILED(enumHr)) {
        return hresultStatus("CoCreateInstance(MMDeviceEnumerator)", enumHr);
    }

    IMMDeviceCollection* collection = nullptr;
    const HRESULT collectionHr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(collectionHr)) {
        safeRelease(&enumerator);
        return hresultStatus("IMMDeviceEnumerator::EnumAudioEndpoints", collectionHr);
    }

    UINT count = 0;
    const HRESULT countHr = collection->GetCount(&count);
    if (FAILED(countHr)) {
        safeRelease(&collection);
        safeRelease(&enumerator);
        return hresultStatus("IMMDeviceCollection::GetCount", countHr);
    }

    IMMDevice* defaultDevice = nullptr;
    if (requestedDeviceId == "default") {
        const HRESULT defaultHr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDevice);
        if (FAILED(defaultHr)) {
            safeRelease(&collection);
            safeRelease(&enumerator);
            return hresultStatus("IMMDeviceEnumerator::GetDefaultAudioEndpoint", defaultHr);
        }
    }

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        IPropertyStore* propertyStore = nullptr;
        PROPVARIANT friendlyName;
        PropVariantInit(&friendlyName);

        const HRESULT itemHr = collection->Item(i, &device);
        if (FAILED(itemHr)) {
            continue;
        }

        const HRESULT storeHr = device->OpenPropertyStore(STGM_READ, &propertyStore);
        if (SUCCEEDED(storeHr)) {
            const HRESULT valueHr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
            if (SUCCEEDED(valueHr) && friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal != nullptr) {
                const std::wstring wideName(friendlyName.pwszVal);
                const std::string utf8Name = wideToUtf8(wideName);
                outAvailableNames->push_back(utf8Name);
                if (*outDevice == nullptr &&
                    ((requestedDeviceId == "default" && defaultDevice != nullptr && device == defaultDevice) ||
                     matchesRequestedDeviceName(requestedDeviceId, wideName))) {
                    *outDevice = device;
                    (*outDevice)->AddRef();
                    *outSelectedDeviceName = utf8Name;
                }
            }
        }

        PropVariantClear(&friendlyName);
        safeRelease(&propertyStore);
        safeRelease(&device);
    }

    if (*outDevice == nullptr && requestedDeviceId == "default" && defaultDevice != nullptr) {
        *outDevice = defaultDevice;
        (*outDevice)->AddRef();
        Status friendlyNameStatus = tryGetAudioDeviceFriendlyName(*outDevice, outSelectedDeviceName);
        if (!friendlyNameStatus.ok()) {
            outSelectedDeviceName->clear();
        }
    }

    safeRelease(&defaultDevice);
    safeRelease(&collection);
    safeRelease(&enumerator);

    if (requestedDeviceId.empty()) {
        return Status::Ok();
    }

    if (*outDevice == nullptr) {
        return Status::InvalidArgument("microphone device not found for WASAPI: requested=\"" + requestedDeviceId +
                                       "\" offered=" + joinQuotedStrings(*outAvailableNames));
    }

    return Status::Ok();
}

Status initializeAudioClientForMicrophone(const media::LiveDeviceCaptureConfig& config,
                                          bool startCapture,
                                          AudioCaptureThreadState* outState) {
    if (outState == nullptr) {
        return Status::InvalidArgument("outState is null");
    }

    std::vector<std::string> availableDeviceNames;
    Status status =
        selectAudioDevice(config.audioDeviceId, &outState->device, &outState->selectedDeviceName, &availableDeviceNames);
    if (!status.ok()) {
        return status;
    }

    const HRESULT activateHr = outState->device->Activate(__uuidof(IAudioClient),
                                                          CLSCTX_ALL,
                                                          nullptr,
                                                          reinterpret_cast<void**>(&outState->audioClient));
    if (FAILED(activateHr)) {
        return hresultStatus("IMMDevice::Activate(IAudioClient)", activateHr);
    }

    const HRESULT mixFormatHr = outState->audioClient->GetMixFormat(&outState->mixFormat);
    if (FAILED(mixFormatHr)) {
        return hresultStatus("IAudioClient::GetMixFormat", mixFormatHr);
    }

    Status formatStatus = describeWasapiFormat(outState->mixFormat, &outState->formatInfo);
    if (!formatStatus.ok()) {
        return formatStatus;
    }

    const REFERENCE_TIME requestedBufferDuration = 1000000;
    const HRESULT initHr = outState->audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                             0,
                                                             requestedBufferDuration,
                                                             0,
                                                             outState->mixFormat,
                                                             nullptr);
    if (FAILED(initHr)) {
        return hresultStatus("IAudioClient::Initialize(shared capture)", initHr);
    }

    const HRESULT serviceHr =
        outState->audioClient->GetService(__uuidof(IAudioCaptureClient),
                                          reinterpret_cast<void**>(&outState->captureClient));
    if (FAILED(serviceHr)) {
        return hresultStatus("IAudioClient::GetService(IAudioCaptureClient)", serviceHr);
    }

    if (startCapture) {
        const HRESULT startHr = outState->audioClient->Start();
        if (FAILED(startHr)) {
            return hresultStatus("IAudioClient::Start", startHr);
        }
    }

    return Status::Ok();
}

Status createAudioClientForMicrophone(const media::LiveDeviceCaptureConfig& config,
                                      AudioCaptureThreadState* outState) {
    return initializeAudioClientForMicrophone(config, true, outState);
}

#endif  // defined(_WIN32)

}  // namespace

struct LiveDeviceContext {
    std::mutex mutex;
    std::condition_variable condition;
    std::queue<CaptureEvent> events;
    std::thread videoThread;
    std::thread audioThread;
    std::atomic_bool stopRequested{false};

    bool videoInitDone = false;
    Status videoInitStatus = Status::Ok();
    bool audioInitDone = true;
    Status audioInitStatus = Status::Ok();

    bool videoThreadExited = false;
    bool audioThreadExited = true;
    Status runtimeFailure = Status::Ok();

    int negotiatedVideoWidth = 0;
    int negotiatedVideoHeight = 0;
    int negotiatedVideoFps = 0;
    int negotiatedSampleRate = 0;
    int negotiatedChannels = 0;
    std::string selectedVideoDeviceName;
    std::string selectedAudioDeviceName;

    void pushEvent(CaptureEvent event) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push(std::move(event));
        condition.notify_one();
    }

    void setVideoInit(Status status, int width, int height, int fps, std::string selectedDeviceName) {
        std::lock_guard<std::mutex> lock(mutex);
        videoInitDone = true;
        videoInitStatus = std::move(status);
        negotiatedVideoWidth = width;
        negotiatedVideoHeight = height;
        negotiatedVideoFps = fps;
        selectedVideoDeviceName = std::move(selectedDeviceName);
        condition.notify_all();
    }

    void setAudioInit(Status status, int sampleRate, int channels, std::string selectedDeviceName) {
        std::lock_guard<std::mutex> lock(mutex);
        audioInitDone = true;
        audioInitStatus = std::move(status);
        negotiatedSampleRate = sampleRate;
        negotiatedChannels = channels;
        selectedAudioDeviceName = std::move(selectedDeviceName);
        condition.notify_all();
    }

    void setThreadExited(bool isVideoThread) {
        std::lock_guard<std::mutex> lock(mutex);
        if (isVideoThread) {
            videoThreadExited = true;
        } else {
            audioThreadExited = true;
        }
        condition.notify_all();
    }

    void fail(Status status) {
        std::lock_guard<std::mutex> lock(mutex);
        if (runtimeFailure.ok()) {
            runtimeFailure = std::move(status);
        }
        condition.notify_all();
    }
};

namespace {

#if defined(_WIN32)

void videoCaptureThreadMain(media::LiveDeviceCaptureConfig config, LiveDeviceContext* context) {
    ScopedComApartment apartment;
    if (FAILED(apartment.hr)) {
        context->setVideoInit(hresultStatus("CoInitializeEx(video thread)", apartment.hr), 0, 0, 0, {});
        context->setThreadExited(true);
        return;
    }

    VideoCaptureThreadState state;
    const Status createStatus = createSourceReaderForCamera(config, &state);
    if (!createStatus.ok()) {
        context->setVideoInit(createStatus, 0, 0, 0, {});
        context->setThreadExited(true);
        return;
    }

    context->setVideoInit(Status::Ok(),
                          static_cast<int>(state.selection.width),
                          static_cast<int>(state.selection.height),
                          state.selection.fps > 0 ? state.selection.fps : config.fps,
                          state.selectedDeviceName);

    while (!context->stopRequested.load()) {
        DWORD actualStreamIndex = 0;
        DWORD streamFlags = 0;
        LONGLONG sampleTime = 0;
        IMFSample* sample = nullptr;

        const HRESULT readHr = state.sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                              0,
                                                              &actualStreamIndex,
                                                              &streamFlags,
                                                              &sampleTime,
                                                              &sample);
        if (FAILED(readHr)) {
            context->fail(hresultStatus("IMFSourceReader::ReadSample(video)", readHr));
            break;
        }
        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            context->fail(Status::InternalError("Media Foundation video stream ended unexpectedly"));
            safeRelease(&sample);
            break;
        }
        if ((streamFlags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            safeRelease(&sample);
            continue;
        }
        if (sample == nullptr) {
            continue;
        }

        if (state.firstSampleTime < 0) {
            state.firstSampleTime = sampleTime;
        }
        const std::int64_t ptsMs = sampleTime >= state.firstSampleTime
                                       ? (sampleTime - state.firstSampleTime) / 10000
                                       : (static_cast<std::int64_t>(state.nextFrameIndex) * 1000) /
                                             static_cast<std::int64_t>(std::max(1, state.selection.fps));

        media::VideoFrame frame;
        Status frameStatus = sampleToVideoFrame(config,
                                                state.selection,
                                                state.nextFrameIndex,
                                                ptsMs,
                                                sample,
                                                &state.mjpegDecoder,
                                                &state.scaleContext,
                                                &frame);
        safeRelease(&sample);
        if (!frameStatus.ok()) {
            context->fail(frameStatus);
            break;
        }

        CaptureEvent event;
        event.kind = CaptureEventKind::video;
        event.videoFrame = std::move(frame);
        ++state.nextFrameIndex;
        context->pushEvent(std::move(event));
    }

    MFShutdown();
    context->setThreadExited(true);
}

void audioCaptureThreadMain(media::LiveDeviceCaptureConfig config, LiveDeviceContext* context) {
    ScopedComApartment apartment;
    if (FAILED(apartment.hr)) {
        context->setAudioInit(hresultStatus("CoInitializeEx(audio thread)", apartment.hr), 0, 0, {});
        context->setThreadExited(false);
        return;
    }

    AudioCaptureThreadState state;
    const Status createStatus = createAudioClientForMicrophone(config, &state);
    if (!createStatus.ok()) {
        context->setAudioInit(createStatus, 0, 0, {});
        context->setThreadExited(false);
        return;
    }

    context->setAudioInit(Status::Ok(), state.formatInfo.sampleRate, state.formatInfo.channels, state.selectedDeviceName);

    while (!context->stopRequested.load()) {
        UINT32 packetFrameCount = 0;
        const HRESULT packetHr = state.captureClient->GetNextPacketSize(&packetFrameCount);
        if (FAILED(packetHr)) {
            context->fail(hresultStatus("IAudioCaptureClient::GetNextPacketSize", packetHr));
            break;
        }

        if (packetFrameCount == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        BYTE* packetData = nullptr;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        const HRESULT bufferHr =
            state.captureClient->GetBuffer(&packetData, &packetFrameCount, &flags, &devicePosition, &qpcPosition);
        if (FAILED(bufferHr)) {
            context->fail(hresultStatus("IAudioCaptureClient::GetBuffer", bufferHr));
            break;
        }

        const std::int64_t ptsMs = (state.framesCaptured * 1000) /
                                   static_cast<std::int64_t>(std::max(1, state.formatInfo.sampleRate));
        media::AudioFrame frame;
        Status frameStatus =
            wasapiPacketToAudioFrame(config, state.formatInfo, state.nextFrameIndex, ptsMs, packetData, packetFrameCount, flags, &frame);
        const HRESULT releaseHr = state.captureClient->ReleaseBuffer(packetFrameCount);
        if (FAILED(releaseHr)) {
            context->fail(hresultStatus("IAudioCaptureClient::ReleaseBuffer", releaseHr));
            break;
        }
        if (!frameStatus.ok()) {
            context->fail(frameStatus);
            break;
        }

        state.framesCaptured += packetFrameCount;
        ++state.nextFrameIndex;

        CaptureEvent event;
        event.kind = CaptureEventKind::audio;
        event.audioFrame = std::move(frame);
        context->pushEvent(std::move(event));
    }

    context->setThreadExited(false);
}

#endif  // defined(_WIN32)

}  // namespace

Status loadLiveDeviceCaptureConfig(const std::string& configPath, media::LiveDeviceCaptureConfig* outConfig) {
    if (configPath.empty()) {
        return Status::InvalidArgument("configPath is empty");
    }
    return loadLiveDeviceConfig(configPath, outConfig);
}

Status probeLiveDeviceCaptureConfig(const media::LiveDeviceCaptureConfig& config, LiveDeviceProbeResult* outProbe) {
    if (outProbe == nullptr) {
        return Status::InvalidArgument("outProbe is null");
    }
    if (config.videoDeviceId.empty() && config.audioDeviceId.empty()) {
        return Status::InvalidArgument("probe requires at least one enabled device");
    }
    if (!config.videoDeviceId.empty() && (config.width <= 0 || config.height <= 0 || config.fps <= 0)) {
        return Status::InvalidArgument("probe requires positive width, height, and fps when video is enabled");
    }
    if (!config.audioDeviceId.empty() && (config.sampleRate <= 0 || config.channels <= 0)) {
        return Status::InvalidArgument("probe requires positive sampleRate and channels when audio is enabled");
    }

    *outProbe = LiveDeviceProbeResult{};

#if !defined(_WIN32)
    return Status::NotImplemented("Media Foundation / WASAPI live capture is only implemented on Windows");
#else
    ScopedComApartment apartment;
    if (FAILED(apartment.hr)) {
        return hresultStatus("CoInitializeEx(probe thread)", apartment.hr);
    }

    if (!config.videoDeviceId.empty()) {
        std::vector<std::string> availableVideoDeviceNames;
        std::string selectedVideoDeviceName;
        int negotiatedVideoWidth = 0;
        int negotiatedVideoHeight = 0;
        int negotiatedVideoFps = 0;

        {
            VideoCaptureThreadState videoState;
            Status status = createSourceReaderForCamera(config, &videoState);
            if (!status.ok()) {
                return status;
            }

            IMFActivate* selectedActivate = nullptr;
            status = selectVideoDeviceActivate(
                config.videoDeviceId, &selectedActivate, &selectedVideoDeviceName, &availableVideoDeviceNames);
            safeRelease(&selectedActivate);
            if (!status.ok()) {
                MFShutdown();
                return status;
            }

            negotiatedVideoWidth = static_cast<int>(videoState.selection.width);
            negotiatedVideoHeight = static_cast<int>(videoState.selection.height);
            negotiatedVideoFps = videoState.selection.fps > 0 ? videoState.selection.fps : config.fps;
        }

        outProbe->availableVideoDeviceNames = std::move(availableVideoDeviceNames);
        outProbe->selectedVideoDeviceName =
            !selectedVideoDeviceName.empty() ? std::move(selectedVideoDeviceName) : config.videoDeviceId;
        outProbe->negotiatedVideoWidth = negotiatedVideoWidth;
        outProbe->negotiatedVideoHeight = negotiatedVideoHeight;
        outProbe->negotiatedVideoFps = negotiatedVideoFps;
    }

    if (!config.audioDeviceId.empty()) {
        {
            IMMDevice* selectedDevice = nullptr;
            std::string selectedAudioDeviceName;
            std::vector<std::string> availableAudioDeviceNames;
            Status status = selectAudioDevice(
                config.audioDeviceId, &selectedDevice, &selectedAudioDeviceName, &availableAudioDeviceNames);
            safeRelease(&selectedDevice);
            if (!status.ok()) {
                MFShutdown();
                return status;
            }

            outProbe->availableAudioDeviceNames = std::move(availableAudioDeviceNames);
            outProbe->selectedAudioDeviceName = std::move(selectedAudioDeviceName);
        }

        AudioCaptureThreadState audioState;
        Status status = initializeAudioClientForMicrophone(config, false, &audioState);
        if (!status.ok()) {
            MFShutdown();
            return status;
        }

        outProbe->selectedAudioDeviceName =
            !outProbe->selectedAudioDeviceName.empty() ? outProbe->selectedAudioDeviceName : config.audioDeviceId;
        outProbe->negotiatedSampleRate =
            audioState.formatInfo.sampleRate > 0 ? audioState.formatInfo.sampleRate : config.sampleRate;
        outProbe->negotiatedChannels =
            audioState.formatInfo.channels > 0 ? audioState.formatInfo.channels : config.channels;
    }

    MFShutdown();

    return Status::Ok();
#endif
}

LiveDeviceCaptureSource::LiveDeviceCaptureSource() = default;

LiveDeviceCaptureSource::~LiveDeviceCaptureSource() = default;

Status LiveDeviceCaptureSource::configureDeviceSource(const std::string& configPath) {
    if (configPath.empty()) {
        return Status::InvalidArgument("configPath is empty");
    }

    media::LiveDeviceCaptureConfig parsedConfig;
    Status status = loadLiveDeviceCaptureConfig(configPath, &parsedConfig);
    if (!status.ok()) {
        return status;
    }

    return configureDeviceSource(parsedConfig);
}

Status LiveDeviceCaptureSource::configureDeviceSource(const media::LiveDeviceCaptureConfig& config) {
    if (config.videoDeviceId.empty() && config.audioDeviceId.empty()) {
        return Status::InvalidArgument("live device source requires at least one enabled device");
    }
    if (!config.videoDeviceId.empty() && (config.width <= 0 || config.height <= 0 || config.fps <= 0)) {
        return Status::InvalidArgument("live device source requires positive width, height, and fps when video is enabled");
    }
    if (!config.audioDeviceId.empty() && (config.sampleRate <= 0 || config.channels <= 0)) {
        return Status::InvalidArgument("live device source requires positive sampleRate and channels when audio is enabled");
    }

    config_ = config;
    started_ = false;
    context_.reset();
    return Status::Ok();
}

Status LiveDeviceCaptureSource::start() {
    if (config_.videoDeviceId.empty() && config_.audioDeviceId.empty()) {
        return Status::InvalidArgument("live device source is not fully configured");
    }
    if (!config_.videoDeviceId.empty() && (config_.width <= 0 || config_.height <= 0 || config_.fps <= 0)) {
        return Status::InvalidArgument("live device source video settings are incomplete");
    }
    if (!config_.audioDeviceId.empty() && (config_.sampleRate <= 0 || config_.channels <= 0)) {
        return Status::InvalidArgument("live device source audio settings are incomplete");
    }

#if !defined(_WIN32)
    return Status::NotImplemented("Media Foundation / WASAPI live capture is only implemented on Windows");
#else
    context_ = std::make_unique<LiveDeviceContext>();
    context_->videoInitDone = config_.videoDeviceId.empty();
    context_->videoThreadExited = config_.videoDeviceId.empty();
    context_->audioInitDone = config_.audioDeviceId.empty();
    context_->audioThreadExited = config_.audioDeviceId.empty();

    media::LiveDeviceCaptureConfig threadConfig = config_;
    if (!config_.videoDeviceId.empty()) {
        context_->videoThread = std::thread(videoCaptureThreadMain, threadConfig, context_.get());
    }
    if (!config_.audioDeviceId.empty()) {
        context_->audioThread = std::thread(audioCaptureThreadMain, threadConfig, context_.get());
    }

    std::unique_lock<std::mutex> lock(context_->mutex);
    context_->condition.wait(lock, [&]() {
        return context_->videoInitDone && context_->audioInitDone;
    });

    const Status videoInitStatus = context_->videoInitStatus;
    const Status audioInitStatus = context_->audioInitStatus;
    const int negotiatedVideoWidth = context_->negotiatedVideoWidth;
    const int negotiatedVideoHeight = context_->negotiatedVideoHeight;
    const int negotiatedVideoFps = context_->negotiatedVideoFps;
    const int negotiatedSampleRate = context_->negotiatedSampleRate;
    const int negotiatedChannels = context_->negotiatedChannels;
    const std::string selectedVideoDeviceName = context_->selectedVideoDeviceName;
    const std::string selectedAudioDeviceName = context_->selectedAudioDeviceName;
    lock.unlock();

    if (!videoInitStatus.ok()) {
        stop();
        return videoInitStatus;
    }
    if (!audioInitStatus.ok()) {
        stop();
        return audioInitStatus;
    }

    if (!config_.videoDeviceId.empty()) {
        config_.width = negotiatedVideoWidth > 0 ? negotiatedVideoWidth : config_.width;
        config_.height = negotiatedVideoHeight > 0 ? negotiatedVideoHeight : config_.height;
        config_.fps = negotiatedVideoFps > 0 ? negotiatedVideoFps : config_.fps;
        config_.resolvedVideoDeviceName =
            !selectedVideoDeviceName.empty() ? selectedVideoDeviceName : config_.videoDeviceId;
    } else {
        config_.resolvedVideoDeviceName.clear();
    }
    if (!config_.audioDeviceId.empty()) {
        config_.sampleRate = negotiatedSampleRate > 0 ? negotiatedSampleRate : config_.sampleRate;
        config_.channels = negotiatedChannels > 0 ? negotiatedChannels : config_.channels;
        config_.resolvedAudioDeviceName =
            !selectedAudioDeviceName.empty() ? selectedAudioDeviceName : config_.audioDeviceId;
    } else {
        config_.resolvedAudioDeviceName.clear();
    }

    started_ = true;
    return Status::Ok();
#endif
}

Status LiveDeviceCaptureSource::readNext(CaptureEvent* outEvent) {
    if (outEvent == nullptr) {
        return Status::InvalidArgument("outEvent is null");
    }
    if (!started_ || context_ == nullptr) {
        return Status::InvalidArgument("live device source is not started");
    }

    std::unique_lock<std::mutex> lock(context_->mutex);
    context_->condition.wait(lock, [&]() {
        return !context_->events.empty() || !context_->runtimeFailure.ok() ||
               (context_->videoThreadExited && context_->audioThreadExited);
    });

    if (!context_->events.empty()) {
        *outEvent = std::move(context_->events.front());
        context_->events.pop();
        return Status::Ok();
    }

    if (!context_->runtimeFailure.ok()) {
        return context_->runtimeFailure;
    }

    return Status::InternalError("live device capture ended unexpectedly");
}

void LiveDeviceCaptureSource::stop() noexcept {
    if (context_ != nullptr) {
        context_->stopRequested.store(true);
        {
            std::lock_guard<std::mutex> lock(context_->mutex);
            context_->condition.notify_all();
        }
        if (context_->videoThread.joinable()) {
            context_->videoThread.join();
        }
        if (context_->audioThread.joinable()) {
            context_->audioThread.join();
        }
    }

    context_.reset();
    started_ = false;
}

int LiveDeviceCaptureSource::pendingEventCount() const noexcept {
    if (context_ == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(context_->mutex);
    return static_cast<int>(context_->events.size());
}

}  // namespace live_studio::capture
