#include "util/audio_decoder.h"

#include <QDebug>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

struct FormatCtxDeleter {
    void operator()(AVFormatContext *ctx) const {
        if (ctx)
            avformat_close_input(&ctx);
    }
};

struct CodecCtxDeleter {
    void operator()(AVCodecContext *ctx) const {
        if (ctx)
            avcodec_free_context(&ctx);
    }
};

struct SwrCtxDeleter {
    void operator()(SwrContext *ctx) const {
        if (ctx)
            swr_free(&ctx);
    }
};

struct FrameDeleter {
    void operator()(AVFrame *f) const {
        if (f)
            av_frame_free(&f);
    }
};

struct PacketDeleter {
    void operator()(AVPacket *p) const {
        if (p)
            av_packet_free(&p);
    }
};

} // namespace

std::vector<float> AudioDecoder::decodeToFloat32(const QString &filePath) {
    return decodeToFloat32(filePath, -1, -1);
}

std::vector<float> AudioDecoder::decodeToFloat32(const QString &filePath, int64_t startMs, int64_t endMs) {
    m_cancelled = false;
    std::vector<float> output;

    // Open input
    AVFormatContext *fmtRaw = avformat_alloc_context();
    if (!fmtRaw)
        return output;
    fmtRaw->interrupt_callback.callback = [](void *flag) {
        return static_cast<std::atomic_bool *>(flag)->load() ? 1 : 0;
    };
    fmtRaw->interrupt_callback.opaque = &m_cancelled;
    QByteArray pathUtf8 = filePath.toUtf8();
    if (avformat_open_input(&fmtRaw, pathUtf8.constData(), nullptr, nullptr) < 0) {
        if (fmtRaw)
            avformat_free_context(fmtRaw);
        qWarning() << "[AudioDecoder] 无法打开文件:" << filePath;
        return output;
    }
    std::unique_ptr<AVFormatContext, FormatCtxDeleter> fmtCtx(fmtRaw);

    if (avformat_find_stream_info(fmtCtx.get(), nullptr) < 0) {
        qWarning() << "[AudioDecoder] 无法获取流信息";
        return output;
    }

    // Find audio stream
    int audioIdx = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = static_cast<int>(i);
            break;
        }
    }
    if (audioIdx < 0) {
        qWarning() << "[AudioDecoder] 未找到音频流";
        return output;
    }

    AVStream *audioStream = fmtCtx->streams[audioIdx];
    const AVCodec *codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
    if (!codec) {
        qWarning() << "[AudioDecoder] 未找到音频解码器";
        return output;
    }

    // Open codec
    AVCodecContext *codecRaw = avcodec_alloc_context3(codec);
    std::unique_ptr<AVCodecContext, CodecCtxDeleter> codecCtx(codecRaw);
    avcodec_parameters_to_context(codecCtx.get(), audioStream->codecpar);
    if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
        qWarning() << "[AudioDecoder] 无法打开音频解码器";
        return output;
    }

    // Setup resampler: any input format → 16kHz mono float32
    SwrContext *swrRaw = nullptr;
    AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_MONO;
    AVChannelLayout inChLayout = codecCtx->ch_layout;

    swr_alloc_set_opts2(&swrRaw, &outChLayout, AV_SAMPLE_FMT_FLT, TARGET_SAMPLE_RATE, &inChLayout,
                        codecCtx->sample_fmt, codecCtx->sample_rate, 0, nullptr);
    if (!swrRaw || swr_init(swrRaw) < 0) {
        qWarning() << "[AudioDecoder] 重采样器初始化失败";
        if (swrRaw)
            swr_free(&swrRaw);
        return output;
    }
    std::unique_ptr<SwrContext, SwrCtxDeleter> swrCtx(swrRaw);

    // Seek if needed
    if (startMs > 0) {
        int64_t seekTs = av_rescale_q(startMs * 1000, {1, AV_TIME_BASE}, audioStream->time_base);
        av_seek_frame(fmtCtx.get(), audioIdx, seekTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx.get());
    }

    // Pre-allocate output (estimate based on duration)
    int64_t durationMs = fmtCtx->duration > 0
                             ? (fmtCtx->duration / 1000)
                             : (audioStream->duration > 0
                                    ? av_rescale_q(audioStream->duration, audioStream->time_base, {1, 1000})
                                    : 0);
    if (durationMs > 0) {
        const auto beginMs = std::max<int64_t>(0, startMs);
        const auto finishMs = endMs > 0 ? std::min(durationMs, endMs) : durationMs;
        int64_t estimatedSamples =
            (std::min<int64_t>(600000, std::max<int64_t>(0, finishMs - beginMs)) * TARGET_SAMPLE_RATE) / 1000;
        output.reserve(static_cast<size_t>(estimatedSamples));
    }

    // Decode loop
    std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());

    int64_t endPts =
        (endMs > 0) ? av_rescale_q(endMs * 1000, {1, AV_TIME_BASE}, audioStream->time_base) : INT64_MAX;

    int64_t totalDuration = (durationMs > 0) ? durationMs : 1;
    int lastPercent = 0;

    const auto consumeFrame = [&] {
        if (m_cancelled)
            return;

        // Skip frames before start time
        if (startMs > 0 && frame->pts != AV_NOPTS_VALUE) {
            int64_t frameMs = av_rescale_q(frame->pts, audioStream->time_base, {1, 1000});
            const int64_t frameDurationMs =
                frame->sample_rate > 0 ? int64_t(frame->nb_samples) * 1000 / frame->sample_rate : 0;
            if (frameMs + frameDurationMs <= startMs) {
                av_frame_unref(frame.get());
                return;
            }
        }

        // Resample
        int outSamples = swr_get_out_samples(swrCtx.get(), frame->nb_samples);
        std::vector<float> buffer(outSamples);
        uint8_t *outBuf = reinterpret_cast<uint8_t *>(buffer.data());

        int converted = swr_convert(swrCtx.get(), &outBuf, outSamples,
                                    const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
        if (converted > 0) {
            int beginSample = 0, endSample = converted;
            if (frame->pts != AV_NOPTS_VALUE) {
                const int64_t frameMs = av_rescale_q(frame->pts, audioStream->time_base, {1, 1000});
                if (startMs > frameMs)
                    beginSample =
                        int(std::min<int64_t>(converted, (startMs - frameMs) * TARGET_SAMPLE_RATE / 1000));
                if (endMs >= 0)
                    endSample =
                        int(std::clamp<int64_t>((endMs - frameMs) * TARGET_SAMPLE_RATE / 1000, 0, converted));
            }
            if (endSample > beginSample)
                output.insert(output.end(), buffer.begin() + beginSample, buffer.begin() + endSample);
        }

        // Progress reporting
        if (m_progress && frame->pts != AV_NOPTS_VALUE) {
            int64_t currentMs = av_rescale_q(frame->pts, audioStream->time_base, {1, 1000});
            int percent = static_cast<int>((currentMs * 100) / totalDuration);
            percent = std::clamp(percent, 0, 100);
            if (percent > lastPercent) {
                lastPercent = percent;
                m_progress(percent);
            }
        }

        av_frame_unref(frame.get());
    };

    while (av_read_frame(fmtCtx.get(), pkt.get()) >= 0) {
        if (m_cancelled)
            break;

        if (pkt->stream_index != audioIdx) {
            av_packet_unref(pkt.get());
            continue;
        }

        // Check end boundary
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts > endPts) {
            av_packet_unref(pkt.get());
            break;
        }

        if (avcodec_send_packet(codecCtx.get(), pkt.get()) < 0) {
            av_packet_unref(pkt.get());
            continue;
        }

        while (avcodec_receive_frame(codecCtx.get(), frame.get()) == 0)
            consumeFrame();

        av_packet_unref(pkt.get());
    }

    // Drain delayed codec frames, then consume the resampler tail into an actual buffer.
    if (!m_cancelled && avcodec_send_packet(codecCtx.get(), nullptr) >= 0)
        while (avcodec_receive_frame(codecCtx.get(), frame.get()) == 0)
            consumeFrame();
    while (!m_cancelled) {
        const int capacity = swr_get_out_samples(swrCtx.get(), 0);
        if (capacity <= 0)
            break;
        std::vector<float> tail(capacity);
        uint8_t *bytes = reinterpret_cast<uint8_t *>(tail.data());
        const int count = swr_convert(swrCtx.get(), &bytes, capacity, nullptr, 0);
        if (count <= 0)
            break;
        output.insert(output.end(), tail.begin(), tail.begin() + count);
    }
    if (endMs > 0) {
        const int64_t maximum =
            std::max<int64_t>(0, endMs - std::max<int64_t>(0, startMs)) * TARGET_SAMPLE_RATE / 1000;
        if (output.size() > size_t(maximum))
            output.resize(size_t(maximum));
    }
    if (m_cancelled)
        output.clear();

    qDebug() << "[AudioDecoder] 解码完成:" << filePath << "| 采样数:" << output.size()
             << "| 时长:" << (output.size() / TARGET_SAMPLE_RATE) << "s";

    return output;
}
