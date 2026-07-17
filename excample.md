#ifndef PICTURECREATOR_H
#define PICTURECREATOR_H
#include <QImage>
#include <string>
extern"C"{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
class PictureCreator
{
public:
    PictureCreator();
    ~PictureCreator();

    QImage getPreViewImage(const QString &videoPath,int maxWidth = 120,int maxHeight = 90);
    int duration();
    int duration(const QString &videoPath);
    QString getFileType(const QString &videoPath);
private:
    QImage convertFrameToQImage(AVFrame *frame, int maxWidth, int maxHeight);
    int duration_ = -1;
};

#endif // PICTURECREATOR_H


#include "picturecreator.h"
#include <QDebug>

PictureCreator::PictureCreator() {
    avformat_network_init();
}

PictureCreator::~PictureCreator() {
    avformat_network_deinit();
}

QImage PictureCreator::getPreViewImage(const QString &videoPath, int maxWidth, int maxHeight)
{
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    AVCodecParameters *codecParams = nullptr;
    const AVCodec *codec = nullptr;
    int videoStreamIdx = -1;
    AVFrame *frame = av_frame_alloc();
    QImage previewImage;

    QString type = getFileType(videoPath);

    if(type == "AUDIO")
    {
        // 1. 打开MP3文件
        if (avformat_open_input(&fmtCtx, videoPath.toStdString().c_str(), nullptr, nullptr) != 0) {
            duration_ = 0;
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            duration_ = 0;
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        duration_ = fmtCtx->duration * av_q2d(AV_TIME_BASE_Q);

        // 2. 查找【专辑封面流】(附加图片流 attached_pic)
        int coverStreamIdx = -1;
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            AVStream* stream = fmtCtx->streams[i];
            // 判断是否为内嵌专辑封面
            if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                coverStreamIdx = i;
                break;
            }
        }

        // 3. 未找到封面 → 返回默认音频图标
        if (coverStreamIdx == -1) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }

        // 4. 找到封面 → 初始化解码器
        codecParams = fmtCtx->streams[coverStreamIdx]->codecpar;
        codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }

        // 读取封面数据包）
        AVPacket pkt = fmtCtx->streams[coverStreamIdx]->attached_pic;
        if (avcodec_send_packet(codecCtx, &pkt) == 0) {
            if (avcodec_receive_frame(codecCtx, frame) == 0) {
                previewImage = convertFrameToQImage(frame, maxWidth, maxHeight);
            }
        }
        // Clean up codec context since we return early from this block
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        av_frame_free(&frame);
        if (previewImage.isNull()) {
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        return previewImage;
    }
    else if(type == "FILE")
    {
        // 1. 打开视频文件
        if (avformat_open_input(&fmtCtx, videoPath.toStdString().c_str(), nullptr, nullptr) != 0) {
            qWarning() << "Failed to open video file:" << videoPath.toStdString().c_str();
            goto cleanup;
        }

        // 2. 查找流信息
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            qWarning() << "Failed to find stream info";
            goto cleanup;
        }

        // 3. 查找视频流
        duration_ = fmtCtx->duration * av_q2d(AV_TIME_BASE_Q);

        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIdx = i;
                break;
            }
        }
        if (videoStreamIdx == -1) {
            qWarning() << "No video stream found";
            goto cleanup;
        }

        // 4. 初始化解码器
        codecParams = fmtCtx->streams[videoStreamIdx]->codecpar;
        codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            qWarning() << "Unsupported codec";
            goto cleanup;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            qWarning() << "Failed to allocate codec context";
            goto cleanup;
        }
        if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
            qWarning() << "Failed to copy codec parameters";
            goto cleanup;
        }
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            qWarning() << "Failed to open codec";
            goto cleanup;
        }

        // 5. 读取并解码第一帧
        {
            AVPacket packet;
            while (av_read_frame(fmtCtx, &packet) >= 0) {
                if (packet.stream_index == videoStreamIdx) {
                    if (avcodec_send_packet(codecCtx, &packet) == 0) {
                        if (avcodec_receive_frame(codecCtx, frame) == 0) {
                            previewImage = convertFrameToQImage(frame, maxWidth, maxHeight);
                            av_packet_unref(&packet);
                            break;
                        }
                    }
                }
                av_packet_unref(&packet);
            }
        }
    }else if(type == "RTSP"){
        duration_ = 0;
        av_frame_free(&frame);
        return QImage(":/SmartPlayer-icon/image_rtsp.png");
    }else if(type == "RTMP"){
        duration_ = 0;
        av_frame_free(&frame);
        return QImage(":/SmartPlayer-icon/image_rtmp.png");
    }

