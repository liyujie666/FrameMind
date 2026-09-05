#ifndef FRAMEMIND_ONE_SHOT_VLM_CHANNEL_H
#define FRAMEMIND_ONE_SHOT_VLM_CHANNEL_H

#include <QObject>
#include <QImage>
#include <QList>
#include <QString>
#include <QVector>

#include <functional>

class AgentService;

/**
 * 后台/局部视觉分析的独立串行通道。
 *
 * 它持有专属 AgentService（及其专属 NetworkClient），从而不与用户问答的
 * 流式会话共用请求状态；通道内部仍严格串行，符合 AgentService 单流约束。
 */
class OneShotVlmChannel final : public QObject
{
    Q_OBJECT
public:
    enum class Priority { Background, Interactive };

    explicit OneShotVlmChannel(AgentService* agent, QObject* parent = nullptr);

    void enqueue(const QString& systemPrompt,
                 const QString& userText,
                 const QList<QImage>& frames,
                 Priority priority,
                 const QString& cancellationKey,
                 std::function<void(const QString&)> onDone);

    /// 取消尚未发起的同一视频后台任务；正在请求只会被标记为丢弃结果。
    void cancelBackground(const QString& cancellationKey);

    bool isBusy() const { return m_running; }
    int pendingCount() const { return m_pending.size(); }

private:
    struct Request {
        QString systemPrompt;
        QString userText;
        QList<QImage> frames;
        Priority priority = Priority::Background;
        QString cancellationKey;
        QString conversationId;
        std::function<void(const QString&)> onDone;
        bool discardResult = false;
    };

    void startNext();
    void finishActive(const QString& content);

    AgentService* m_agent = nullptr;
    QVector<Request> m_pending;
    Request m_active;
    bool m_running = false;
};

#endif // FRAMEMIND_ONE_SHOT_VLM_CHANNEL_H
