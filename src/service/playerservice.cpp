#include "service/playerservice.h"

#include "infrastructure/imageprocessor.h"
#include "smartplayer.h"
#include "smartplayercallback.h"

#include <QFileInfo>
#include <QFutureInterface>

// ===================================================================
// CallbackBridge —— 实现 SDK 回调接口，转发到 PlayerService（SDK 线程）
// ===================================================================
class PlayerService::CallbackBridge : public SmartPlayerCallback {
public:
    explicit CallbackBridge(PlayerService* owner) : m_owner(owner) {}

    void onStateChanged(SmartPlayerState state) override {
        if (m_owner) m_owner->onSdkStateChanged(state);
    }
    void onPositionChanged(int64_t posMs) override {
        if (m_owner) m_owner->onSdkPositionChanged(posMs);
    }
    void onDurationChanged(int64_t durationMs) override {
        if (m_owner) m_owner->onSdkDurationChanged(durationMs);
    }
    void onOpenResult(bool success, const char* errMsg) override {
        if (m_owner) m_owner->onSdkOpenResult(success, errMsg);
    }
    void onMediaInfoReady(const SmartMediaInfo& info) override {
        if (m_owner) m_owner->onSdkMediaInfoReady(info);
    }
    void onPlayFinished() override {
        if (m_owner) m_owner->onSdkPlayFinished();
    }
    void onError(const char* msg) override {
        if (m_owner) m_owner->onSdkError(msg);
    }
    void onScreenshot(const char* path, bool success) override {
        if (m_owner) m_owner->onSdkScreenshot(path, success);
    }
    void onVideoFrame(const uint8_t* data, int width, int height,
                      SmartPixelFormat pixelFormat) override {
        if (m_owner) m_owner->onSdkVideoFrame(data, width, height, pixelFormat);
    }

private:
    PlayerService* m_owner = nullptr;
};

// ===================================================================
// PlayerService
// ===================================================================
PlayerService::PlayerService(QObject* parent)
    : QObject(parent)
    , m_player(std::make_unique<SmartPlayer>())
    , m_bridge(std::make_unique<CallbackBridge>(this))
{
    m_player->setCallback(m_bridge.get());
}

PlayerService::~PlayerService()
{
    if (m_player) {
        // 先解除回调，避免析构期间 SDK 线程仍回调到已销毁对象
        m_player->setCallback(nullptr);
        m_player->stop();
    }
}

PlayerState PlayerService::toPlayerState(SmartPlayerState s)
{
    switch (s) {
    case SP_STATE_RUNNING: return PlayerState::Playing;
    case SP_STATE_PAUSED:  return PlayerState::Paused;
    case SP_STATE_STOPPED:
    default:               return PlayerState::Stopped;
    }
}

// -------------------- 命令（主线程调用，做空指针保护）--------------------
void PlayerService::open(const QString& filePath)
{
    if (!m_player) return;
    {
        std::lock_guard<std::mutex> lk(m_infoMutex);
        m_videoInfo = VideoInfo{};
        m_videoInfo.filePath = filePath;
        m_videoInfo.fileName = QFileInfo(filePath).fileName();
    }
    {
        std::lock_guard<std::mutex> lk(m_frameMutex);
        m_lastFrame = QImage();
    }

    std::string url = filePath.toUtf8().constData();
    qDebug() << "Opening URL (UTF-8):" << QString::fromUtf8(url.c_str());

    m_player->open(url.c_str());
}

void PlayerService::play()                       { if (m_player) m_player->play(); }
void PlayerService::pause()                      { if (m_player) m_player->pause(); }
void PlayerService::stop()                       { if (m_player) m_player->stop(); }
void PlayerService::seek(int64_t posMs)          { if (m_player) m_player->seek(posMs); }
void PlayerService::setVolume(int vol)           { if (m_player) m_player->setVolume(vol); }
void PlayerService::setSpeed(float speed)        { if (m_player) m_player->setSpeed(speed); }
void PlayerService::setMute(bool mute)           { if (m_player) m_player->setMute(mute); }
void PlayerService::setHardwareDecode(bool en)   { if (m_player) m_player->setHardwareDecode(en); }

