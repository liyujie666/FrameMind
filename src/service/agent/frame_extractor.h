#ifndef FRAMEMIND_FRAME_EXTRACTOR_H
#define FRAMEMIND_FRAME_EXTRACTOR_H

#include <QImage>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>

/** 单次顺序解码的视频帧提取器，供离线索引使用，不依赖播放器状态。 */
class FrameExtractor final
{
public:
    struct Frame {
        int64_t requestedMs = 0;
        int64_t ptsMs = 0;
        QImage image;
    };

    struct Options {
        int maxEdge = 960;
        int jpegQuality = 0; // 预留：调用方持久化时控制质量
    };

    /**
     * 按递增目标时间一次性顺序解码。返回帧的 ptsMs 来自实际解码时间戳；
     * cancelled 可由索引任务的原子取消标志传入，随时中断 I/O 与解码。
     */
    static QVector<Frame> extract(const QString& videoPath,
                                  const QVector<int64_t>& requestedMs,
                                  const Options& options = {},
                                  const std::atomic_bool* cancelled = nullptr,
                                  QString* errorMessage = nullptr);
};

#endif // FRAMEMIND_FRAME_EXTRACTOR_H
