#ifndef FRAMEMIND_EVENTBUS_H
#define FRAMEMIND_EVENTBUS_H

#include <QObject>
#include <QImage>
#include <QString>
#include <cstdint>

/**
 * 跨模块 / 跨 ViewModel 松耦合通信总线。
 *
 * 约定（见 architecture-design.md §7.2）：
 *   ViewModel 之间禁止互相持有引用，所有跨 VM 通信走 EventBus。
 *   EventBus 是允许存在的全局单例（其余模块统一走 DIContainer）。
 *
 * 发布方调用 notifyXxx() 公开方法触发事件；订阅方 connect 对应信号。
 * （Qt 信号是 protected，外部不能直接 emit，故提供公开转发方法。）
 */
class EventBus : public QObject {
    Q_OBJECT
public:
    static EventBus* instance();

    // ---- 发布方法（公开） ----
    void notifyVideoOpened(const QString& filePath) { emit videoOpened(filePath); }
    void requestSeek(int64_t posMs) { emit seekToPosition(posMs); }
    void requestFrameForAI(int64_t posMs) { emit frameForAIRequested(posMs); }
    void provideScreenshotForAI(const QImage& frame, int64_t tsMs)
    {
        emit screenshotForAI(frame, tsMs);
    }

signals:
    /// 视频已打开
    void videoOpened(const QString& filePath);
    /// AI / 时间线 请求跳转到指定时间点
    void seekToPosition(int64_t posMs);
    /// 请求对指定时间点截帧用于 AI（-1 表示当前帧）
    void frameForAIRequested(int64_t posMs);
    /// 截帧回包给 AI
    void screenshotForAI(const QImage& frame, int64_t tsMs);

private:
    explicit EventBus(QObject* parent = nullptr);
    Q_DISABLE_COPY(EventBus)
};

#endif // FRAMEMIND_EVENTBUS_H