void PlayerService::takeScreenshot(const QString& savePath)
{
    if (m_player) m_player->takeScreenshot(savePath.toUtf8().constData());
}

QFuture<QImage> PlayerService::captureFrameAt(int64_t /*posMs*/, int /*timeoutMs*/)
{
    // M1 占位：真正的异步按时间点截帧在 M3-T2 实现。
    QFutureInterface<QImage> fi;
    fi.reportStarted();
    fi.reportCanceled();
    fi.reportFinished();
    return fi.future();
}

QImage PlayerService::lastDecodedFrame() const
{
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_lastFrame;
}

int64_t PlayerService::duration() const  { return m_player ? m_player->duration() : 0; }
int64_t PlayerService::position() const  { return m_player ? m_player->position() : 0; }

PlayerState PlayerService::state() const
{
    return m_player ? toPlayerState(m_player->state()) : PlayerState::Stopped;
}

VideoInfo PlayerService::videoInfo() const
{
    std::lock_guard<std::mutex> lk(m_infoMutex);
    return m_videoInfo;
}

// -------------------- SDK 回调转发（SDK 线程）--------------------
// 这些方法在 SDK 解码线程被调用；emit 信号后由 Qt 以 QueuedConnection
// 投递到主线程的 ViewModel。
void PlayerService::onSdkStateChanged(SmartPlayerState state)
{
    emit stateChanged(toPlayerState(state));
}

void PlayerService::onSdkPositionChanged(int64_t posMs)
{
    emit positionChanged(posMs);
}

void PlayerService::onSdkDurationChanged(int64_t durationMs)
{
    {
        std::lock_guard<std::mutex> lk(m_infoMutex);
        m_videoInfo.durationMs = durationMs;
    }
    emit durationChanged(durationMs);
}

void PlayerService::onSdkOpenResult(bool success, const char* err)
{
    emit openResult(success, QString::fromUtf8(err));
}

void PlayerService::onSdkMediaInfoReady(const SmartMediaInfo& info)
{
    VideoInfo vi;
    {
        std::lock_guard<std::mutex> lk(m_infoMutex);
        // filePath/fileName 已在 open() 时填好，这里补充其余字段
        m_videoInfo.format     = QString::fromUtf8(info.formatName);
        m_videoInfo.durationMs = info.durationMs;
        m_videoInfo.bitRate    = info.bitRate;
        m_videoInfo.frameRate  = info.videoFrameRate;
        m_videoInfo.hasAudio   = info.hasAudio;
        if (m_videoInfo.fileName.isEmpty())
            m_videoInfo.fileName = QString::fromUtf8(info.fileName);
        vi = m_videoInfo;
    }
    emit mediaInfoReady(vi);
}

void PlayerService::onSdkPlayFinished()
{
    emit playFinished();
}

void PlayerService::onSdkError(const char* msg)
{
    emit errorOccurred(QString::fromUtf8(msg));
}

void PlayerService::onSdkScreenshot(const char* path, bool success)
{
    emit screenshotReady(QString::fromUtf8(path), success);
}

void PlayerService::onSdkVideoFrame(const uint8_t* data, int width, int height,
                                    SmartPixelFormat format)
{
    // 1. 转 QImage（独立数据）
    QImage img = ImageProcessor::fromVideoFrame(data, width, height, format);
    if (img.isNull()) {
        qWarning() << "[PlayerService] Frame conversion failed";
        return;
    }

    // 2. 缓存最近一帧
    {
        std::lock_guard<std::mutex> lk(m_frameMutex);
        m_lastFrame = img;
    }
    // 3. 首帧回填宽高
    {
        std::lock_guard<std::mutex> lk(m_infoMutex);
        if (m_videoInfo.width == 0 || m_videoInfo.height == 0) {
            m_videoInfo.width = width;
            m_videoInfo.height = height;
        }
    }
    // 4. 跨线程投递给渲染层
    emit frameDecoded(img);
}
