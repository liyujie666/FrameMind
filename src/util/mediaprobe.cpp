#include "util/mediaprobe.h"

#include "util/picturecreator.h"

#include <QtConcurrent/QtConcurrent>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>

namespace {

// 把缩略图写到磁盘缓存：<AppData>/thumbnails/<md5(path)>.jpg
// 返回绝对路径；如果提供的 image 为空则返回空。
QString writeThumbnailCache(const QString& mediaPath, const QImage& image)
{
    if (image.isNull() || mediaPath.isEmpty()) return {};
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/thumbnails");
    QDir().mkpath(dir);

    const QByteArray h =
        QCryptographicHash::hash(mediaPath.toUtf8(), QCryptographicHash::Md5).toHex();
    const QString out = dir + QStringLiteral("/") + QString::fromLatin1(h)
                        + QStringLiteral(".jpg");
    image.save(out, "JPG", 85);
    return out;
}

MediaProbeResult probeImpl(const QString& mediaPath, int thumbMaxSize)
{
    MediaProbeResult r;
    if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath)) return r;

    PictureCreator pc;
    r.durationMs       = qint64(pc.duration(mediaPath)) * 1000;  // 秒→ms
    r.thumbnailImage   = pc.getPreViewImage(mediaPath, thumbMaxSize, thumbMaxSize);
    if (!r.thumbnailImage.isNull()) {
        r.thumbnail = writeThumbnailCache(mediaPath, r.thumbnailImage);
    }
    return r;
}

} // namespace

QFuture<MediaProbeResult>
MediaProbe::probeAsync(const QString& mediaPath, int thumbMaxSize)
{
    return QtConcurrent::run([mediaPath, thumbMaxSize] {
        return probeImpl(mediaPath, thumbMaxSize);
    });
}
