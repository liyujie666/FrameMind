#ifndef FRAMEMIND_PLAYERVIEWMODEL_H
#define FRAMEMIND_PLAYERVIEWMODEL_H

#include <QObject>
#include <QImage>
#include <QString>
#include <cstdint>

#include "model/playertypes.h"

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
    void captureFrameForAI(int64_t posMs); // 📷：截当前帧回包给 AI（M2 仅当前帧）

signals:
    void positionChanged(int64_t posMs);
    void durationChanged(int64_t durationMs);
    void stateChanged(PlayerState state);
    void volumeChanged(int vol);
    void speedChanged(float speed);
    void mutedChanged(bool muted);
    void mediaTitleChanged(const QString& title);
    void frameReady(const QImage& frame);
    void errorOccurred(const QString& msg);

private:
    void connectService();

    PlayerService* m_playerService = nullptr;
    EventBus*      m_eventBus = nullptr;

    int64_t m_position = 0;
    int64_t m_duration = 0;
    PlayerState m_state = PlayerState::Stopped;
    int   m_volume = 50;
    float m_speed = 1.0f;
    bool  m_muted = false;
    QString m_mediaTitle;
};

#endif // FRAMEMIND_PLAYERVIEWMODEL_H
