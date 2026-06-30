#include "viewmodel/chatviewmodel.h"

#include "viewmodel/chatmessagelistmodel.h"
#include "service/agentservice.h"
#include "service/conversationservice.h"
#include "infrastructure/eventbus.h"

#include <QTimer>
#include <QUuid>
#include <QDateTime>

namespace {
constexpr int kFlushIntervalMs = 40;  // ~25fps 流式刷新节流
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
}

ChatViewModel::ChatViewModel(AgentService* agentService,
                             ConversationService* convService,
                             EventBus* eventBus,
                             QObject* parent)
    : QObject(parent)
    , m_agentService(agentService)
    , m_convService(convService)
    , m_eventBus(eventBus)
    , m_messageModel(new ChatMessageListModel(this))
{
    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this]() {
        if (m_dirty && m_assistantRow >= 0) {
            m_dirty = false;
            m_messageModel->flushRow(m_assistantRow);  // 节流刷新视图
            emit messageUpdated(m_assistantRow);
        }
    });

    connectAgent();

    if (m_eventBus) {
        connect(m_eventBus, &EventBus::screenshotForAI,
                this, &ChatViewModel::onScreenshotForAI);
    }

    // 启动时载入最近会话（若有）
    if (m_convService) {
        const auto convs = m_convService->getAllConversations();
        if (!convs.isEmpty()) {
            switchConversation(convs.first().id);
        }
    }
}

void ChatViewModel::connectAgent()
{
    if (!m_agentService) return;

    connect(m_agentService, &AgentService::responseChunk, this,
            [this](const QString& convId, const QString& delta) {
                if (convId != m_currentConversationId || m_assistantRow < 0) return;
                m_messageModel->appendDeltaSilent(m_assistantRow, delta);
                m_dirty = true;
            });

    connect(m_agentService, &AgentService::responseFinished, this,
            [this](const QString& convId, const ChatMessage& full) {
                if (convId != m_currentConversationId || m_assistantRow < 0) return;
                m_flushTimer->stop();
                m_messageModel->updateContent(m_assistantRow, full.content);
                m_messageModel->setStreaming(m_assistantRow, false);
                emit messageUpdated(m_assistantRow);

                // 落库（用占位时生成的 id 持久化）
                if (m_convService) {
                    ChatMessage saved = full;
                    saved.id = m_assistantId;
                    saved.role = ChatMessage::Assistant;
                    m_convService->saveMessage(m_currentConversationId, saved);
                }
                m_assistantRow = -1;
                m_streaming = false;
                emit streamingChanged(false);
                emit conversationsChanged();
            });

    connect(m_agentService, &AgentService::responseError, this,
            [this](const QString& convId, const QString& err) {
                if (convId != m_currentConversationId) return;
                m_flushTimer->stop();
                if (m_assistantRow >= 0) {
                    m_messageModel->updateContent(
                        m_assistantRow,
                        QStringLiteral("⚠️ 生成失败：%1").arg(err));
                    m_messageModel->setStreaming(m_assistantRow, false);
                    emit messageUpdated(m_assistantRow);
                    m_assistantRow = -1;
                }
                m_streaming = false;
                emit streamingChanged(false);
                emit errorOccurred(err);
            });
}

QList<Conversation> ChatViewModel::conversations() const
{
    return m_convService ? m_convService->getAllConversations()
                         : QList<Conversation>{};
}

void ChatViewModel::ensureConversation()
{
    if (!m_currentConversationId.isEmpty()) return;
    if (!m_convService) {
        m_currentConversationId = newId();
        return;
    }
    const Conversation c = m_convService->createConversation();
    m_currentConversationId = c.id;
    if (m_agentService) m_agentService->clearHistory(c.id);
    emit conversationChanged(m_currentConversationId);
    emit conversationsChanged();
}

