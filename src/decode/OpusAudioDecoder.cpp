#include "decode/OpusAudioDecoder.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace live_studio::decode {
namespace {

std::string ffmpegErrorString(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

Status configureResampler(SwrContext** inOutResampler,
                          int inputSampleRate,
                          int inputChannels,
                          AVSampleFormat inputFormat,
                          int outputSampleRate,
                          int outputChannels) {
    if (inOutResampler == nullptr) {
        return Status::InvalidArgument("Opus decoder resampler handle is null");
    }
    if (inputSampleRate <= 0 || inputChannels <= 0 || outputSampleRate <= 0 || outputChannels <= 0) {
        return Status::InvalidArgument("Opus decoder resampler requires positive rates and channels");
    }

    swr_free(inOutResampler);

    AVChannelLayout inputLayout;
    av_channel_layout_default(&inputLayout, inputChannels);
    AVChannelLayout outputLayout;
    av_channel_layout_default(&outputLayout, outputChannels);

    const int allocResult = swr_alloc_set_opts2(inOutResampler,
                                                &outputLayout,
                                                AV_SAMPLE_FMT_S16,
                                                outputSampleRate,
                                                &inputLayout,
                                                inputFormat,
                                                inputSampleRate,
                                                0,
                                                nullptr);
    av_channel_layout_uninit(&inputLayout);
    av_channel_layout_uninit(&outputLayout);
    if (allocResult < 0 || *inOutResampler == nullptr) {
        return Status::InternalError("swr_alloc_set_opts2 failed for Opus decoder: " +
                                     ffmpegErrorString(allocResult));
    }

    const int initResult = swr_init(*inOutResampler);
    if (initResult < 0) {
        return Status::InternalError("swr_init failed for Opus decoder: " + ffmpegErrorString(initResult));
    }

    return Status::Ok();
}

Status convertDecodedFrameToS16(SwrContext* resampler,
                                AVFrame* frame,
                                int frameIndex,
                                std::int64_t ptsSamples,
                                media::AudioFrame* outFrame) {
    if (resampler == nullptr || frame == nullptr || outFrame == nullptr) {
        return Status::InvalidArgument("Opus decoder conversion arguments are null");
    }
    if (frame->sample_rate <= 0) {
        return Status::InternalError("decoded Opus frame sample_rate must be positive");
    }
    if (frame->ch_layout.nb_channels <= 0) {
        return Status::InternalError("decoded Opus frame channels must be positive");
    }

    const int outputSampleCount =
        av_rescale_rnd(swr_get_delay(resampler, frame->sample_rate) + frame->nb_samples,
                       frame->sample_rate,
                       frame->sample_rate,
                       AV_ROUND_UP);
    if (outputSampleCount <= 0) {
        return Status::InternalError("Opus decoder resampler returned no output capacity");
    }

    std::uint8_t** convertedData = nullptr;
    int convertedLineSize = 0;
    const int allocResult = av_samples_alloc_array_and_samples(&convertedData,
                                                               &convertedLineSize,
                                                               frame->ch_layout.nb_channels,
                                                               outputSampleCount,
                                                               AV_SAMPLE_FMT_S16,
                                                               0);
    if (allocResult < 0) {
        return Status::InternalError("av_samples_alloc_array_and_samples failed for Opus decoder: " +
                                     ffmpegErrorString(allocResult));
    }

    const int convertedSamples = swr_convert(resampler,
                                             convertedData,
                                             outputSampleCount,
                                             const_cast<const std::uint8_t**>(frame->extended_data),
                                             frame->nb_samples);
    if (convertedSamples < 0) {
        if (convertedData != nullptr) {
            av_freep(&convertedData[0]);
        }
        av_freep(&convertedData);
        return Status::InternalError("swr_convert failed for Opus decoder: " +
                                     ffmpegErrorString(convertedSamples));
    }

    outFrame->sourceKind = "rtc-opus";
    outFrame->sourceLabel = "rtc-opus-decoded";
    outFrame->frameIndex = frameIndex;
    outFrame->sampleRate = frame->sample_rate;
    outFrame->channels = frame->ch_layout.nb_channels;
    outFrame->samplesPerChannel = convertedSamples;
    outFrame->ptsMs = (ptsSamples * 1000) / frame->sample_rate;
    outFrame->captureWallClockNs = 0;
    outFrame->hasSamplePayload = true;
    outFrame->sampleFormat = media::AudioSampleFormat::s16_interleaved;
    outFrame->samplePayload.assign(convertedData[0],
                                   convertedData[0] + static_cast<std::size_t>(convertedSamples) *
                                       static_cast<std::size_t>(frame->ch_layout.nb_channels) *
                                       sizeof(std::int16_t));

    if (convertedData != nullptr) {
        av_freep(&convertedData[0]);
    }
    av_freep(&convertedData);
    return Status::Ok();
}

}  // namespace

struct OpusAudioDecoder::DecoderContext {
    const AVCodec* codec = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* resampler = nullptr;
    AVSampleFormat resamplerInputFormat = AV_SAMPLE_FMT_NONE;
    int resamplerInputSampleRate = 0;
    int resamplerInputChannels = 0;
    int nextFrameIndex = 0;
    std::int64_t nextPtsSamples = 0;

