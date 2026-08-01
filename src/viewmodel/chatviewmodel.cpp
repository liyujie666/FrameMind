#include "viewmodel/chatviewmodel.h"

#include "viewmodel/chatmessagelistmodel.h"
#include "viewmodel/playerviewmodel.h"
#include "service/agentservice.h"
#include "service/conversationservice.h"
#include "service/agent/video_agent.h"
#include "service/agent/video_analysis_service.h"
#include "service/agent/video_indexer.h"
#include "infrastructure/eventbus.h"
#include "model/videocontext.h"

#include <QTimer>
#include <QUuid>
#include <QDateTime>
#include <QFileInfo>
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
                if (m_videoAgentStreaming) return;
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

void ChatViewModel::setPlayerViewModel(PlayerViewModel* playerVM)
{
    m_playerVM = playerVM;

    if (!m_playerVM) return;

    // 等播放器 SDK 真正打开成功后再启动索引（此时 filePath 已填好）
    connect(m_playerVM, &PlayerViewModel::videoOpened,
            this, [this](const QString& filePath) {
        if (filePath.isEmpty()) return;
        
        // 视频切换时，触发对话切换和索引
        const bool isNewVideo = (filePath != m_activeVideoPath);
        
        if (isNewVideo) {
            m_activeVideoPath = filePath;
            m_indexingPath.clear();  // 清空索引路径，允许新视频触发索引
            
            // 切换到该视频对应的对话
            switchToVideoConversation(filePath);
        }
        
        // 只有在尚未索引过这个视频时才启动索引
        if (filePath != m_indexingPath) {
            m_indexingPath = filePath;
            
            if (m_videoAgent) {
                const QString videoId = VideoIndexer::computeVideoId(filePath);
                m_videoAgent->setActiveVideo(filePath, videoId);
            }
            if (m_videoAnalysis) {
                m_videoAnalysis->onVideoOpened(filePath);
            }

            qDebug() << "[ChatViewModel] 视频就绪，启动 RAG 索引:" << filePath;
        } else {
            qDebug() << "[ChatViewModel] 视频已索引，跳过重复索引:" << filePath;
        }
    });
}

void ChatViewModel::setVideoAgent(VideoAgent* agent)
{
    m_videoAgent = agent;
}

void ChatViewModel::setVideoAnalysisService(VideoAnalysisService* vas)
{
    m_videoAnalysis = vas;
}

void ChatViewModel::onVideoOpened(const QString& videoPath)
{
    if (videoPath.isEmpty()) return;
    
    // 视频切换时，自动切换到该视频对应的会话
    if (videoPath != m_activeVideoPath) {
        m_indexingPath.clear();
        m_activeVideoPath = videoPath;
        
        // 查找或创建该视频对应的会话
        switchToVideoConversation(videoPath);
    }
}

QString ChatViewModel::findConversationForVideo(const QString& videoPath) const
{
    if (!m_convService || videoPath.isEmpty()) return {};
    
    const auto convs = m_convService->getAllConversations();
    for (const auto& conv : convs) {
        if (QFileInfo(conv.videoFilePath).canonicalFilePath() == 
            QFileInfo(videoPath).canonicalFilePath()) {
            return conv.id;
        }
    }
    return {};
}

void ChatViewModel::switchToVideoConversation(const QString& videoPath)
{
    if (!m_convService) return;
    
    // 查找该视频已有的会话
    QString convId = findConversationForVideo(videoPath);
    
    if (convId.isEmpty()) {
        // 没有找到，创建新会话并绑定视频
        const Conversation c = m_convService->createConversation(videoPath);
        convId = c.id;
        qDebug() << "[ChatViewModel] 为视频创建新会话 | videoPath:" << videoPath 
                 << "| convId:" << convId;
    } else {
        qDebug() << "[ChatViewModel] 切换到视频已有会话 | videoPath:" << videoPath 
                 << "| convId:" << convId;
    }
    
    // 切换到该会话
    if (convId != m_currentConversationId) {
        switchConversation(convId);
    }
}

VideoContext ChatViewModel::getVideoContext() const
{
    // 优先从 VideoAnalysisService 构建，包含已生成的摘要和场景概览
    if (m_videoAnalysis && !m_activeVideoPath.isEmpty()) {
        auto repr = m_videoAnalysis->representation(m_activeVideoPath);
        if (repr && repr->isValid()) {
            return m_videoAnalysis->buildVideoContext(repr);
        }
    }

    // 降级：repr 尚未就绪时只提供基础元信息
    VideoContext ctx;
    if (!m_playerVM) return ctx;
    ctx.fileName  = m_playerVM->mediaTitle();
    ctx.durationMs = m_playerVM->duration();
    return ctx;
}

