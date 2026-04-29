#include "encode/OpusAudioEncoder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace live_studio::encode {
namespace {

constexpr int kDefaultOpusSampleRate = 48000;
constexpr int kDefaultOpusFrameDurationMs = 20;

std::string ffmpegErrorString(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

bool codecSupportsSampleRate(const AVCodec* codec, int sampleRate) {
    if (codec == nullptr || sampleRate <= 0) {
        return false;
    }
    if (codec->supported_samplerates == nullptr) {
        return true;
    }

    for (const int* supported = codec->supported_samplerates; *supported != 0; ++supported) {
        if (*supported == sampleRate) {
            return true;
        }
    }
    return false;
}

AVSampleFormat pickEncoderSampleFormat(const AVCodec* codec) {
    if (codec == nullptr || codec->sample_fmts == nullptr) {
        return AV_SAMPLE_FMT_FLTP;
    }

    for (const AVSampleFormat* sampleFormat = codec->sample_fmts; *sampleFormat != AV_SAMPLE_FMT_NONE;
         ++sampleFormat) {
        if (*sampleFormat == AV_SAMPLE_FMT_FLTP) {
            return *sampleFormat;
        }
    }
    return codec->sample_fmts[0];
}

int pickEncoderFrameSamples(const AVCodecContext* codecContext) {
    if (codecContext != nullptr && codecContext->frame_size > 0) {
        return codecContext->frame_size;
    }
    return (kDefaultOpusSampleRate * kDefaultOpusFrameDurationMs) / 1000;
}

Status ensureAudioFrameWritable(AVCodecContext* codecContext, AVFrame* frame, int nbSamples) {
    if (codecContext == nullptr || frame == nullptr) {
        return Status::InvalidArgument("Opus encoder frame arguments are null");
    }
    if (nbSamples <= 0) {
        return Status::InvalidArgument("Opus encoder frame requires positive sample count");
    }

    if (frame->nb_samples != nbSamples) {
        av_frame_unref(frame);
        frame->format = codecContext->sample_fmt;
        frame->nb_samples = nbSamples;
        frame->sample_rate = codecContext->sample_rate;
        const int channelLayoutResult = av_channel_layout_copy(&frame->ch_layout, &codecContext->ch_layout);
        if (channelLayoutResult < 0) {
            return Status::InternalError("av_channel_layout_copy failed for Opus frame: " +
                                         ffmpegErrorString(channelLayoutResult));
        }

        const int allocResult = av_frame_get_buffer(frame, 0);
        if (allocResult < 0) {
            return Status::InternalError("av_frame_get_buffer failed for Opus frame: " +
                                         ffmpegErrorString(allocResult));
        }
    }

    const int writableResult = av_frame_make_writable(frame);
    if (writableResult < 0) {
        return Status::InternalError("av_frame_make_writable failed for Opus frame: " +
                                     ffmpegErrorString(writableResult));
    }

    return Status::Ok();
}

Status copyInterleavedS16ToEncoderFifo(const media::AudioFrame& frame,
                                       int inputSampleRate,
                                       AVCodecContext* codecContext,
                                       SwrContext* resampler,
                                       AVAudioFifo* fifo) {
    if (codecContext == nullptr || resampler == nullptr || fifo == nullptr) {
        return Status::InvalidArgument("Opus encoder conversion state is null");
    }
    if (!frame.hasSamplePayload || frame.sampleFormat != media::AudioSampleFormat::s16_interleaved) {
        return Status::InvalidArgument("audio frame does not carry S16 interleaved PCM");
    }
    if (frame.samplesPerChannel <= 0) {
        return Status::InvalidArgument("audio frame samplesPerChannel must be positive");
    }

    const std::uint8_t* inputData[1] = {frame.samplePayload.data()};
    const int outputSampleCount =
        av_rescale_rnd(swr_get_delay(resampler, inputSampleRate) + frame.samplesPerChannel,
                       codecContext->sample_rate,
                       inputSampleRate,
                       AV_ROUND_UP);
    if (outputSampleCount <= 0) {
        return Status::InternalError("Opus resampler returned no output capacity");
    }

    std::uint8_t** convertedData = nullptr;
    int convertedLineSize = 0;
    const int allocResult = av_samples_alloc_array_and_samples(&convertedData,
                                                               &convertedLineSize,
                                                               codecContext->ch_layout.nb_channels,
                                                               outputSampleCount,
                                                               codecContext->sample_fmt,
                                                               0);
    if (allocResult < 0) {
        return Status::InternalError("av_samples_alloc_array_and_samples failed for Opus input: " +
                                     ffmpegErrorString(allocResult));
    }

    const int convertedSamples = swr_convert(resampler,
                                             convertedData,
                                             outputSampleCount,
                                             inputData,
                                             frame.samplesPerChannel);
    if (convertedSamples < 0) {
        if (convertedData != nullptr) {
            av_freep(&convertedData[0]);
        }
        av_freep(&convertedData);
        return Status::InternalError("swr_convert failed for Opus input: " + ffmpegErrorString(convertedSamples));
    }

    const int fifoGrowResult = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + convertedSamples);
    if (fifoGrowResult < 0) {
        if (convertedData != nullptr) {
            av_freep(&convertedData[0]);
        }
        av_freep(&convertedData);
        return Status::InternalError("av_audio_fifo_realloc failed for Opus input: " +
                                     ffmpegErrorString(fifoGrowResult));
    }

    const int writtenSamples = av_audio_fifo_write(fifo, reinterpret_cast<void**>(convertedData), convertedSamples);
    if (writtenSamples != convertedSamples) {
        if (convertedData != nullptr) {
            av_freep(&convertedData[0]);
        }
        av_freep(&convertedData);
        return Status::InternalError("av_audio_fifo_write failed to queue all Opus input samples");
    }

    if (convertedData != nullptr) {
        av_freep(&convertedData[0]);
    }
    av_freep(&convertedData);
    return Status::Ok();
}

}  // namespace

