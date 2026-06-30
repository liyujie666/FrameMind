#ifndef FRAMEMIND_IMAGEPROCESSOR_H
#define FRAMEMIND_IMAGEPROCESSOR_H

#include <QImage>
#include <QByteArray>
#include <QSize>
#include <QRectF>
#include <cstdint>

#include "smartplayerdefs.h"  // SmartPixelFormat

/**
 * SDK 帧数据 → QImage 转换工具（纯静态方法）。
 *
 * SDK 帧布局见 smartplayercallback.h::onVideoFrame：
 *   YUV420P / NV12 / RGBA / BGRA。
 * 注意：传入的 data 仅在回调期间有效，本类内部一律完成拷贝后返回独立 QImage。
 */
class ImageProcessor {
public:
    /// 按像素格式转为 QImage（返回的 QImage 拥有独立数据）
    static QImage fromVideoFrame(const uint8_t* data, int width, int height,
                                 SmartPixelFormat format);

    /// 等比缩放到目标尺寸内
    static QImage scaleToFit(const QImage& img, const QSize& targetSize);

    /// 编码为 base64 JPEG（M2 发送给 AI 用）
    static QByteArray toBase64Jpeg(const QImage& img, int quality = 85);

    /// 按归一化坐标裁剪
    static QImage cropRegion(const QImage& img, const QRectF& normalizedRect);

private:
    static QImage fromYUV420P(const uint8_t* data, int width, int height);
    static QImage fromNV12(const uint8_t* data, int width, int height);
};

#endif // FRAMEMIND_IMAGEPROCESSOR_H
