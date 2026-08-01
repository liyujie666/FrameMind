#ifndef FRAMEMIND_PLAYERSERVICE_H
#define FRAMEMIND_PLAYERSERVICE_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QFuture>
#include <memory>
#include <mutex>

#include "model/playertypes.h"
#include "model/videoinfo.h"
#include "model/videoframe.h"
#include "smartplayerdefs.h"

class SmartPlayer;

/**
 * 封装 SmartPlayer SDK，屏蔽底层线程回调细节，向上暴露 Qt 信号。
 *
 * SDK 通过 setCallback(SmartPlayerCallback*) 注册回调，且回调发生在
 * 播放器内部解码线程。PlayerService 内部组合一个 CallbackBridge
 * （实现 SmartPlayerCallback），把事件转成 PlayerService 的 Qt 信号。
 * 接收方（PlayerViewModel）位于主线程，依赖 QueuedConnection 完成跨线程投递。
 */
class PlayerService : public QObject {
    Q_OBJECT
public:
    explicit PlayerService(QObject* parent = nullptr);
    ~PlayerService() override;

    void open(const QString& filePath);
    void play();
    void pause();
    void stop();
    void seek(int64_t posMs);              // 异步
    void setVolume(int vol);               // 0~100
    void setSpeed(float speed);
    void setMute(bool mute);
    void setHardwareDecode(bool enable);
    void takeScreenshot(const QString& savePath);  // 异步

    /**
     * 异步截取指定时间点的帧（AI 工具 seek_and_analyze 用）。
     * M1 仅留接口骨架，返回一个已取消的 future；真正实现见 M3-T2。
     */
    QFuture<QImage> captureFrameAt(int64_t posMs, int timeoutMs = 2000);

    /// 从指定视频文件截取帧，不依赖当前播放器打开的媒体。
    QFuture<QImage> captureFrameAt(const QString& videoPath,
                                   int64_t posMs,
                                   int timeoutMs = 2000);

    /// 同步获取当前已缓存的最近一帧（不触发 seek），可能为空
    QImage lastDecodedFrame() const;

    // 状态查询
    int64_t duration() const;
    int64_t position() const;
    PlayerState state() const;
    VideoInfo videoInfo() const;

signals:
    void positionChanged(int64_t posMs);
    void durationChanged(int64_t durationMs);
    void stateChanged(PlayerState state);
    void rawFrameReady(const VideoFrame& frame);     // 原始帧数据，GPU 渲染用
    void frameDecoded(const QImage& frame);          // QImage 帧（缩略图等遗留消费者）
    void openResult(bool success, const QString& error);
    void mediaInfoReady(const VideoInfo& info);
    void playFinished();
    void screenshotReady(const QString& path, bool success);
    void errorOccurred(const QString& msg);

private:
    // CallbackBridge 在 SDK 解码线程触发，转发为 PlayerService 的 Qt 信号
    class CallbackBridge;
    friend class CallbackBridge;

    // 供 CallbackBridge 调用（运行在 SDK 线程）的转发入口
    void onSdkStateChanged(SmartPlayerState state);
    void onSdkPositionChanged(int64_t posMs);
    void onSdkDurationChanged(int64_t durationMs);
    void onSdkOpenResult(bool success, const char* err);
    void onSdkMediaInfoReady(const SmartMediaInfo& info);
    void onSdkPlayFinished();
    void onSdkError(const char* msg);
    void onSdkScreenshot(const char* path, bool success);
    void onSdkVideoFrame(const uint8_t* data, int width, int height,
                         SmartPixelFormat format);

    static PlayerState toPlayerState(SmartPlayerState s);

    std::unique_ptr<SmartPlayer>    m_player;
    std::unique_ptr<CallbackBridge> m_bridge;

    mutable std::mutex m_frameMutex;
    QImage             m_lastFrame;        // 缓存最近一帧（懒转换），受 m_frameMutex 保护
    VideoFrame         m_lastRawFrame;     // 缓存最近一帧原始数据，受 m_frameMutex 保护

    mutable std::mutex m_infoMutex;
    VideoInfo          m_videoInfo;        // 受 m_infoMutex 保护
};

#endif // FRAMEMIND_PLAYERSERVICE_H