    ~DecoderContext() {
        swr_free(&resampler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);
    }
};

OpusAudioDecoder::OpusAudioDecoder() = default;

OpusAudioDecoder::~OpusAudioDecoder() = default;

Status OpusAudioDecoder::configure(int expectedSampleRate, int expectedChannels) {
    if (expectedSampleRate <= 0 || expectedChannels <= 0) {
        return Status::InvalidArgument("Opus decoder parameters must be positive");
    }

    av_log_set_level(AV_LOG_ERROR);

    std::unique_ptr<DecoderContext> nextContext = std::make_unique<DecoderContext>();
    nextContext->codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (nextContext->codec == nullptr) {
        return Status::InternalError("FFmpeg could not find an Opus decoder");
    }

    nextContext->codecContext = avcodec_alloc_context3(nextContext->codec);
    if (nextContext->codecContext == nullptr) {
        return Status::InternalError("avcodec_alloc_context3 failed for Opus decoder");
    }

    nextContext->codecContext->codec_type = AVMEDIA_TYPE_AUDIO;
    nextContext->codecContext->codec_id = AV_CODEC_ID_OPUS;
    nextContext->codecContext->sample_rate = expectedSampleRate;
    nextContext->codecContext->pkt_timebase = AVRational{1, expectedSampleRate};
    nextContext->codecContext->thread_count = 1;
    av_channel_layout_default(&nextContext->codecContext->ch_layout, expectedChannels);

    const int openResult = avcodec_open2(nextContext->codecContext, nextContext->codec, nullptr);
    if (openResult < 0) {
        return Status::InternalError("avcodec_open2(Opus decoder) failed: " + ffmpegErrorString(openResult));
    }

    nextContext->packet = av_packet_alloc();
    nextContext->frame = av_frame_alloc();
    if (nextContext->packet == nullptr || nextContext->frame == nullptr) {
        return Status::InternalError("failed to allocate Opus decoder packet/frame");
    }

    context_ = std::move(nextContext);
    expectedSampleRate_ = expectedSampleRate;
    expectedChannels_ = expectedChannels;
    configured_ = true;
    return Status::Ok();
}

Status OpusAudioDecoder::drainFrames(std::int64_t ptsSamplesHint, std::vector<media::AudioFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr) {
        return Status::InvalidArgument("Opus decoder is not configured");
    }

    while (true) {
        av_frame_unref(context_->frame);
        const int receiveResult = avcodec_receive_frame(context_->codecContext, context_->frame);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            return Status::Ok();
        }
        if (receiveResult < 0) {
            return Status::InternalError("avcodec_receive_frame failed for Opus decoder: " +
                                         ffmpegErrorString(receiveResult));
        }

