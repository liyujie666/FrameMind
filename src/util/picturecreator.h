#ifndef FRAMEMIND_PICTURECREATOR_H
#define FRAMEMIND_PICTURECREATOR_H

#include <QString>
#include <QImage>

/**
 * 视频缩略图工具（项目侧封装）。
 *
 * 与样例 PictureCreator 接口形态保持一致（getPreViewImage / duration / getFileType）。
 * 实现后端使用 SmartPlayer SDK：
 *   - getPreViewImage → SmartPlayer::extractThumbnail() 同步抽取一帧并落盘为 jpg，
 *                        再从 jpg 读回 QImage 返回（统一 QImage 接口，调用方免感知 jpg）。
 *   - duration        → 同步抽帧后，从 SDK 临时实例的 mediaInfo() 读取 durationMs。
 *                        -1 表示未知。
 *   - getFileType     → 路径协议/扩展名判断（FILE / AUDIO / RTSP / RTMP / UNKNOWN）。
 *
 * 注意：此对象按"一次性使用"语义设计，每次调用 getPreViewImage 都会在内部启停一个
 * 临时 SmartPlayer 实例。对于批量扫描场景请改用 ThumbnailBuilder（见 thumbnailbuilder.h），
 * 它在 worker 线程里调用本类，并把结果异步发回主线程。
 */
class PictureCreator {
public:
    PictureCreator();
    ~PictureCreator();

    /**
     * 从视频文件中抽取一帧并缩放到 (maxWidth, maxHeight) 范围内，返回 QImage。
     * 失败/无视频流时返回空 QImage。
     *
     * @param videoPath  本地文件路径（RTSP/RTMP 不支持缩略图）
     * @param maxWidth   输出图像最长边上限（保持宽高比）
     * @param maxHeight  输出图像最短边上限（保持宽高比）
     */
    QImage getPreViewImage(const QString& videoPath,
                           int maxWidth  = 120,
                           int maxHeight = 90);

    /**
     * 返回最近一次 getPreViewImage 调用所获得的媒体时长（秒，向下取整）。
     * 未调用过或失败时为 -1。
     */
    int duration();

    /**
     * 不修改内部状态地获取某个视频文件的时长（秒）。
     * @return 时长秒数；失败 / 文件不存在 / 不支持时返回 0
     */
    int duration(const QString& videoPath);

    /**
     * 文件类型识别：
     *   "FILE"   - 本地视频/音频文件
     *   "AUDIO"  - 本地音频文件
     *   "RTSP"   - rtsp://
     *   "RTMP"   - rtmp://
     *   "UNKNOWN"- 其它
     */
    QString getFileType(const QString& videoPath);

private:
    int  duration_ = -1;
};

#endif // FRAMEMIND_PICTURECREATOR_H
