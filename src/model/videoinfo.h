#ifndef FRAMEMIND_VIDEOINFO_H
#define FRAMEMIND_VIDEOINFO_H

#include <QString>
#include <QMetaType>
#include <cstdint>

/**
 * 视频信息（从 SDK 的 SmartMediaInfo 映射而来）。
 *
 * 注意：SmartMediaInfo 不直接提供 width/height，
 * 这两个字段在拿到首帧后由 PlayerService 回填。
 */
struct VideoInfo {
    QString filePath;
    QString fileName;
    QString format;
    int64_t durationMs = 0;
    int64_t bitRate    = 0;
    double  frameRate  = 0.0;
    int     width      = 0;
    int     height     = 0;
    bool    hasAudio   = false;
};

Q_DECLARE_METATYPE(VideoInfo)

#endif // FRAMEMIND_VIDEOINFO_H
