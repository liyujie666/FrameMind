#include "viewmodel/playerviewmodel.h"

#include "service/playerservice.h"
#include "infrastructure/eventbus.h"
#include "model/videoinfo.h"

#include <QDebug>
#include <QTimer>
#include <QFutureWatcher>
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
                // Seek 冷却期间完全忽略外部位置回调，避免队列中残留的旧位置信号导致进度条回跳
                if (m_seeking) {
                    if (!m_seekTimer.hasExpired(kSeekCooldownMs)) return;
                    m_seeking = false;

                    // Requests with a completion id are finalized by pollSeekResult,
                    // which verifies the SDK's actual position before changing state.
                    if (m_pendingSeekRequestId.isEmpty()) {
                        if (m_pauseAfterSeek) {
                            m_pauseAfterSeek = false;
                            m_playerService->pause();
                        } else if (m_pendingPlay) {
                            m_pendingPlay = false;
                            m_playerService->play();
                        }
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
        connect(m_eventBus, &EventBus::seekToPositionWithResult,
                this, &PlayerViewModel::seekToTimestampWithResult);
        connect(m_eventBus, &EventBus::playerActionRequested,
                this, &PlayerViewModel::executePlayerAction);
        connect(m_eventBus, &EventBus::frameForAIRequested,
                this, &PlayerViewModel::captureFrameForAI);
    }
}

void PlayerViewModel::captureFrameForAI(int64_t posMs)
{
    if (!m_playerService || !m_eventBus) return;

    const QString videoPath = m_playerService->videoInfo().filePath;
    const int64_t targetMs = posMs >= 0 ? posMs : m_position;
    if (videoPath.isEmpty()) {
        m_eventBus->provideScreenshotForAI(m_playerService->lastDecodedFrame(), m_position);
        return;
    }

    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, targetMs]() {
        QImage frame = watcher->result();
        if (frame.isNull()) frame = m_playerService->lastDecodedFrame();
        m_eventBus->provideScreenshotForAI(frame, targetMs);
        watcher->deleteLater();
    });
    watcher->setFuture(m_playerService->captureFrameAt(videoPath, targetMs, 2000));
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
    m_pendingPlay = false;
    m_pauseAfterSeek = false;
    m_position = posMs;
    emit positionChanged(posMs);

    // The SDK requires a running pipeline for reliable seeks.
    if (m_state == PlayerState::Stopped) {
        m_pendingPlay = true;
        m_playerService->play();
    } else if (m_state == PlayerState::Paused) {
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

void PlayerViewModel::seekToTimestampWithResult(int64_t posMs,const QString& requestId)
{
    if (!m_playerService || !m_eventBus || requestId.isEmpty()) {
        if (m_eventBus && !requestId.isEmpty()) {
            m_eventBus->notifySeekCompleted(
                requestId, false, 0, QStringLiteral("播放器服务不可用"));
        }
        return;
    }
    if (m_playerService->videoInfo().filePath.isEmpty()) {
        m_eventBus->notifySeekCompleted(
            requestId, false, 0, QStringLiteral("当前没有已打开的视频"));
        return;
    }
    if (m_pendingSeekRequestId.isEmpty()) {
        m_pendingSeekRequestId = requestId;
    } else {
        m_eventBus->notifySeekCompleted(
            m_pendingSeekRequestId, false, m_playerService->position(),
            QStringLiteral("seek 请求被新的播放器操作替代"));
        m_pendingSeekRequestId = requestId;
    }
    seek(posMs);
    pollSeekResult(requestId, 0);
}

void PlayerViewModel::executePlayerAction(const QString& action,
                                           const QString& requestId)
{
    if (!m_playerService || !m_eventBus || requestId.isEmpty()) {
        if (m_eventBus && !requestId.isEmpty()) {
            m_eventBus->notifyPlayerActionCompleted(
                action, requestId, false, 0, QStringLiteral("播放器服务不可用"));
        }
        return;
    }
    if (m_playerService->videoInfo().filePath.isEmpty()) {
        m_eventBus->notifyPlayerActionCompleted(
            action, requestId, false, 0, QStringLiteral("当前没有已打开的视频"));
        return;
    }
    if (action == QLatin1String("seek")) {
        m_eventBus->notifyPlayerActionCompleted(
            action, requestId, false, m_playerService->position(),
            QStringLiteral("seek 必须通过 seekToPositionWithResult 调用"));
        return;
    }
    if (action != QLatin1String("play") && action != QLatin1String("pause")) {
        m_eventBus->notifyPlayerActionCompleted(
            action, requestId, false, m_playerService->position(),
            QStringLiteral("未知播放器操作: %1").arg(action));
        return;
    }

    if (action == QLatin1String("play")) m_playerService->play();
    else m_playerService->pause();
    pollPlayerActionResult(action, requestId, 0);
}

void PlayerViewModel::pollSeekResult(const QString& requestId, int attempt)
{
    if (!m_eventBus || !m_playerService || m_pendingSeekRequestId != requestId) return;
    const int64_t actual = m_playerService->position();
    if (qAbs(actual - m_seekTarget) <= kSeekToleranceMs) {
        m_pendingSeekRequestId.clear();
        m_seeking = false;
        if (m_pauseAfterSeek) {
            m_pauseAfterSeek = false;
            m_playerService->pause();
        } else if (m_pendingPlay) {
            m_pendingPlay = false;
            m_playerService->play();
        }
        m_eventBus->notifySeekCompleted(requestId, true, actual, {});
        return;
    }
    if (attempt >= kSeekPollAttempts) {
        m_pendingSeekRequestId.clear();
        m_seeking = false;
        m_eventBus->notifySeekCompleted(
            requestId, false, actual, QStringLiteral("播放器未确认到达目标位置"));
        return;
    }
    QTimer::singleShot(kSeekPollIntervalMs, this,
                       [this, requestId, attempt] { pollSeekResult(requestId, attempt + 1); });
}

void PlayerViewModel::pollPlayerActionResult(const QString& action,
                                               const QString& requestId,
                                               int attempt)
{
    if (!m_eventBus || !m_playerService) return;
    const bool playing = m_playerService->state() == PlayerState::Playing;
    const bool paused = m_playerService->state() == PlayerState::Paused;
    const bool reached = action == QLatin1String("play") ? playing : paused;
    if (reached) {
        m_eventBus->notifyPlayerActionCompleted(action, requestId, true,
                                                m_playerService->position(), {});
        return;
    }
    if (attempt >= kSeekPollAttempts) {
        m_eventBus->notifyPlayerActionCompleted(
            action, requestId, false, m_playerService->position(),
            QStringLiteral("播放器未确认执行 %1").arg(action));
        return;
    }
    QTimer::singleShot(kSeekPollIntervalMs, this,
                       [this, action, requestId, attempt] {
                           pollPlayerActionResult(action, requestId, attempt + 1);
                       });
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

    // seek() 在 Stopped 状态会主动启动播放，在 Paused 状态会临时恢复运行。
    // 因此这里不再额外设置可能与普通 seek 串扰的 pendingPlay 标志。
    seek(posMs);
}
