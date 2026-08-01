#ifndef FRAMEMIND_VIDEOFRAME_H
#define FRAMEMIND_VIDEOFRAME_H

#include <QSharedData>
#include <QByteArray>
#include <QMetaType>
#include "smartplayerdefs.h"

/**
 * 轻量视频帧容器——承载 SDK 回调的原始像素数据（YUV420P/NV12/RGBA/BGRA），
 * 使用隐式共享（QSharedData）实现零拷贝信号传递。
 *
 * 设计目标：SDK 回调线程只做一次 memcpy 构造 VideoFrame，
 * 渲染线程直接将原始数据上传 GPU 由 shader 完成格式转换。
 */
class VideoFrameData : public QSharedData {
public:
    QByteArray data;
    int width = 0;
    int height = 0;
    SmartPixelFormat format = SP_FMT_UNKNOWN;
};

class VideoFrame {
public:
    VideoFrame() : d(new VideoFrameData) {}

    VideoFrame(const uint8_t* rawData, size_t size, int w, int h, SmartPixelFormat fmt)
        : d(new VideoFrameData)
    {
        d->data = QByteArray(reinterpret_cast<const char*>(rawData), static_cast<int>(size));
        d->width = w;
        d->height = h;
        d->format = fmt;
    }

    bool isNull() const { return d->data.isEmpty() || d->width <= 0 || d->height <= 0; }

    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(d->data.constData()); }
    int width() const { return d->width; }
    int height() const { return d->height; }
    SmartPixelFormat format() const { return d->format; }
    int dataSize() const { return d->data.size(); }

private:
    QSharedDataPointer<VideoFrameData> d;
};

Q_DECLARE_METATYPE(VideoFrame)

#endif // FRAMEMIND_VIDEOFRAME_H