struct OpusAudioEncoder::EncoderContext {
    const AVCodec* codec = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    int encoderFrameSamples = 0;
    std::int64_t nextPtsSamples = 0;
    bool timestampBaseInitialized = false;
    std::int64_t timestampBaseSamples = 0;

    ~EncoderContext() {
        av_audio_fifo_free(fifo);
        swr_free(&resampler);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codecContext);
    }
};

OpusAudioEncoder::OpusAudioEncoder() = default;

OpusAudioEncoder::~OpusAudioEncoder() = default;

Status OpusAudioEncoder::configure(int inputSampleRate, int inputChannels, int bitrate) {
    if (inputSampleRate <= 0 || inputChannels <= 0 || bitrate <= 0) {
        return Status::InvalidArgument("Opus encoder parameters must be positive");
    }
    if (inputChannels > 2) {
        return Status::InvalidArgument("Opus encoder currently supports mono or stereo input only");
    }

    av_log_set_level(AV_LOG_ERROR);

    std::unique_ptr<EncoderContext> nextContext = std::make_unique<EncoderContext>();
    nextContext->codec = avcodec_find_encoder_by_name("libopus");
    if (nextContext->codec == nullptr) {
        nextContext->codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    }
    if (nextContext->codec == nullptr) {
        return Status::InternalError("FFmpeg could not find an Opus encoder");
    }
    if (!codecSupportsSampleRate(nextContext->codec, kDefaultOpusSampleRate)) {
        return Status::InvalidArgument("Opus encoder does not support sample_rate=48000");
    }

    nextContext->codecContext = avcodec_alloc_context3(nextContext->codec);
    if (nextContext->codecContext == nullptr) {
        return Status::InternalError("avcodec_alloc_context3 failed for Opus encoder");
    }

    nextContext->codecContext->codec_type = AVMEDIA_TYPE_AUDIO;
    nextContext->codecContext->codec_id = AV_CODEC_ID_OPUS;
    nextContext->codecContext->bit_rate = bitrate;
    nextContext->codecContext->sample_rate = kDefaultOpusSampleRate;
    nextContext->codecContext->time_base = AVRational{1, kDefaultOpusSampleRate};
    nextContext->codecContext->sample_fmt = pickEncoderSampleFormat(nextContext->codec);
    nextContext->codecContext->thread_count = 1;
    nextContext->codecContext->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    av_channel_layout_default(&nextContext->codecContext->ch_layout, inputChannels);
    if (!av_channel_layout_check(&nextContext->codecContext->ch_layout)) {
        return Status::InternalError("av_channel_layout_default produced an invalid channel layout for Opus encoder");
    }

    const int openResult = avcodec_open2(nextContext->codecContext, nextContext->codec, nullptr);
    if (openResult < 0) {
        return Status::InternalError("avcodec_open2(Opus) failed: " + ffmpegErrorString(openResult));
    }

    nextContext->frame = av_frame_alloc();
    if (nextContext->frame == nullptr) {
        return Status::InternalError("av_frame_alloc failed for Opus encoder frame");
    }

    nextContext->packet = av_packet_alloc();
    if (nextContext->packet == nullptr) {
        return Status::InternalError("av_packet_alloc failed for Opus encoder packet");
    }

    nextContext->encoderFrameSamples = pickEncoderFrameSamples(nextContext->codecContext);
    nextContext->fifo = av_audio_fifo_alloc(nextContext->codecContext->sample_fmt,
                                            nextContext->codecContext->ch_layout.nb_channels,
                                            nextContext->encoderFrameSamples);
    if (nextContext->fifo == nullptr) {
        return Status::InternalError("av_audio_fifo_alloc failed for Opus encoder");
    }

    AVChannelLayout inputLayout;
    av_channel_layout_default(&inputLayout, inputChannels);
    const int resamplerResult = swr_alloc_set_opts2(&nextContext->resampler,
                                                    &nextContext->codecContext->ch_layout,
                                                    nextContext->codecContext->sample_fmt,
                                                    nextContext->codecContext->sample_rate,
                                                    &inputLayout,
                                                    AV_SAMPLE_FMT_S16,
                                                    inputSampleRate,
                                                    0,
                                                    nullptr);
    av_channel_layout_uninit(&inputLayout);
    if (resamplerResult < 0 || nextContext->resampler == nullptr) {
        return Status::InternalError("swr_alloc_set_opts2 failed for Opus encoder: " +
                                     ffmpegErrorString(resamplerResult));
    }

    const int swrInitResult = swr_init(nextContext->resampler);
    if (swrInitResult < 0) {
        return Status::InternalError("swr_init failed for Opus encoder: " + ffmpegErrorString(swrInitResult));
    }

    context_ = std::move(nextContext);
    inputSampleRate_ = inputSampleRate;
    outputSampleRate_ = kDefaultOpusSampleRate;
    channels_ = inputChannels;
    bitrate_ = bitrate;
    configured_ = true;
    return Status::Ok();
}