        const int actualSampleRate =
            context_->frame->sample_rate > 0 ? context_->frame->sample_rate : expectedSampleRate_;
        const int actualChannels =
            context_->frame->ch_layout.nb_channels > 0 ? context_->frame->ch_layout.nb_channels : expectedChannels_;
        if (actualSampleRate <= 0 || actualChannels <= 0) {
            return Status::InternalError("decoded Opus frame reported invalid sample rate or channels");
        }

        const auto actualInputFormat = static_cast<AVSampleFormat>(context_->frame->format);
        if (context_->resampler == nullptr || context_->resamplerInputFormat != actualInputFormat ||
            context_->resamplerInputSampleRate != actualSampleRate ||
            context_->resamplerInputChannels != actualChannels) {
            Status resamplerStatus = configureResampler(&context_->resampler,
                                                       actualSampleRate,
                                                       actualChannels,
                                                       actualInputFormat,
                                                       actualSampleRate,
                                                       actualChannels);
            if (!resamplerStatus.ok()) {
                return resamplerStatus;
            }

            context_->resamplerInputFormat = actualInputFormat;
            context_->resamplerInputSampleRate = actualSampleRate;
            context_->resamplerInputChannels = actualChannels;
        }

        const std::int64_t framePtsSamples =
            context_->frame->pts != AV_NOPTS_VALUE ? context_->frame->pts : ptsSamplesHint;

        media::AudioFrame decodedFrame;
        Status convertStatus =
            convertDecodedFrameToS16(context_->resampler, context_->frame, context_->nextFrameIndex, framePtsSamples, &decodedFrame);
        if (!convertStatus.ok()) {
            return convertStatus;
        }

        outFrames->push_back(std::move(decodedFrame));
        context_->nextPtsSamples = framePtsSamples + context_->frame->nb_samples;
        ++context_->nextFrameIndex;
    }
}

Status OpusAudioDecoder::decodePacket(const std::uint8_t* data,
                                      std::size_t size,
                                      std::int64_t ptsSamplesHint,
                                      std::vector<media::AudioFrame>* outFrames) {
    if (data == nullptr || size == 0) {
        return Status::InvalidArgument("Opus decoder packet is empty");
    }
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->packet == nullptr || context_->codecContext == nullptr) {
        return Status::InvalidArgument("Opus decoder is not configured");
    }

    outFrames->clear();

    av_packet_unref(context_->packet);
    const int packetResult = av_new_packet(context_->packet, static_cast<int>(size));
    if (packetResult < 0) {
        return Status::InternalError("av_new_packet failed for Opus decoder: " + ffmpegErrorString(packetResult));
    }
    std::memcpy(context_->packet->data, data, size);
    context_->packet->pts = ptsSamplesHint;
    context_->packet->dts = ptsSamplesHint;

    const int sendResult = avcodec_send_packet(context_->codecContext, context_->packet);
    av_packet_unref(context_->packet);
    if (sendResult < 0) {
        return Status::InternalError("avcodec_send_packet failed for Opus decoder: " + ffmpegErrorString(sendResult));
    }

    return drainFrames(ptsSamplesHint, outFrames);
}

Status OpusAudioDecoder::flush(std::vector<media::AudioFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr) {
        return Status::InvalidArgument("Opus decoder is not configured");
    }

    outFrames->clear();

    const int flushResult = avcodec_send_packet(context_->codecContext, nullptr);
    if (flushResult < 0 && flushResult != AVERROR_EOF) {
        return Status::InternalError("avcodec_send_packet(null) failed for Opus decoder: " +
                                     ffmpegErrorString(flushResult));
    }

    return drainFrames(context_->nextPtsSamples, outFrames);
}

}  // namespace live_studio::decode