cleanup:
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (fmtCtx) avformat_close_input(&fmtCtx);
    if (frame) av_frame_free(&frame);

    if (type == "AUDIO" && previewImage.isNull()) {
        return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
    }

    return previewImage;
}

QImage PictureCreator::convertFrameToQImage(AVFrame *frame, int maxWidth, int maxHeight)
{

    SwsContext *swsCtx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        maxWidth, maxHeight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
        );

    if (!swsCtx) return QImage();

    AVFrame *rgbFrame = av_frame_alloc();
    rgbFrame->format = AV_PIX_FMT_RGB24;
    rgbFrame->width = maxWidth;
    rgbFrame->height = maxHeight;
    av_frame_get_buffer(rgbFrame, 0);

    sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height,
              rgbFrame->data, rgbFrame->linesize);

    QImage image(
        rgbFrame->data[0],
        maxWidth,
        maxHeight,
        rgbFrame->linesize[0],
        QImage::Format_RGB888
        );

    QImage copy = image.copy();
    sws_freeContext(swsCtx);
    av_frame_free(&rgbFrame);

    return copy;
}

int PictureCreator::duration()
{
    return duration_;
}

int PictureCreator::duration(const QString &videoPath)
{
    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, videoPath.toStdString().c_str(), nullptr, nullptr) < 0) {
        return 0;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return 0;
    }
    int durationSec = fmtCtx->duration * av_q2d(AV_TIME_BASE_Q);
    avformat_close_input(&fmtCtx);
    return durationSec;
}

QString PictureCreator::getFileType(const QString &videoPath)
{
    // 原代码完全复用，支持MP3识别
    QString path = videoPath.toLower();
    if (path.endsWith(".mp4") || path.endsWith(".mkv") || path.endsWith(".avi")) {
        return "FILE";
    } else if (path.endsWith(".mp3")) {
        return "AUDIO";
    } else if (videoPath.startsWith("rtsp")) {
        return "RTSP";
    }else if(videoPath.startsWith("rtmp")){
        return "RTMP";
    }
    return "UNKNOWN";
}

#include "picturecreator.h"
#include <QDebug>

PictureCreator::PictureCreator() {
    avformat_network_init();
}

PictureCreator::~PictureCreator() {
    avformat_network_deinit();
}

