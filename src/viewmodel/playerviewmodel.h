#ifndef FRAMEMIND_PLAYERVIEWMODEL_H
#define FRAMEMIND_PLAYERVIEWMODEL_H

#include <QObject>
#include <QImage>
#include <QString>
#include <QElapsedTimer>
#include <cstdint>

#include "model/playertypes.h"
#include "model/videoframe.h"

class PlayerService;
class EventBus;

/**
 * 播放器视图模型：持有播放 UI 状态，协调 PlayerService。
 *
 * View 只与本 VM 交互，不直接持有 PlayerService。
 * 跨 VM 通信走 EventBus（如 AI 回复时间戳跳转 → seekToPosition）。
 */
class PlayerViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(int64_t position READ position NOTIFY positionChanged)
    Q_PROPERTY(int64_t duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(PlayerState state READ state NOTIFY stateChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(float speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMute NOTIFY mutedChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)

public:
    explicit PlayerViewModel(PlayerService* playerService,
                             EventBus* eventBus,
                             QObject* parent = nullptr);

    int64_t position() const   { return m_position; }
    int64_t duration() const   { return m_duration; }
    PlayerState state() const  { return m_state; }
    int volume() const         { return m_volume; }
    float speed() const        { return m_speed; }
    bool muted() const         { return m_muted; }
    QString mediaTitle() const { return m_mediaTitle; }

public slots:
    void openFile(const QString& filePath);
    void togglePlay();
    void seek(int64_t posMs);
    void setVolume(int vol);
    void setSpeed(float speed);
    void setMute(bool mute);
    void seekToTimestamp(int64_t posMs);   // AI 回复点击时间戳跳转
    void seekAndPlay(int64_t posMs);       // 时间线/字幕点击：seek 并自动播放
    void captureFrameForAI(int64_t posMs); // 截当前帧回包给 AI（M2 仅当前帧）

signals:
    void positionChanged(int64_t posMs);
    void durationChanged(int64_t durationMs);
    void stateChanged(PlayerState state);
    void volumeChanged(int vol);
    void speedChanged(float speed);
    void mutedChanged(bool muted);
    void mediaTitleChanged(const QString& title);
    void frameReady(const QImage& frame);
    void rawFrameReady(const VideoFrame& frame);
    void errorOccurred(const QString& msg);
    /// SDK 成功打开视频后发出（此时 duration 已填好）
    void videoOpened(const QString& filePath);
    /// 即将打开新文件（在 SDK open 调用之前），用于 UI 立即清空上一帧画面
    void videoFileChanging();

private slots:
    void seekToTimestampWithResult(int64_t posMs, const QString& requestId);
    void executePlayerAction(const QString& action, const QString& requestId);

private:
    void connectService();
    void pollSeekResult(const QString& requestId, int attempt);
    void pollPlayerActionResult(const QString& action, const QString& requestId,
                                int attempt);

    PlayerService* m_playerService = nullptr;
    EventBus*      m_eventBus = nullptr;

    int64_t m_position = 0;
    int64_t m_duration = 0;
    PlayerState m_state = PlayerState::Stopped;
    int   m_volume = 50;
    float m_speed = 1.0f;
    bool  m_muted = false;
    QString m_mediaTitle;

    bool    m_seeking = false;
    int64_t m_seekTarget = 0;
    QElapsedTimer m_seekTimer;
    static constexpr int kSeekCooldownMs = 150;
    static constexpr int kSeekToleranceMs = 750;
    static constexpr int kSeekPollIntervalMs = 50;
    static constexpr int kSeekPollAttempts = 60;

    // 每次 openFile 递增：rawFrameReady 转发时比对代次，丢弃旧视频的延迟帧
    uint32_t m_openGeneration = 0;
    // openResult 成功后与 m_openGeneration 同步，用于开放新视频帧的接收
    uint32_t m_acceptGeneration = 0;

    // seekAndPlay：seek 落地后自动 play()（Stopped 状态用）
    bool m_pendingPlay = false;
    // 普通 seek in Paused：seek 落地后恢复 pause()
    bool m_pauseAfterSeek = false;
    QString m_pendingSeekRequestId;
};

#endif // FRAMEMIND_PLAYERVIEWMODEL_H
