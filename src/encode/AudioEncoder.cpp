#include "encode/AudioEncoder.hpp"

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
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace live_studio::encode {
namespace {

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

int resolveAacObjectType(const AVCodecContext* codecContext) {
    if (codecContext == nullptr) {
        return 2;
    }

    switch (codecContext->profile) {
    case FF_PROFILE_AAC_MAIN:
        return 1;
    case FF_PROFILE_AAC_HE:
    case FF_PROFILE_AAC_HE_V2:
        return 5;
    case FF_PROFILE_AAC_LTP:
        return 4;
    case FF_PROFILE_AAC_LOW:
    default:
        return 2;
    }
}

std::string describeAacProfile(int objectType) {
    switch (objectType) {
    case 1:
        return "aac-main";
    case 2:
        return "aac-lc";
    case 4:
        return "aac-ltp";
    case 5:
        return "aac-he";
    default:
        return "aac-object-" + std::to_string(objectType);
    }
}

std::string describeAudioSpecificConfig(const std::vector<std::uint8_t>& bytes,
                                        int sampleRate,
                                        int channels,
                                        const std::string& profile) {
    return "asc(" + std::to_string(bytes.size()) + "B," + profile + "," + std::to_string(sampleRate) + "Hz," +
           std::to_string(channels) + "ch)";
}

Status ensureAudioFrameWritable(AVCodecContext* codecContext, AVFrame* frame, int nbSamples) {
    if (codecContext == nullptr || frame == nullptr) {
        return Status::InvalidArgument("audio encoder frame arguments are null");
    }
    if (nbSamples <= 0) {
        return Status::InvalidArgument("audio encoder frame requires positive sample count");
    }

    if (frame->nb_samples != nbSamples) {
        av_frame_unref(frame);
        frame->format = codecContext->sample_fmt;
        frame->nb_samples = nbSamples;
        frame->sample_rate = codecContext->sample_rate;
        const int bufferResult = av_channel_layout_copy(&frame->ch_layout, &codecContext->ch_layout);
        if (bufferResult < 0) {
            return Status::InternalError("av_channel_layout_copy failed for AAC frame: " +
                                         ffmpegErrorString(bufferResult));
        }

        const int allocResult = av_frame_get_buffer(frame, 0);
        if (allocResult < 0) {
            return Status::InternalError("av_frame_get_buffer failed for AAC frame: " +
                                         ffmpegErrorString(allocResult));
        }
    }

    const int writableResult = av_frame_make_writable(frame);
    if (writableResult < 0) {
        return Status::InternalError("av_frame_make_writable failed for AAC frame: " +
                                     ffmpegErrorString(writableResult));
    }

    return Status::Ok();
}

Status copyInterleavedS16ToEncoderFifo(const media::AudioFrame& frame,
                                       AVCodecContext* codecContext,
                                       SwrContext* resampler,
                                       AVAudioFifo* fifo) {
    if (codecContext == nullptr || resampler == nullptr || fifo == nullptr) {
        return Status::InvalidArgument("audio encoder conversion state is null");
    }
    if (!frame.hasSamplePayload || frame.sampleFormat != media::AudioSampleFormat::s16_interleaved) {
        return Status::InvalidArgument("audio frame does not carry S16 interleaved PCM");
    }
    if (frame.samplesPerChannel <= 0) {
        return Status::InvalidArgument("audio frame samplesPerChannel must be positive");
    }

    const std::uint8_t* inputData[1] = {frame.samplePayload.data()};
    const int outputSampleCount =
        av_rescale_rnd(swr_get_delay(resampler, codecContext->sample_rate) + frame.samplesPerChannel,
                       codecContext->sample_rate,
                       codecContext->sample_rate,
                       AV_ROUND_UP);
    if (outputSampleCount <= 0) {
        return Status::InternalError("live audio resampler returned no output capacity");
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
        return Status::InternalError("av_samples_alloc_array_and_samples failed for AAC input: " +
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
        return Status::InternalError("swr_convert failed for AAC input: " + ffmpegErrorString(convertedSamples));
    }

    const int fifoGrowResult = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + convertedSamples);
    if (fifoGrowResult < 0) {
        if (convertedData != nullptr) {
            av_freep(&convertedData[0]);
        }
        av_freep(&convertedData);
        return Status::InternalError("av_audio_fifo_realloc failed for AAC input: " +
                                     ffmpegErrorString(fifoGrowResult));
    }

    const int writtenSamples = av_audio_fifo_write(fifo, reinterpret_cast<void**>(convertedData), convertedSamples);
    if (writtenSamples != convertedSamples) {
        if (convertedData != nullptr) {
            av_freep(&convertedData[0]);
        }
        av_freep(&convertedData);
        return Status::InternalError("av_audio_fifo_write failed to queue all AAC input samples");
    }

    if (convertedData != nullptr) {
        av_freep(&convertedData[0]);
    }
    av_freep(&convertedData);
    return Status::Ok();
}

}  // namespace

struct AudioEncoder::EncoderContext {
    const AVCodec* codec = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    media::AacDecoderConfig decoderConfig;
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

AudioEncoder::AudioEncoder() = default;

AudioEncoder::~AudioEncoder() = default;

Status AudioEncoder::configureAac(int sampleRate, int channels, int bitrate) {
    if (sampleRate <= 0 || channels <= 0 || bitrate <= 0) {
        return Status::InvalidArgument("AAC encoder parameters must be positive");
    }

    av_log_set_level(AV_LOG_ERROR);

    std::unique_ptr<EncoderContext> nextContext = std::make_unique<EncoderContext>();
    nextContext->codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (nextContext->codec == nullptr) {
        return Status::InternalError("FFmpeg could not find an AAC encoder");
    }
    if (!codecSupportsSampleRate(nextContext->codec, sampleRate)) {
        return Status::InvalidArgument("AAC encoder does not support sample_rate=" + std::to_string(sampleRate));
    }

    nextContext->codecContext = avcodec_alloc_context3(nextContext->codec);
    if (nextContext->codecContext == nullptr) {
        return Status::InternalError("avcodec_alloc_context3 failed for AAC encoder");
    }

    nextContext->codecContext->codec_type = AVMEDIA_TYPE_AUDIO;
    nextContext->codecContext->codec_id = AV_CODEC_ID_AAC;
    nextContext->codecContext->bit_rate = bitrate;
    nextContext->codecContext->sample_rate = sampleRate;
    nextContext->codecContext->time_base = AVRational{1, sampleRate};
    nextContext->codecContext->sample_fmt = pickEncoderSampleFormat(nextContext->codec);
    nextContext->codecContext->thread_count = 1;
    nextContext->codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_channel_layout_default(&nextContext->codecContext->ch_layout, channels);
    if (!av_channel_layout_check(&nextContext->codecContext->ch_layout)) {
        return Status::InternalError("av_channel_layout_default produced an invalid channel layout for AAC encoder");
    }

    AVDictionary* options = nullptr;
    av_dict_set(&options, "profile", "aac_low", 0);

    const int openResult = avcodec_open2(nextContext->codecContext, nextContext->codec, &options);
    av_dict_free(&options);
    if (openResult < 0) {
        return Status::InternalError("avcodec_open2(AAC) failed: " + ffmpegErrorString(openResult));
    }

    nextContext->frame = av_frame_alloc();
    if (nextContext->frame == nullptr) {
        return Status::InternalError("av_frame_alloc failed for AAC encoder frame");
    }

    nextContext->packet = av_packet_alloc();
    if (nextContext->packet == nullptr) {
        return Status::InternalError("av_packet_alloc failed for AAC encoder packet");
    }

    nextContext->fifo = av_audio_fifo_alloc(nextContext->codecContext->sample_fmt,
                                            nextContext->codecContext->ch_layout.nb_channels,
                                            nextContext->codecContext->frame_size > 0
                                                ? nextContext->codecContext->frame_size
                                                : 1024);
    if (nextContext->fifo == nullptr) {
        return Status::InternalError("av_audio_fifo_alloc failed for AAC encoder");
    }

    AVChannelLayout inputLayout;
    av_channel_layout_default(&inputLayout, channels);
    const int resamplerResult = swr_alloc_set_opts2(&nextContext->resampler,
                                                    &nextContext->codecContext->ch_layout,
                                                    nextContext->codecContext->sample_fmt,
                                                    nextContext->codecContext->sample_rate,
                                                    &inputLayout,
                                                    AV_SAMPLE_FMT_S16,
                                                    sampleRate,
                                                    0,
                                                    nullptr);
    av_channel_layout_uninit(&inputLayout);
    if (resamplerResult < 0 || nextContext->resampler == nullptr) {
        return Status::InternalError("swr_alloc_set_opts2 failed for AAC encoder: " +
                                     ffmpegErrorString(resamplerResult));
    }

    const int swrInitResult = swr_init(nextContext->resampler);
    if (swrInitResult < 0) {
        return Status::InternalError("swr_init failed for AAC encoder: " + ffmpegErrorString(swrInitResult));
    }

    if (nextContext->codecContext->extradata == nullptr || nextContext->codecContext->extradata_size <= 0) {
        return Status::InternalError("AAC encoder did not expose AudioSpecificConfig extradata");
    }

    nextContext->decoderConfig.sampleRate = nextContext->codecContext->sample_rate;
    nextContext->decoderConfig.channels = nextContext->codecContext->ch_layout.nb_channels;
    nextContext->decoderConfig.profile = describeAacProfile(resolveAacObjectType(nextContext->codecContext));
    nextContext->decoderConfig.audioSpecificConfigBytes.assign(
        nextContext->codecContext->extradata,
        nextContext->codecContext->extradata + nextContext->codecContext->extradata_size);
    nextContext->decoderConfig.configLabel = describeAudioSpecificConfig(
        nextContext->decoderConfig.audioSpecificConfigBytes,
        nextContext->decoderConfig.sampleRate,
        nextContext->decoderConfig.channels,
        nextContext->decoderConfig.profile);

    context_ = std::move(nextContext);
    sampleRate_ = sampleRate;
    channels_ = channels;
    bitrate_ = bitrate;
    configured_ = true;
    return Status::Ok();
}

Status AudioEncoder::buildSequenceHeader(media::AacDecoderConfig* outConfig) const {
    if (outConfig == nullptr) {
        return Status::InvalidArgument("outConfig is null");
    }
    if (!configured_ || context_ == nullptr) {
        return Status::InvalidArgument("audio encoder is not configured");
    }

    *outConfig = context_->decoderConfig;
    return Status::Ok();
}

Status AudioEncoder::drainPackets(int frameIndexHint,
                                  int samplesPerChannelHint,
                                  std::vector<media::EncodedAudioAccessUnit>* outUnits) {
    if (outUnits == nullptr) {
        return Status::InvalidArgument("outUnits is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->packet == nullptr) {
        return Status::InvalidArgument("audio encoder is not configured");
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
            return Status::InternalError("avcodec_receive_packet failed for AAC encoder: " +
                                         ffmpegErrorString(receiveResult));
        }

        media::EncodedAudioAccessUnit unit;
        unit.frameIndex = frameIndexHint;
        const std::int64_t packetPtsSamples = context_->packet->pts == AV_NOPTS_VALUE ? context_->nextPtsSamples
                                                                                       : context_->packet->pts;
        const std::int64_t packetDtsSamples =
            context_->packet->dts == AV_NOPTS_VALUE ? packetPtsSamples : context_->packet->dts;
        if (!context_->timestampBaseInitialized) {
            context_->timestampBaseInitialized = true;
            context_->timestampBaseSamples = std::min<std::int64_t>(0, std::min(packetPtsSamples, packetDtsSamples));
        }

        const std::int64_t normalizedPtsSamples =
            std::max<std::int64_t>(0, packetPtsSamples - context_->timestampBaseSamples);
        const std::int64_t normalizedDtsSamples =
            std::max<std::int64_t>(0, packetDtsSamples - context_->timestampBaseSamples);

        unit.ptsMs = av_rescale_q(normalizedPtsSamples, AVRational{1, sampleRate_}, AVRational{1, 1000});
        unit.dtsMs = av_rescale_q(normalizedDtsSamples, AVRational{1, sampleRate_}, AVRational{1, 1000});
        unit.payloadSummary =
            "aac-raw(" + std::to_string(context_->packet->size) + "B," + std::to_string(samplesPerChannelHint) + "spc)";
        unit.rawPayload.assign(context_->packet->data, context_->packet->data + context_->packet->size);
        outUnits->push_back(std::move(unit));
        av_packet_unref(context_->packet);
    }
}

Status AudioEncoder::encodeReadyFrames(std::vector<media::EncodedAudioAccessUnit>* outUnits) {
    if (outUnits == nullptr) {
        return Status::InvalidArgument("outUnits is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr ||
        context_->fifo == nullptr) {
        return Status::InvalidArgument("audio encoder is not configured");
    }

    const int encoderFrameSamples = context_->codecContext->frame_size > 0 ? context_->codecContext->frame_size : 1024;
    while (av_audio_fifo_size(context_->fifo) >= encoderFrameSamples) {
        Status status = ensureAudioFrameWritable(context_->codecContext, context_->frame, encoderFrameSamples);
        if (!status.ok()) {
            return status;
        }

        const int readSamples = av_audio_fifo_read(context_->fifo,
                                                   reinterpret_cast<void**>(context_->frame->data),
                                                   encoderFrameSamples);
        if (readSamples != encoderFrameSamples) {
            return Status::InternalError("av_audio_fifo_read did not return a full AAC frame");
        }

        const std::int64_t framePtsSamples = context_->nextPtsSamples;
        context_->frame->pts = framePtsSamples;
        context_->nextPtsSamples += encoderFrameSamples;

        const int sendResult = avcodec_send_frame(context_->codecContext, context_->frame);
        if (sendResult < 0) {
            return Status::InternalError("avcodec_send_frame failed for AAC encoder: " +
                                         ffmpegErrorString(sendResult));
        }

        status = drainPackets(static_cast<int>(framePtsSamples / encoderFrameSamples), encoderFrameSamples, outUnits);
        if (!status.ok()) {
            return status;
        }
    }

    return Status::Ok();
}

Status AudioEncoder::encodeFrame(const media::AudioFrame& frame,
                                 std::vector<media::EncodedAudioAccessUnit>* outUnits) {
    if (outUnits == nullptr) {
        return Status::InvalidArgument("outUnits is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->resampler == nullptr ||
        context_->fifo == nullptr) {
        return Status::InvalidArgument("audio encoder is not configured");
    }
    if (frame.sampleRate != sampleRate_ || frame.channels != channels_) {
        return Status::InvalidArgument("audio frame does not match AAC encoder configuration");
    }

    outUnits->clear();

    Status status = copyInterleavedS16ToEncoderFifo(frame, context_->codecContext, context_->resampler, context_->fifo);
    if (!status.ok()) {
        return status;
    }

    return encodeReadyFrames(outUnits);
}

Status AudioEncoder::flush(std::vector<media::EncodedAudioAccessUnit>* outUnits) {
    if (outUnits == nullptr) {
        return Status::InvalidArgument("outUnits is null");
    }
    if (!configured_ || context_ == nullptr || context_->codecContext == nullptr || context_->frame == nullptr ||
        context_->fifo == nullptr) {
        return Status::InvalidArgument("audio encoder is not configured");
    }

    outUnits->clear();

    const int encoderFrameSamples = context_->codecContext->frame_size > 0 ? context_->codecContext->frame_size : 1024;
    const int remainingSamples = av_audio_fifo_size(context_->fifo);
    if (remainingSamples > 0) {
        Status status = ensureAudioFrameWritable(context_->codecContext, context_->frame, encoderFrameSamples);
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
            return Status::InternalError("av_audio_fifo_read failed while flushing AAC encoder");
        }

        context_->frame->pts = context_->nextPtsSamples;
        context_->nextPtsSamples += encoderFrameSamples;

        const int sendResult = avcodec_send_frame(context_->codecContext, context_->frame);
        if (sendResult < 0) {
            return Status::InternalError("avcodec_send_frame failed while flushing AAC encoder: " +
                                         ffmpegErrorString(sendResult));
        }

        status = drainPackets(-1, encoderFrameSamples, outUnits);
        if (!status.ok()) {
            return status;
        }
    }

    const int flushResult = avcodec_send_frame(context_->codecContext, nullptr);
    if (flushResult < 0 && flushResult != AVERROR_EOF) {
        return Status::InternalError("avcodec_send_frame(null) failed while draining AAC encoder: " +
                                     ffmpegErrorString(flushResult));
    }

    return drainPackets(-1, encoderFrameSamples, outUnits);
}

}  // namespace live_studio::encode