void ChatViewModel::ensureConversation()
{
    if (!m_currentConversationId.isEmpty()) return;
    if (!m_convService) {
        m_currentConversationId = newId();
        return;
    }
    const Conversation c = m_convService->createConversation(m_activeVideoPath);
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

    VideoContext ctx = getVideoContext();
    const int64_t currentPos = m_playerVM ? m_playerVM->position() : 0;

    if (m_videoAgent && !m_activeVideoPath.isEmpty()) {
        // VideoAgent 五阶段路径：RAG 检索 + Tool Calling
        m_videoAgentStreaming = true;
        m_videoAgent->ask(
            m_currentConversationId,
            text,
            frames,
            ctx,
            currentPos,
            [this](const QString& delta) {
                // onProgress：流式 delta，与 AgentService::responseChunk 等效
                m_messageModel->appendDeltaSilent(m_assistantRow, delta);
                m_dirty = true;
            },
            [this](const AgentAnswer& answer) {
                // onDone
                m_videoAgentStreaming = false;
                m_flushTimer->stop();
                m_messageModel->updateContent(m_assistantRow, answer.answer);
                m_messageModel->setStreaming(m_assistantRow, false);
                emit messageUpdated(m_assistantRow);

                if (m_convService) {
                    ChatMessage saved;
                    saved.id = m_assistantId;
                    saved.role = ChatMessage::Assistant;
                    saved.content = answer.answer;
                    saved.timestamp = QDateTime::currentDateTime();
                    m_convService->saveMessage(m_currentConversationId, saved);
                }
                m_assistantRow = -1;
                m_streaming = false;
                emit streamingChanged(false);
                emit conversationsChanged();
            },
            [this](const QString& err) {
                // onError
                m_videoAgentStreaming = false;
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
    } else if (m_agentService) {
        // 降级路径：未打开视频时走裸 AgentService（纯文字对话）
        m_agentService->sendMessage(m_currentConversationId, text, frames, ctx);
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

void ChatViewModel::requestCurrentFrame()
{
    // 请求当前帧用于预览
    m_awaitingFrameForPreview = true;
    if (m_eventBus) {
        m_eventBus->requestFrameForAI(-1);  // -1：使用当前帧
    }
}

void ChatViewModel::sendMessageWithCachedFrame(const QString& text)
{
    if (m_streaming) return;
    if (m_cachedFrames.isEmpty()) {
        // 如果没有缓存帧，只发送文本
        doSend(text, {});
    } else {
        // 使用缓存的所有帧发送
        doSend(text, m_cachedFrames);
        // 清除缓存
        m_cachedFrames.clear();
    }
}

void ChatViewModel::onScreenshotForAI(const QImage& frame, int64_t ts)
{
    // 处理帧预览请求
    if (m_awaitingFrameForPreview) {
        m_awaitingFrameForPreview = false;
        if (frame.isNull()) {
            qWarning() << "[ChatViewModel] Frame is null for preview";
            emit errorOccurred(tr("未获取到当前帧"));
        } else {
            // 添加到缓存帧列表并通知UI显示预览
            m_cachedFrames.append(frame);
            emit currentFrameReady(frame, ts);
        }
        return;
    }
    
    // 处理直接发送请求
    if (m_awaitingFrame) {
        m_awaitingFrame = false;
        const QString text = m_pendingFrameText;
        m_pendingFrameText.clear();
        if (frame.isNull()) {
            qWarning() << "[ChatViewModel] Frame is null, sending text only";
            emit errorOccurred(tr("未获取到当前帧，已仅按文字提问"));
            doSend(text, {});
        } else {
            doSend(text, { frame });
        }
    }
}

void ChatViewModel::stopGeneration()
{
    if (!m_streaming) return;
    if (m_videoAgentStreaming && m_videoAgent) {
        m_videoAgent->cancel();
        m_videoAgentStreaming = false;
        m_flushTimer->stop();
        if (m_assistantRow >= 0) {
            m_messageModel->setStreaming(m_assistantRow, false);
            emit messageUpdated(m_assistantRow);
            if (m_convService) {
                ChatMessage saved;
                saved.id = m_assistantId;
                saved.role = ChatMessage::Assistant;
                saved.content = m_messageModel->messageAt(m_assistantRow).content;
                saved.timestamp = QDateTime::currentDateTime();
                m_convService->saveMessage(m_currentConversationId, saved);
            }
            m_assistantRow = -1;
        }
        m_streaming = false;
        emit streamingChanged(false);
        return;
    }
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
    const Conversation c = m_convService->createConversation(m_activeVideoPath);
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
