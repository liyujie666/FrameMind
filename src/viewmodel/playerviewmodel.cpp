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
                // Seek 冷却期间完全忽略外部位置回调，
                // 避免 QueuedConnection 队列中残留的旧位置信号导致进度条回跳
                if (m_seeking) {
                    if (!m_seekTimer.hasExpired(kSeekCooldownMs)) return;
                    m_seeking = false;

                    // 暂停状态 seek 修复：seek 落地后恢复暂停或触发 play
                    if (m_pauseAfterSeek) {
                        m_pauseAfterSeek = false;
                        m_playerService->pause();
                    } else if (m_pendingPlay) {
                        m_pendingPlay = false;
                        m_playerService->play();
                    }
                }
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

    connect(m_playerService, &PlayerService::rawFrameReady, this,
            [this](const VideoFrame& frame) {
                // 丢弃 openFile 之前排队的旧视频帧（QueuedConnection FIFO 中可能残留）
                if (m_openGeneration != m_acceptGeneration) return;
                emit rawFrameReady(frame);
            });

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
                // 新文件打开成功，开放帧接收：令 acceptGeneration 追上 openGeneration
                m_acceptGeneration = m_openGeneration;
                emit videoOpened(m_playerService->videoInfo().filePath);
            });

    // 播放到末尾：仅切换 UI 状态为 Ended，不再 seek(0)
    // （SDK 此时已是 Stopped，seek 无效；重播走 reopen 路径）
    connect(m_playerService, &PlayerService::playFinished, this,
            [this]() {
                m_state = PlayerState::Ended;
                emit stateChanged(PlayerState::Ended);
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
    // 递增代际：此后到来的旧视频 rawFrameReady 信号将被过滤丢弃，
    // 直到新文件的 openResult 成功后才重新开放（m_acceptGeneration 同步）
    ++m_openGeneration;
    emit videoFileChanging();
    m_playerService->open(filePath);
}

void PlayerViewModel::togglePlay()
{
    if (!m_playerService) return;
    if (m_state == PlayerState::Playing) {
        m_playerService->pause();
    } else if (m_state == PlayerState::Ended) {
        // SDK 在 Ended 后已是 Stopped，seek/play 均无效，重新 open 是唯一可靠路径
        // openResult 回调里会自动 play()
        const QString path = m_playerService->videoInfo().filePath;
        if (!path.isEmpty()) m_playerService->open(path);
    } else {
        m_playerService->play();
    }
}

void PlayerViewModel::seek(int64_t posMs)
{
    if (!m_playerService) return;
    if (posMs < 0) posMs = 0;
    if (m_duration > 0 && posMs > m_duration) posMs = m_duration;

    m_seeking = true;
    m_seekTarget = posMs;
    m_seekTimer.start();
    m_position = posMs;
    emit positionChanged(posMs);

    // SDK 在 Paused 状态下 seek 会损坏音频管道（seek 后音频静默且无法恢复）。
    // 解决方式：先 play() 让 SDK 进入 Running 状态，seek 完成后再 pause() 回来。
    // m_pauseAfterSeek 标志由 positionChanged 冷却期结束时消费。
    if (m_state == PlayerState::Paused) {
        m_pauseAfterSeek = true;
        m_playerService->play();
    }

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

void PlayerViewModel::seekAndPlay(int64_t posMs)
{
    if (!m_playerService) return;

    if (m_state == PlayerState::Ended) {
        // SDK 已是 Stopped，seek/play 均无效，重新 open；openResult 回调里自动 play()
        const QString path = m_playerService->videoInfo().filePath;
        if (!path.isEmpty()) m_playerService->open(path);
        return;
    }

    // 清除可能残留的 pause 意图：seekAndPlay 目标是播放，不需要 seek 后恢复暂停
    m_pauseAfterSeek = false;

    if (m_state == PlayerState::Stopped) {
        // Stopped 状态下 seek() 内部不会自动 play，用 pendingPlay 在冷却期后触发
        m_pendingPlay = true;
    }
    // Paused 状态：seek() 内部会 play→seek，m_pauseAfterSeek=false 保证不会 pause 回去
    // Playing 状态：seek() 直接 seek，SDK 继续播放，无需额外处理

    seek(posMs);
}