Status OpusAudioEncoder::drainPackets(int frameIndexHint,
                                      int samplesPerChannelHint,
                                      std::vector<EncodedOpusFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->packet == nullptr) {
        return Status::InvalidArgument("Opus encoder is not configured");
    }

    while (true) {
        av_packet_unref(context_->packet);
        const int receiveResult = avcodec_receive_packet(context_->codecContext, context_->packet);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            av_packet_unref(context_->packet);
            return Status::Ok();
        }
        if (receiveResult < 0) {
            av_packet_unref(context_->packet);
            return Status::InternalError("avcodec_receive_packet failed for Opus encoder: " +
                                         ffmpegErrorString(receiveResult));
        }

        EncodedOpusFrame encodedFrame;
        encodedFrame.frameIndex = frameIndexHint;
        encodedFrame.sampleRate = outputSampleRate_;
        encodedFrame.channels = channels_;
        encodedFrame.samplesPerChannel =
            context_->packet->duration > 0 ? static_cast<int>(context_->packet->duration) : samplesPerChannelHint;
        const std::int64_t packetPtsSamples = context_->packet->pts == AV_NOPTS_VALUE ? context_->nextPtsSamples
                                                                                       : context_->packet->pts;
        const std::int64_t packetDtsSamples =
            context_->packet->dts == AV_NOPTS_VALUE ? packetPtsSamples : context_->packet->dts;
        if (!context_->timestampBaseInitialized) {
            context_->timestampBaseInitialized = true;
            context_->timestampBaseSamples = std::min<std::int64_t>(0, std::min(packetPtsSamples, packetDtsSamples));
        }

        encodedFrame.ptsSamples = std::max<std::int64_t>(0, packetPtsSamples - context_->timestampBaseSamples);
        encodedFrame.dtsSamples = std::max<std::int64_t>(0, packetDtsSamples - context_->timestampBaseSamples);
        encodedFrame.payloadSummary = "opus(" + std::to_string(context_->packet->size) + "B," +
                                      std::to_string(encodedFrame.samplesPerChannel) + "spc)";
        encodedFrame.payload.assign(context_->packet->data, context_->packet->data + context_->packet->size);
        outFrames->push_back(std::move(encodedFrame));
        av_packet_unref(context_->packet);
    }
}