void ChatViewModel::doSend(const QString& text, const QList<QImage>& frames)
{
    if (m_streaming) return;
    if (text.trimmed().isEmpty() && frames.isEmpty()) return;

    ensureConversation();

    // 1. 用户消息
    ChatMessage userMsg;
    userMsg.id = newId();
    userMsg.role = ChatMessage::User;
    userMsg.content = text;
    userMsg.attachedFrames = frames;
    userMsg.timestamp = QDateTime::currentDateTime();
    const int userRow = m_messageModel->appendMessage(userMsg);
    emit messageAppended(userRow);
    if (m_convService) m_convService->saveMessage(m_currentConversationId, userMsg);

    // 首条用户消息用作会话标题
    if (m_convService && userRow == 0 && !text.trimmed().isEmpty()) {
        m_convService->updateTitle(m_currentConversationId,
                                   text.left(20));
        emit conversationsChanged();
    }

    // 2. assistant 占位
    ChatMessage assistant;
    assistant.id = newId();
    assistant.role = ChatMessage::Assistant;
    assistant.isStreaming = true;
    assistant.timestamp = QDateTime::currentDateTime();
    m_assistantId = assistant.id;
    m_assistantRow = m_messageModel->appendMessage(assistant);
    emit messageAppended(m_assistantRow);

    // 3. 发起请求
    m_streaming = true;
    m_dirty = false;
    m_flushTimer->start();
    emit streamingChanged(true);

    if (m_agentService) {
        m_agentService->sendMessage(m_currentConversationId, text, frames);
    }
}

void ChatViewModel::sendMessage(const QString& text)
{
    doSend(text, {});
}

void ChatViewModel::sendMessageWithFrame(const QString& text, const QImage& frame)
{
    QList<QImage> frames;
    if (!frame.isNull()) frames.append(frame);
    doSend(text, frames);
}

void ChatViewModel::sendMessageWithCurrentFrame(const QString& text)
{
    if (m_streaming) return;
    // 经 EventBus 向 PlayerVM 请求当前帧，回包在 onScreenshotForAI
    m_pendingFrameText = text;
    m_awaitingFrame = true;
    if (m_eventBus) {
        m_eventBus->requestFrameForAI(-1);  // -1：使用当前帧
    } else {
        m_awaitingFrame = false;
        doSend(text, {});
    }
}

void ChatViewModel::onScreenshotForAI(const QImage& frame, int64_t /*ts*/)
{
    if (!m_awaitingFrame) return;
    m_awaitingFrame = false;
    const QString text = m_pendingFrameText;
    m_pendingFrameText.clear();
    if (frame.isNull()) {
        emit errorOccurred(tr("未获取到当前帧，已仅按文字提问"));
        doSend(text, {});
    } else {
        doSend(text, { frame });
    }
}

void ChatViewModel::stopGeneration()
{
    if (!m_streaming) return;
    if (m_agentService) m_agentService->stopGeneration();
    // 最终化由 responseFinished 处理
}

void ChatViewModel::regenerateLastResponse()
{
    if (m_streaming) return;
    // 找最后一条用户消息，重新发送
    for (int i = m_messageModel->count() - 1; i >= 0; --i) {
        const ChatMessage m = m_messageModel->messageAt(i);
        if (m.role == ChatMessage::User) {
            doSend(m.content, m.attachedFrames);
            return;
        }
    }
}

void ChatViewModel::setCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed) return;
    m_collapsed = collapsed;
    emit collapsedChanged(collapsed);
}

void ChatViewModel::createNewConversation()
{
    if (!m_convService) return;
    const Conversation c = m_convService->createConversation();
    m_currentConversationId = c.id;
    m_assistantRow = -1;
    if (m_agentService) m_agentService->clearHistory(c.id);
    m_messageModel->clear();
    emit conversationChanged(m_currentConversationId);
    emit conversationsChanged();
}

void ChatViewModel::switchConversation(const QString& convId)
{
    if (convId.isEmpty() || convId == m_currentConversationId) return;
    m_currentConversationId = convId;
    m_assistantRow = -1;

    QList<ChatMessage> msgs;
    if (m_convService) msgs = m_convService->getMessages(convId);
    m_messageModel->setMessages(msgs);
    if (m_agentService) m_agentService->seedHistory(convId, msgs);

    emit conversationChanged(convId);
}

void ChatViewModel::deleteConversation(const QString& convId)
{
    if (!m_convService) return;
    m_convService->deleteConversation(convId);
    if (m_agentService) m_agentService->clearHistory(convId);

    if (convId == m_currentConversationId) {
        m_currentConversationId.clear();
        m_messageModel->clear();
        const auto convs = m_convService->getAllConversations();
        if (!convs.isEmpty()) {
            switchConversation(convs.first().id);
        } else {
            emit conversationChanged(QString());
        }
    }
    emit conversationsChanged();
}

void ChatViewModel::onTimestampClicked(int64_t posMs)
{
    if (m_eventBus) {
        m_eventBus->requestSeek(posMs);
    }
}
