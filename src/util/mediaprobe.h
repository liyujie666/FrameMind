#ifndef FRAMEMIND_MEDIAPROBE_H
#define FRAMEMIND_MEDIAPROBE_H

#include <QString>
#include <QImage>
#include <QFuture>
#include <QFutureWatcher>
#include <memory>

/**
 * 媒体探针结果。
 *   durationMs     - 该媒体时长（ms，<=0 表示未知）
 *   thumbnail      - SDK 生成的 jpg 缓存绝对路径；空表示失败
 *   thumbnailImage - 已经读取并缩放到目标尺寸的 QImage；空表示失败
 */
struct MediaProbeResult {
    qint64 durationMs = 0;
    QString thumbnail;        // 缓存路径，绝对
    QImage  thumbnailImage;   // 已缩放的图，可用于即时显示
};

/**
 * 异步媒体探针：在 worker 线程里跑 SmartPlayer 同步抽取一帧并 probe 时长。
 *
 * 设计动机：
 *   - FileListViewModel 需要在扫描目录后异步补全时长 + 缩略图，
 *     否则同步调用会卡主线程
 *   - 通过 QFuture + QFutureWatcher 把结果发回主线程
 *
 * 使用：
 *   MediaProbe probe;
 *   auto fut = probe.probeAsync("/path/to/video.mp4", /*maxSize*\/ 320);
 *   QFutureWatcher<MediaProbeResult> w;
 *   w.setFuture(fut);
 *   connect(&w, &QFutureWatcher<...>::finished,
 *           this, [this]{ auto r = w.future().result(); ... });
 */
class MediaProbe {
public:
    MediaProbe()  = default;
    ~MediaProbe() = default;

    /**
     * 异步 probe 一个媒体文件。
     * 在 QtConcurrent 全局线程池中执行，不阻塞调用者所在线程。
     *
     * @param mediaPath 媒体文件绝对路径
     * @param thumbMaxSize 缩略图最长边（像素，等比缩放）
     * @return future；future.result() 返回 MediaProbeResult
     */
    QFuture<MediaProbeResult> probeAsync(const QString& mediaPath,
                                         int thumbMaxSize = 320);
};

#endif // FRAMEMIND_MEDIAPROBE_H