Status OpusAudioEncoder::encodeReadyFrames(std::vector<EncodedOpusFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr ||
        context_->fifo == nullptr) {
        return Status::InvalidArgument("Opus encoder is not configured");
    }

    while (av_audio_fifo_size(context_->fifo) >= context_->encoderFrameSamples) {
        Status status = ensureAudioFrameWritable(context_->codecContext, context_->frame, context_->encoderFrameSamples);
        if (!status.ok()) {
            return status;
        }

        const int readSamples = av_audio_fifo_read(context_->fifo,
                                                   reinterpret_cast<void**>(context_->frame->data),
                                                   context_->encoderFrameSamples);
        if (readSamples != context_->encoderFrameSamples) {
            return Status::InternalError("av_audio_fifo_read did not return a full Opus frame");
        }

        const std::int64_t framePtsSamples = context_->nextPtsSamples;
        context_->frame->pts = framePtsSamples;
        context_->nextPtsSamples += context_->encoderFrameSamples;

        const int sendResult = avcodec_send_frame(context_->codecContext, context_->frame);
        if (sendResult < 0) {
            return Status::InternalError("avcodec_send_frame failed for Opus encoder: " +
                                         ffmpegErrorString(sendResult));
        }

        status = drainPackets(static_cast<int>(framePtsSamples / std::max(1, context_->encoderFrameSamples)),
                              context_->encoderFrameSamples,
                              outFrames);
        if (!status.ok()) {
            return status;
        }
    }

    return Status::Ok();
}

Status OpusAudioEncoder::encodeFrame(const media::AudioFrame& frame,
                                     std::vector<EncodedOpusFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->resampler == nullptr ||
        context_->fifo == nullptr) {
        return Status::InvalidArgument("Opus encoder is not configured");
    }
    if (frame.sampleRate != inputSampleRate_ || frame.channels != channels_) {
        return Status::InvalidArgument("audio frame does not match Opus encoder configuration");
    }

    outFrames->clear();

    Status status =
        copyInterleavedS16ToEncoderFifo(frame, inputSampleRate_, context_->codecContext, context_->resampler, context_->fifo);
    if (!status.ok()) {
        return status;
    }

    return encodeReadyFrames(outFrames);
}

Status OpusAudioEncoder::flush(std::vector<EncodedOpusFrame>* outFrames) {
    if (outFrames == nullptr) {
        return Status::InvalidArgument("outFrames is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr ||
        context_->fifo == nullptr) {
        return Status::InvalidArgument("Opus encoder is not configured");
    }

    outFrames->clear();

    const int remainingSamples = av_audio_fifo_size(context_->fifo);
    if (remainingSamples > 0) {
        const bool encoderHasFixedFrameSize = context_->codecContext->frame_size > 0;
        const int samplesToSend = encoderHasFixedFrameSize ? context_->encoderFrameSamples : remainingSamples;
        Status status = ensureAudioFrameWritable(context_->codecContext, context_->frame, samplesToSend);
        if (!status.ok()) {
            return status;
        }

        const int planeCount = av_sample_fmt_is_planar(context_->codecContext->sample_fmt)
                                   ? context_->codecContext->ch_layout.nb_channels
                                   : 1;
        for (int plane = 0; plane < planeCount; ++plane) {
            std::memset(context_->frame->data[plane], 0, static_cast<std::size_t>(context_->frame->linesize[plane]));
        }

        const int readSamples =
            av_audio_fifo_read(context_->fifo, reinterpret_cast<void**>(context_->frame->data), remainingSamples);
        if (readSamples != remainingSamples) {
            return Status::InternalError("av_audio_fifo_read failed while flushing Opus encoder");
        }

        context_->frame->pts = context_->nextPtsSamples;
        context_->nextPtsSamples += samplesToSend;

        const int sendResult = avcodec_send_frame(context_->codecContext, context_->frame);
        if (sendResult < 0) {
            return Status::InternalError("avcodec_send_frame failed while flushing Opus encoder: " +
                                         ffmpegErrorString(sendResult));
        }

        status = drainPackets(-1, samplesToSend, outFrames);
        if (!status.ok()) {
            return status;
        }
    }

    const int flushResult = avcodec_send_frame(context_->codecContext, nullptr);
    if (flushResult < 0 && flushResult != AVERROR_EOF) {
        return Status::InternalError("avcodec_send_frame(null) failed while draining Opus encoder: " +
                                     ffmpegErrorString(flushResult));
    }

    return drainPackets(-1, context_->encoderFrameSamples, outFrames);
}

}  // namespace live_studio::encode
