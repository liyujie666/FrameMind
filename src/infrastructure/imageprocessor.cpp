#include "infrastructure/imageprocessor.h"

#include <QBuffer>
#include <QDebug>
#include <algorithm>
#include <cstring>

namespace {
// 限幅到 [0,255]
inline uint8_t clip8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}
}  // namespace

QImage ImageProcessor::fromVideoFrame(const uint8_t* data, int width, int height,
                                      SmartPixelFormat format)
{
    if (!data || width <= 0 || height <= 0) {
        qWarning() << "[ImageProcessor] Invalid params: data=" << data << "size:" << width << "x" << height;
        return QImage();
    }

    //qDebug() << "[ImageProcessor] Converting frame:" << width << "x" << height << "format:" << format;

    switch (format) {
    case SP_FMT_RGBA: {
        // 每像素 RGBA8888；QImage 包装后必须 copy（data 回调后失效）
        QImage img(data, width, height, width * 4, QImage::Format_RGBA8888);
        return img.copy();
    }
    case SP_FMT_BGRA: {
        // 内存字节序 B,G,R,A 对应 QImage::Format_ARGB32（小端 0xAARRGGBB）
        QImage img(data, width, height, width * 4, QImage::Format_ARGB32);
        return img.copy();
    }
    case SP_FMT_YUV420P:
        return fromYUV420P(data, width, height);
    case SP_FMT_NV12:
        return fromNV12(data, width, height);
    case SP_FMT_UNKNOWN:
    default:
        qWarning() << "[ImageProcessor] Unknown pixel format:" << format;
        return QImage();
    }
}

QImage ImageProcessor::fromYUV420P(const uint8_t* data, int width, int height)
{
    // 平面布局: [Y: w*h][U: w/2*h/2][V: w/2*h/2]
    const int ySize = width * height;
    const int cW = width / 2;
    const int cH = height / 2;
    const uint8_t* yPlane = data;
    const uint8_t* uPlane = data + ySize;
    const uint8_t* vPlane = uPlane + cW * cH;

    QImage img(width, height, QImage::Format_RGB32);
    for (int j = 0; j < height; ++j) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(j));
        const int cj = j / 2;
        for (int i = 0; i < width; ++i) {
            const int ci = i / 2;
            const int y = yPlane[j * width + i];
            const int u = uPlane[cj * cW + ci] - 128;
            const int v = vPlane[cj * cW + ci] - 128;
            // BT.601 近似
            const int r = (298 * (y - 16) + 409 * v + 128) >> 8;
            const int g = (298 * (y - 16) - 100 * u - 208 * v + 128) >> 8;
            const int b = (298 * (y - 16) + 516 * u + 128) >> 8;
            line[i] = qRgb(clip8(r), clip8(g), clip8(b));
        }
    }
    return img;
}

QImage ImageProcessor::fromNV12(const uint8_t* data, int width, int height)
{
    // 布局: [Y: w*h][interleaved UV: w*h/2]，UV 顺序为 U,V 交错
    const int ySize = width * height;
    const uint8_t* yPlane = data;
    const uint8_t* uvPlane = data + ySize;
    const int cW = width / 2;

    QImage img(width, height, QImage::Format_RGB32);
    for (int j = 0; j < height; ++j) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(j));
        const int cj = j / 2;
        for (int i = 0; i < width; ++i) {
            const int ci = i / 2;
            const int y = yPlane[j * width + i];
            const int u = uvPlane[cj * cW * 2 + ci * 2] - 128;
            const int v = uvPlane[cj * cW * 2 + ci * 2 + 1] - 128;
            const int r = (298 * (y - 16) + 409 * v + 128) >> 8;
            const int g = (298 * (y - 16) - 100 * u - 208 * v + 128) >> 8;
            const int b = (298 * (y - 16) + 516 * u + 128) >> 8;
            line[i] = qRgb(clip8(r), clip8(g), clip8(b));
        }
    }
    return img;
}

QImage ImageProcessor::scaleToFit(const QImage& img, const QSize& targetSize)
{
    if (img.isNull() || targetSize.isEmpty()) {
        return img;
    }
    return img.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QByteArray ImageProcessor::toBase64Jpeg(const QImage& img, int quality)
{
    if (img.isNull()) {
        return QByteArray();
    }
    QByteArray raw;
    QBuffer buffer(&raw);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "JPEG", quality);
    return raw.toBase64();
}

QImage ImageProcessor::cropRegion(const QImage& img, const QRectF& normalizedRect)
{
    if (img.isNull()) {
        return img;
    }
    const QRect r(
        static_cast<int>(normalizedRect.x() * img.width()),
        static_cast<int>(normalizedRect.y() * img.height()),
        static_cast<int>(normalizedRect.width() * img.width()),
        static_cast<int>(normalizedRect.height() * img.height()));
    return img.copy(r.intersected(img.rect()));
}
