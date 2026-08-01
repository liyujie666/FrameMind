#ifndef FRAMEMIND_CHATVIEWMODEL_H
#define FRAMEMIND_CHATVIEWMODEL_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QList>
#include <QElapsedTimer>

#include "model/conversation.h"
#include "model/chatmessage.h"
#include "model/videocontext.h"

class AgentService;
class ConversationService;
class EventBus;
class PlayerViewModel;
class VideoAgent;
class VideoAnalysisService;
class ChatMessageListModel;
class QTimer;

/**
 * 对话视图模型（M2-T5/T7/T8）。
 *
 * 通信原则：不直接持有 PlayerViewModel，跨 VM 通信走 EventBus
 * （时间戳跳转 → seekToPosition；📷 当前帧 → frameForAIRequested / screenshotForAI）。
 */
class ChatViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isStreaming READ isStreaming NOTIFY streamingChanged)
    Q_PROPERTY(bool isCollapsed READ isCollapsed WRITE setCollapsed NOTIFY collapsedChanged)
    Q_PROPERTY(QString currentConversationId READ currentConversationId NOTIFY conversationChanged)

public:
    explicit ChatViewModel(AgentService* agentService,
                           ConversationService* convService,
                           EventBus* eventBus,
                           QObject* parent = nullptr);

    ChatMessageListModel* messageModel() const { return m_messageModel; }
    void setPlayerViewModel(PlayerViewModel* playerVM);
    void setVideoAgent(VideoAgent* agent);
    void setVideoAnalysisService(VideoAnalysisService* vas);
    VideoContext getVideoContext() const;
    bool isStreaming() const { return m_streaming; }
    bool isCollapsed() const { return m_collapsed; }
    QString currentConversationId() const { return m_currentConversationId; }

    QList<Conversation> conversations() const;

public slots:
    void sendMessage(const QString& text);
    void sendMessageWithFrame(const QString& text, const QImage& frame);
    void sendMessageWithCurrentFrame(const QString& text);  // 📷：经 EventBus 取当前帧
    void stopGeneration();
    void regenerateLastResponse();
    void setCollapsed(bool collapsed);

    void createNewConversation();
    void switchConversation(const QString& convId);
    void deleteConversation(const QString& convId);

    /// 视频打开时由 MainWindow 或 PlayerViewModel 触发，启动 RAG 索引
    void onVideoOpened(const QString& videoPath);

    void onTimestampClicked(int64_t posMs);

signals:
    void streamingChanged(bool streaming);
    void collapsedChanged(bool collapsed);
    void conversationChanged(const QString& convId);
    void conversationsChanged();
    void messageAppended(int index);
    void messageUpdated(int index);
    void errorOccurred(const QString& msg);

private:
    void connectAgent();
    void ensureConversation();
    void doSend(const QString& text, const QList<QImage>& frames);
    void onScreenshotForAI(const QImage& frame, int64_t ts);

    AgentService*         m_agentService = nullptr;
    ConversationService*  m_convService = nullptr;
    EventBus*             m_eventBus = nullptr;
    PlayerViewModel*      m_playerVM = nullptr;
    VideoAgent*           m_videoAgent = nullptr;
    VideoAnalysisService* m_videoAnalysis = nullptr;
    ChatMessageListModel* m_messageModel = nullptr;

    QString m_activeVideoPath;
    QString m_indexingPath;    // 已触发索引的路径，避免 durationChanged 重复触发

    bool    m_streaming = false;
    bool    m_videoAgentStreaming = false;  // 视频 Agent 路径：屏蔽裸 responseChunk 重复追加
    bool    m_collapsed = false;
    QString m_currentConversationId;

    int     m_assistantRow = -1;   // 当前流式 assistant 行
    QString m_assistantId;

    // 流式 UI 节流
    QTimer*       m_flushTimer = nullptr;
    bool          m_dirty = false;

    // 📷 当前帧待发
    bool    m_awaitingFrame = false;
    QString m_pendingFrameText;
};

#endif // FRAMEMIND_CHATVIEWMODEL_H
