#include "service/agent/frame_extractor.h"

#include <QDebug>

#include <algorithm>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context) avformat_close_input(&context);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const {
        if (context) avcodec_free_context(&context);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        if (frame) av_frame_free(&frame);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        if (packet) av_packet_free(&packet);
    }
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const {
        if (context) sws_freeContext(context);
    }
};

bool isCancelled(const std::atomic_bool* cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}

int interruptIo(void* opaque)
{
    return isCancelled(static_cast<const std::atomic_bool*>(opaque)) ? 1 : 0;
}

void setError(QString* target, const QString& message)
{
    if (target) *target = message;
    qWarning() << "[FrameExtractor]" << message;
}

} // namespace

QVector<FrameExtractor::Frame> FrameExtractor::extract(const QString& videoPath,
                                                        const QVector<int64_t>& requestedMs,
                                                        const Options& options,
                                                        const std::atomic_bool* cancelled,
                                                        QString* errorMessage)
{
    QVector<Frame> output;
    if (videoPath.isEmpty() || requestedMs.isEmpty()) return output;
    if (options.maxEdge <= 0) {
        setError(errorMessage, QStringLiteral("maxEdge 必须大于 0"));
        return output;
    }

    QVector<int64_t> targets = requestedMs;
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    AVFormatContext* formatRaw = avformat_alloc_context();
    if (!formatRaw) {
        setError(errorMessage, QStringLiteral("无法分配媒体格式上下文"));
        return output;
    }
    formatRaw->interrupt_callback.callback = &interruptIo;
    formatRaw->interrupt_callback.opaque = const_cast<std::atomic_bool*>(cancelled);
    const QByteArray pathUtf8 = videoPath.toUtf8();
    if (avformat_open_input(&formatRaw, pathUtf8.constData(), nullptr, nullptr) < 0) {
        if (formatRaw) avformat_free_context(formatRaw);
        setError(errorMessage, isCancelled(cancelled)
            ? QStringLiteral("帧提取已取消") : QStringLiteral("无法打开视频: %1").arg(videoPath));
        return output;
    }
    std::unique_ptr<AVFormatContext, FormatContextDeleter> format(formatRaw);
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        setError(errorMessage, QStringLiteral("无法读取视频流信息"));
        return output;
    }

    int videoStreamIndex = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(index);
            break;
        }
    }
    if (videoStreamIndex < 0) {
        setError(errorMessage, QStringLiteral("未找到视频流"));
        return output;
    }

    AVStream* stream = format->streams[videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        setError(errorMessage, QStringLiteral("未找到视频解码器"));
        return output;
    }

    AVCodecContext* codecRaw = avcodec_alloc_context3(codec);
    if (!codecRaw || avcodec_parameters_to_context(codecRaw, stream->codecpar) < 0
        || avcodec_open2(codecRaw, codec, nullptr) < 0) {
        if (codecRaw) avcodec_free_context(&codecRaw);
        setError(errorMessage, QStringLiteral("无法初始化视频解码器"));
        return output;
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> codecContext(codecRaw);

    const int sourceWidth = codecContext->width;
    const int sourceHeight = codecContext->height;
    const double scale = qMin(1.0, static_cast<double>(options.maxEdge)
        / static_cast<double>(qMax(sourceWidth, sourceHeight)));
    const int targetWidth = qMax(1, static_cast<int>(sourceWidth * scale));
    const int targetHeight = qMax(1, static_cast<int>(sourceHeight * scale));

    std::unique_ptr<SwsContext, SwsContextDeleter> scaler(sws_getContext(
        sourceWidth, sourceHeight, codecContext->pix_fmt,
        targetWidth, targetHeight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
        setError(errorMessage, QStringLiteral("无法初始化像素格式转换器"));
        return output;
    }

    std::unique_ptr<AVFrame, FrameDeleter> decoded(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> rgb(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!decoded || !rgb || !packet) {
        setError(errorMessage, QStringLiteral("无法分配解码缓冲区"));
        return output;
    }

    if (av_image_alloc(rgb->data, rgb->linesize, targetWidth, targetHeight,
                       AV_PIX_FMT_RGB24, 1) < 0) {
        setError(errorMessage, QStringLiteral("无法分配 RGB 图像缓冲区"));
        return output;
    }
    struct RgbBufferDeleter {
        AVFrame* frame = nullptr;
        ~RgbBufferDeleter() { if (frame) av_freep(&frame->data[0]); }
    } rgbBuffer{rgb.get()};

    const int64_t streamStart = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    int targetIndex = 0;
    const auto consumeFrame = [&]() {
        const int64_t timestamp = decoded->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) return;
        const int64_t ptsMs = qMax<int64_t>(0, av_rescale_q(
            timestamp - streamStart, stream->time_base, AVRational{1, 1000}));
        while (targetIndex < targets.size() && ptsMs >= targets.at(targetIndex)) {
            sws_scale(scaler.get(), decoded->data, decoded->linesize, 0, sourceHeight,
                      rgb->data, rgb->linesize);
            QImage image(rgb->data[0], targetWidth, targetHeight, rgb->linesize[0],
                         QImage::Format_RGB888);
            Frame frame;
            frame.requestedMs = targets.at(targetIndex);
            frame.ptsMs = ptsMs;
            frame.image = image.copy();
            output.append(std::move(frame));
            ++targetIndex;
        }
    };

    while (!isCancelled(cancelled) && targetIndex < targets.size()
           && av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == videoStreamIndex
            && avcodec_send_packet(codecContext.get(), packet.get()) >= 0) {
            while (!isCancelled(cancelled)
                   && avcodec_receive_frame(codecContext.get(), decoded.get()) == 0) {
                consumeFrame();
                av_frame_unref(decoded.get());
                if (targetIndex >= targets.size()) break;
            }
        }
        av_packet_unref(packet.get());
    }

    if (!isCancelled(cancelled) && targetIndex < targets.size()) {
        avcodec_send_packet(codecContext.get(), nullptr);
        while (!isCancelled(cancelled) && targetIndex < targets.size()
               && avcodec_receive_frame(codecContext.get(), decoded.get()) == 0) {
            consumeFrame();
            av_frame_unref(decoded.get());
        }
    }

    if (isCancelled(cancelled)) output.clear();
    return output;
}
