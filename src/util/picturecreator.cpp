#include "util/picturecreator.h"

#include "smartplayer.h"
#include "smartplayercallback.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QUuid>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QEventLoop>
#include <QTimer>
#include <QStandardPaths>

namespace {

// 临时回调：只关心 onMediaInfoReady 来抓 durationMs，其它忽略。
class DurationProbeCallback : public SmartPlayerCallback {
public:
    void onMediaInfoReady(const SmartMediaInfo& info) override {
        QMutexLocker lk(&m_mutex);
        m_gotInfo = true;
        m_durationMs = info.durationMs;
        m_wait.wakeAll();
    }
    bool waitForInfo(int timeoutMs) {
        QMutexLocker lk(&m_mutex);
        if (m_gotInfo) return true;
        return m_wait.wait(&m_mutex, timeoutMs);
    }
    int64_t durationMs() const {
        QMutexLocker lk(&m_mutex);
        return m_durationMs;
    }
private:
    mutable QMutex m_mutex;
    QWaitCondition m_wait;
    bool    m_gotInfo    = false;
    int64_t m_durationMs = 0;
};

// SDK 内部已对路径做 UTF-8 转换；这里保留入口便于将来扩展

} // namespace

PictureCreator::PictureCreator()  = default;
PictureCreator::~PictureCreator() = default;

QString PictureCreator::getFileType(const QString& videoPath)
{
    const QString lower = videoPath.toLower();
    if (lower.startsWith(QStringLiteral("rtsp"))) return QStringLiteral("RTSP");
    if (lower.startsWith(QStringLiteral("rtmp"))) return QStringLiteral("RTMP");

    const QString suffix = QFileInfo(videoPath).suffix().toLower();
    static const QSet<QString> audioExt = {
        QStringLiteral("mp3"), QStringLiteral("wav"),
        QStringLiteral("aac"), QStringLiteral("flac"),
        QStringLiteral("m4a"), QStringLiteral("ogg"),
    };
    if (audioExt.contains(suffix)) return QStringLiteral("AUDIO");

    static const QSet<QString> videoExt = {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"), QStringLiteral("flv"), QStringLiteral("ts"),
        QStringLiteral("webm"), QStringLiteral("wmv"), QStringLiteral("m4v"),
        QStringLiteral("3gp"),
    };
    if (videoExt.contains(suffix)) return QStringLiteral("FILE");

    return QStringLiteral("UNKNOWN");
}

int PictureCreator::duration()
{
    return duration_;
}

int PictureCreator::duration(const QString& videoPath)
{
    if (!QFileInfo::exists(videoPath)) return 0;
    const QString type = getFileType(videoPath);
    if (type != QStringLiteral("FILE") && type != QStringLiteral("AUDIO")) {
        return 0;  // RTSP/RTMP/UNKNOWN 不支持
    }

    DurationProbeCallback cb;
    int64_t resultMs = 0;
    {
        // 把 SmartPlayer 放在独立作用域：函数返回时自动析构，避免跨线程 stop() 死锁
        SmartPlayer player;
        player.setCallback(&cb);
        const QByteArray url = videoPath.toUtf8();
        player.open(url.constData());
        if (cb.waitForInfo(3000)) {
            resultMs = cb.durationMs();
        }
        // 析构 player 时 SDK 内部线程自然清理；不再显式 stop()
    }
    if (resultMs < 0) return 0;
    return int(resultMs / 1000);
}

QImage PictureCreator::getPreViewImage(const QString& videoPath,
                                       int maxWidth,
                                       int maxHeight)
{
    duration_ = -1;

    const QString type = getFileType(videoPath);
    if (type == QStringLiteral("RTSP")) {
        duration_ = 0;
        return QImage(QStringLiteral(":/image_rtsp.png"));
    }
    if (type == QStringLiteral("RTMP")) {
        duration_ = 0;
        return QImage(QStringLiteral(":/image_rtmp.png"));
    }
    if (type == QStringLiteral("AUDIO")) {
        // 音频也尝试用 SDK 抽一帧作为预览（如 ID3 内嵌图）；失败回退到默认图标
        // 这里走 SDK 路径
    } else if (type == QStringLiteral("UNKNOWN")) {
        // 继续交给 SDK 让它尝试解码
    }

    if (!QFileInfo::exists(videoPath)) {
        duration_ = 0;
        return QImage();
    }

    // 准备临时输出目录：<AppData>/thumbnails
    QString outDir;
    {
        const QString base =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        outDir = base + QStringLiteral("/thumbnails");
        QDir().mkpath(outDir);
    }
    const QString outPath =
        outDir + QStringLiteral("/pc_") + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QStringLiteral(".jpg");

    // —— 1. 抽 thumbnail（同步）——
    SmartPlayer::ThumbnailOptions opts;
    opts.targetWidth = qMax(maxWidth, maxHeight);
    opts.jpegQuality = 3;

    const QByteArray url = videoPath.toUtf8();
    bool ok = SmartPlayer::extractThumbnail(url.constData(),
                                            outPath.toUtf8().constData(),
                                            opts);

    // 抽帧前后顺便 probe duration（独立完成，已有的实例已析构）
    duration_ = duration(videoPath);
    if (duration_ < 0) duration_ = 0;

    if (!ok) {
        QFile::remove(outPath);
        if (type == QStringLiteral("AUDIO")) {
            return QImage(QStringLiteral(":/image_audio_2.jpg"));
        }
        return QImage();
    }

    // —— 2. 读回 jpg 并按 (maxWidth, maxHeight) 等比缩放 —— 
    QImage img(outPath, "JPG");
    if (img.isNull()) {
        QFile::remove(outPath);
        if (type == QStringLiteral("AUDIO")) {
            return QImage(QStringLiteral(":/image_audio_2.jpg"));
        }
        return QImage();
    }
    img = img.convertToFormat(QImage::Format_RGB888);

    const int longSide = qMax(img.width(), img.height());
    QSize targetSize(img.size());
    if (longSide > qMax(maxWidth, maxHeight)) {
        const qreal factor = qreal(qMax(maxWidth, maxHeight)) / qreal(longSide);
        targetSize = QSize(int(img.width()  * factor),
                           int(img.height() * factor));
        img = img.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // 删除临时 jpg（FileManagerService 会再单独存一份到永久缓存）
    QFile::remove(outPath);
    return img;
}
