#ifndef FRAMEMIND_ONE_SHOT_VLM_CHANNEL_H
#define FRAMEMIND_ONE_SHOT_VLM_CHANNEL_H

#include <QObject>
#include <QImage>
#include <QList>
#include <QString>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>

#include <functional>
#include "model/model_reply.h"
#include "model/image_encoding_options.h"

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

    explicit OneShotVlmChannel(AgentService* agent, QObject* parent = nullptr, int requestTimeoutMs = 0);

    void enqueue(const QString& systemPrompt,
                 const QString& userText,
                 const QList<QImage>& frames,
                 Priority priority,
                 const QString& cancellationKey,
                 std::function<void(const QString&)> onDone);

    void enqueueDetailed(const QString& systemPrompt, const QString& userText,
                         const QList<QImage>& frames, Priority priority,
                         const QString& cancellationKey, std::function<void(ModelReply)> onDone);
    void enqueueRequest(const QString& requestId, const QString& systemPrompt, const QString& userText,
                        const QList<QImage>& frames, Priority priority, const QString& cancellationKey,
                        std::function<void(ModelReply)> onDone, const ImageEncodingOptions& imageOptions = {},
                        int maxOutputTokens = 0);
    void cancelRequest(const QString& requestId);

    /// 移除排队任务，并立即终止同一视频正在进行的后台请求。
    void cancelBackground(const QString& cancellationKey);

    bool isBusy() const { return m_running; }
    int pendingCount() const { return m_pending.size(); }
    QString modelSignature() const;
    int watchdogTimeoutMs() const;

private:
    struct Request {
        QString systemPrompt;
        QString userText;
        QList<QImage> frames;
        ImageEncodingOptions imageOptions;
        int maxOutputTokens = 0;
        QElapsedTimer queueTimer;
        qint64 queueMs = 0;
        Priority priority = Priority::Background;
        QString cancellationKey;
        QString conversationId;
        QString requestId;
        bool notifyCancellation = false; // explicit requests always receive one terminal callback
        std::function<void(ModelReply)> onDone;
        bool discardResult = false;
    };

    void startNext();
    void finishActive(ModelReply reply);

    QTimer m_idleTimer;
    QTimer m_deadlineTimer;
    int m_idleTimeoutMs = 0;

    AgentService* m_agent = nullptr;
    QVector<Request> m_pending;
    Request m_active;
    bool m_running = false;
    int m_requestTimeoutMs = 0;
};

#endif // FRAMEMIND_ONE_SHOT_VLM_CHANNEL_H