QImage PictureCreator::getPreViewImage(const QString &videoPath, int maxWidth, int maxHeight)
{
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    AVCodecParameters *codecParams = nullptr;
    const AVCodec *codec = nullptr;
    int videoStreamIdx = -1;
    AVFrame *frame = av_frame_alloc();
    QImage previewImage;

    QString type = getFileType(videoPath);

    if(type == "AUDIO")
    {
        // 1. 打开MP3文件
        if (avformat_open_input(&fmtCtx, videoPath.toStdString().c_str(), nullptr, nullptr) != 0) {
            duration_ = 0;
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            duration_ = 0;
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        duration_ = fmtCtx->duration * av_q2d(AV_TIME_BASE_Q);

        // 2. 查找【专辑封面流】(附加图片流 attached_pic)
        int coverStreamIdx = -1;
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            AVStream* stream = fmtCtx->streams[i];
            // 判断是否为内嵌专辑封面
            if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                coverStreamIdx = i;
                break;
            }
        }

        // 3. 未找到封面 → 返回默认音频图标
        if (coverStreamIdx == -1) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }

        // 4. 找到封面 → 初始化解码器
        codecParams = fmtCtx->streams[coverStreamIdx]->codecpar;
        codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            avcodec_free_context(&codecCtx);
            avformat_close_input(&fmtCtx);
            av_frame_free(&frame);
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }

        // 读取封面数据包）
        AVPacket pkt = fmtCtx->streams[coverStreamIdx]->attached_pic;
        if (avcodec_send_packet(codecCtx, &pkt) == 0) {
            if (avcodec_receive_frame(codecCtx, frame) == 0) {
                previewImage = convertFrameToQImage(frame, maxWidth, maxHeight);
            }
        }
        // Clean up codec context since we return early from this block
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        av_frame_free(&frame);
        if (previewImage.isNull()) {
            return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
        }
        return previewImage;
    }
    else if(type == "FILE")
    {
        // 1. 打开视频文件
        if (avformat_open_input(&fmtCtx, videoPath.toStdString().c_str(), nullptr, nullptr) != 0) {
            qWarning() << "Failed to open video file:" << videoPath.toStdString().c_str();
            goto cleanup;
        }

        // 2. 查找流信息
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            qWarning() << "Failed to find stream info";
            goto cleanup;
        }

        // 3. 查找视频流
        duration_ = fmtCtx->duration * av_q2d(AV_TIME_BASE_Q);

        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIdx = i;
                break;
            }
        }
        if (videoStreamIdx == -1) {
            qWarning() << "No video stream found";
            goto cleanup;
        }

        // 4. 初始化解码器
        codecParams = fmtCtx->streams[videoStreamIdx]->codecpar;
        codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            qWarning() << "Unsupported codec";
            goto cleanup;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            qWarning() << "Failed to allocate codec context";
            goto cleanup;
        }
        if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
            qWarning() << "Failed to copy codec parameters";
            goto cleanup;
        }
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            qWarning() << "Failed to open codec";
            goto cleanup;
        }

        // 5. 读取并解码第一帧
        {
            AVPacket packet;
            while (av_read_frame(fmtCtx, &packet) >= 0) {
                if (packet.stream_index == videoStreamIdx) {
                    if (avcodec_send_packet(codecCtx, &packet) == 0) {
                        if (avcodec_receive_frame(codecCtx, frame) == 0) {
                            previewImage = convertFrameToQImage(frame, maxWidth, maxHeight);
                            av_packet_unref(&packet);
                            break;
                        }
                    }
                }
                av_packet_unref(&packet);
            }
        }
    }else if(type == "RTSP"){
        duration_ = 0;
        av_frame_free(&frame);
        return QImage(":/SmartPlayer-icon/image_rtsp.png");
    }else if(type == "RTMP"){
        duration_ = 0;
        av_frame_free(&frame);
        return QImage(":/SmartPlayer-icon/image_rtmp.png");
    }

cleanup:
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (fmtCtx) avformat_close_input(&fmtCtx);
    if (frame) av_frame_free(&frame);

    if (type == "AUDIO" && previewImage.isNull()) {
        return QImage(":/SmartPlayer-icon/image_audio_2.jpg");
    }

    return previewImage;
}

QImage PictureCreator::convertFrameToQImage(AVFrame *frame, int maxWidth, int maxHeight)
{

    SwsContext *swsCtx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        maxWidth, maxHeight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
        );

    if (!swsCtx) return QImage();

    AVFrame *rgbFrame = av_frame_alloc();
    rgbFrame->format = AV_PIX_FMT_RGB24;
    rgbFrame->width = maxWidth;
    rgbFrame->height = maxHeight;
    av_frame_get_buffer(rgbFrame, 0);

    sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height,
              rgbFrame->data, rgbFrame->linesize);

    QImage image(
        rgbFrame->data[0],
        maxWidth,
        maxHeight,
        rgbFrame->linesize[0],
        QImage::Format_RGB888
        );

    QImage copy = image.copy();
    sws_freeContext(swsCtx);
    av_frame_free(&rgbFrame);

    return copy;
}

int PictureCreator::duration()
{
    return duration_;
}

int PictureCreator::duration(const QString &videoPath)
{
    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, videoPath.toStdString().c_str(), nullptr, nullptr) < 0) {
        return 0;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return 0;
    }
    int durationSec = fmtCtx->duration * av_q2d(AV_TIME_BASE_Q);
    avformat_close_input(&fmtCtx);
    return durationSec;
}

QString PictureCreator::getFileType(const QString &videoPath)
{
    // 原代码完全复用，支持MP3识别
    QString path = videoPath.toLower();
    if (path.endsWith(".mp4") || path.endsWith(".mkv") || path.endsWith(".avi")) {
        return "FILE";
    } else if (path.endsWith(".mp3")) {
        return "AUDIO";
    } else if (videoPath.startsWith("rtsp")) {
        return "RTSP";
    }else if(videoPath.startsWith("rtmp")){
        return "RTMP";
    }
    return "UNKNOWN";
}
