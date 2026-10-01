#include "service/agent/one_shot_vlm_channel.h"

#include "service/agentservice.h"

#include <QUuid>
#include <QTimer>
#include <utility>

QString OneShotVlmChannel::modelSignature() const {return m_agent?m_agent->modelSignature():QStringLiteral("unavailable");}

OneShotVlmChannel::OneShotVlmChannel(AgentService* agent, QObject* parent, int requestTimeoutMs)
    : QObject(parent)
    , m_agent(agent)
    , m_requestTimeoutMs(qMax(1, requestTimeoutMs))
{
    if (!m_agent) return;

    connect(m_agent, &AgentService::responseFinished, this,
            [this](const QString& conversationId, const ChatMessage& message) {
                if (!m_running || conversationId != m_active.conversationId) return;
                finishActive({message.content, {}});
            });
    connect(m_agent, &AgentService::responseError, this,
            [this](const QString& conversationId, const QString& error) {
                if (!m_running || conversationId != m_active.conversationId) return;
                finishActive({{}, error});
            });
}

void OneShotVlmChannel::enqueue(const QString& systemPrompt,
                                const QString& userText,
                                const QList<QImage>& frames,
                                Priority priority,
                                const QString& cancellationKey,
                                std::function<void(const QString&)> onDone)
{
    enqueueDetailed(systemPrompt, userText, frames, priority, cancellationKey,
                    [onDone](ModelReply reply) { if (onDone) onDone(reply.error.isEmpty() ? reply.content : QString()); });
}

void OneShotVlmChannel::enqueueDetailed(const QString& systemPrompt, const QString& userText,
                                        const QList<QImage>& frames, Priority priority,
                                        const QString& cancellationKey, std::function<void(ModelReply)> onDone)
{
    if (!m_agent) {
        qWarning() << "[OneShotVlmChannel] Agent 为空，无法处理请求";
        if (onDone) onDone({{}, QStringLiteral("模型通道未初始化")});
        return;
    }

    Request request;
    request.systemPrompt = systemPrompt;
    request.userText = userText;
    request.frames = frames;
    request.priority = priority;
    request.cancellationKey = cancellationKey;
    request.conversationId = QStringLiteral("__vlm_oneshot_")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.onDone = std::move(onDone);

    if (priority == Priority::Interactive) {
        auto position = m_pending.begin();
        while (position != m_pending.end() && position->priority == Priority::Interactive) {
            ++position;
        }
        m_pending.insert(position, std::move(request));
    } else {
        m_pending.append(std::move(request));
    }
    
    qDebug() << "[OneShotVlmChannel] 加入队列, 优先级:" << (priority == Priority::Interactive ? "交互" : "后台")
             << "队列长度:" << m_pending.size()
             << "是否运行中:" << m_running;
    
    startNext();
}

void OneShotVlmChannel::cancelBackground(const QString& cancellationKey)
{
    if (cancellationKey.isEmpty()) return;

    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->priority == Priority::Background && it->cancellationKey == cancellationKey) {
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    if (m_running && m_active.priority == Priority::Background
        && m_active.cancellationKey == cancellationKey) {
        m_active.discardResult = true;
        m_agent->abortRequest(m_active.conversationId, QStringLiteral("cancelled: 构建已取消"));
    }
}

void OneShotVlmChannel::startNext()
{
    if (m_running || m_pending.isEmpty() || !m_agent) return;

    m_active = m_pending.takeFirst();
    m_running = true;
    qDebug() << "[OneShotVlmChannel] 开始处理 VLM 请求, convId:" << m_active.conversationId 
             << "帧数:" << m_active.frames.size() 
             << "优先级:" << (m_active.priority == Priority::Interactive ? "交互" : "后台");
    const QString conversationId = m_active.conversationId;
    const QString system = m_active.systemPrompt, text = m_active.userText;
    const auto frames = m_active.frames;
    // Capture identity before send: configuration errors may complete synchronously.
    QTimer::singleShot(m_requestTimeoutMs, this, [this, conversationId] {
        if (!m_running || m_active.conversationId != conversationId) return;
        m_agent->abortRequest(conversationId, QStringLiteral("timeout: 模型请求超过%1毫秒").arg(m_requestTimeoutMs));
    });
    m_agent->sendOneShot(conversationId, system, text, frames);
}

void OneShotVlmChannel::finishActive(ModelReply reply)
{
    if (!m_running) return;

    qDebug() << "[OneShotVlmChannel] 完成 VLM 请求, convId:" << m_active.conversationId 
             << "内容长度:" << reply.content.length()
             << "是否丢弃:" << m_active.discardResult
             << "队列剩余:" << m_pending.size();

    Request completed = std::move(m_active);
    m_active = {};
    m_running = false;
    if (m_agent) m_agent->clearHistory(completed.conversationId);
    if (!completed.discardResult && completed.onDone) {
        qDebug() << "[OneShotVlmChannel] 调用完成回调";
        QTimer::singleShot(0, this, [done = std::move(completed.onDone), reply = std::move(reply)] { done(reply); });
    }
    startNext();
}
