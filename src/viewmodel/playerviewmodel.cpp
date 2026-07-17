#include "viewmodel/playerviewmodel.h"

#include "service/playerservice.h"
#include "infrastructure/eventbus.h"
#include "model/videoinfo.h"

#include <QDebug>

PlayerViewModel::PlayerViewModel(PlayerService* playerService,
                                 EventBus* eventBus,
                                 QObject* parent)
    : QObject(parent)
    , m_playerService(playerService)
    , m_eventBus(eventBus)
{
    connectService();
}

void PlayerViewModel::connectService()
{
    if (!m_playerService) return;

    // PlayerService → VM（SDK 线程信号经 QueuedConnection 投递到主线程）
    connect(m_playerService, &PlayerService::positionChanged, this,
            [this](int64_t pos) {
                if (m_position == pos) return;
                m_position = pos;
                emit positionChanged(pos);
            });

    connect(m_playerService, &PlayerService::durationChanged, this,
            [this](int64_t dur) {
                if (m_duration == dur) return;
                m_duration = dur;
                emit durationChanged(dur);
            });

    connect(m_playerService, &PlayerService::stateChanged, this,
            [this](PlayerState s) {
                if (m_state == s) return;
                m_state = s;
                emit stateChanged(s);
            });

    connect(m_playerService, &PlayerService::frameDecoded, this,
            [this](const QImage& frame) { emit frameReady(frame); });

    connect(m_playerService, &PlayerService::mediaInfoReady, this,
            [this](const VideoInfo& info) {
                const QString title = info.fileName.isEmpty()
                                          ? info.filePath
                                          : info.fileName;
                if (m_mediaTitle == title) return;
                m_mediaTitle = title;
                emit mediaTitleChanged(title);
            });

    connect(m_playerService, &PlayerService::errorOccurred, this,
            [this](const QString& msg) { emit errorOccurred(msg); });

    // open 成功后应用默认音量/倍速并自动播放
    connect(m_playerService, &PlayerService::openResult, this,
            [this](bool success, const QString& err) {
                if (!success) {
                    emit errorOccurred(err.isEmpty() ? tr("视频打开失败") : err);
                    return;
                }
                m_playerService->setVolume(m_volume);
                m_playerService->setSpeed(m_speed);
                m_playerService->setMute(m_muted);
                m_playerService->play();
            });

    // 跨 VM：AI / 时间线 请求跳转
    if (m_eventBus) {
        connect(m_eventBus, &EventBus::seekToPosition,
                this, &PlayerViewModel::seekToTimestamp);
        connect(m_eventBus, &EventBus::frameForAIRequested,
                this, &PlayerViewModel::captureFrameForAI);
    }
}

void PlayerViewModel::captureFrameForAI(int64_t /*posMs*/)
{
    if (!m_playerService || !m_eventBus) return;
    // M2 仅支持「当前帧」：直接取最近解码帧（captureFrameAt 在 M3 实现）
    const QImage frame = m_playerService->lastDecodedFrame();
    m_eventBus->provideScreenshotForAI(frame, m_position);
}

void PlayerViewModel::openFile(const QString& filePath)
{
    if (!m_playerService || filePath.isEmpty()) return;
    m_playerService->open(filePath);
}

void PlayerViewModel::togglePlay()
{
    if (!m_playerService) return;
    if (m_state == PlayerState::Playing) {
        m_playerService->pause();
    } else {
        m_playerService->play();
    }
}

void PlayerViewModel::seek(int64_t posMs)
{
    if (!m_playerService) return;
    if (posMs < 0) posMs = 0;
    if (m_duration > 0 && posMs > m_duration) posMs = m_duration;
    m_playerService->seek(posMs);
}

void PlayerViewModel::setVolume(int vol)
{
    vol = qBound(0, vol, 100);
    if (m_volume == vol) return;
    m_volume = vol;
    if (m_playerService) m_playerService->setVolume(vol);
    emit volumeChanged(vol);
}

void PlayerViewModel::setSpeed(float speed)
{
    if (qFuzzyCompare(m_speed, speed)) return;
    m_speed = speed;
    if (m_playerService) m_playerService->setSpeed(speed);
    emit speedChanged(speed);
}

void PlayerViewModel::setMute(bool mute)
{
    if (m_muted == mute) return;
    m_muted = mute;
    if (m_playerService) m_playerService->setMute(mute);
    emit mutedChanged(mute);
}

void PlayerViewModel::seekToTimestamp(int64_t posMs)
{
    seek(posMs);
}
