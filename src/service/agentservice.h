#ifndef FRAMEMIND_AGENTSERVICE_H
#define FRAMEMIND_AGENTSERVICE_H

#include <QObject>
#include <QString>
#include <QList>
#include <QImage>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

#include "model/chatmessage.h"
#include "model/videocontext.h"

class NetworkClient;
class SettingsService;

/**
 * AI 智能体交互核心（M2：无 Tool 版本）。
 *
 * 负责把 text + frames + videoCtx 拼成 OpenAI Compatible 请求体，
 * 通过 NetworkClient 以 SSE 流式发出，并把增量/完成/错误广播为信号。
 *
 * 多轮上下文：内部按 conversationId 维护历史（user/assistant 轮次）。
 * 切换会话时由 ChatViewModel 调用 seedHistory 从 DB 重建上下文。
 *
 * 安全：API Key 从 SettingsService::secretGet 取，禁止 hardcode/落库/打印。
 */
class AgentService : public QObject {
    Q_OBJECT
public:
    explicit AgentService(NetworkClient* network,
                          SettingsService* settings,
                          QObject* parent = nullptr);

    void sendMessage(const QString& conversationId,
                     const QString& text,
                     const QList<QImage>& frames = {},
                     const VideoContext& videoCtx = {});

    void stopGeneration();
    void setModel(const QString& modelName);
    void setEndpoint(const QString& endpoint);

    /// 用历史消息重建某会话的上下文（切换会话/启动后调用）
    void seedHistory(const QString& conversationId,
                     const QList<ChatMessage>& messages);
    void clearHistory(const QString& conversationId);

signals:
    void responseChunk(const QString& convId, const QString& delta);
    void responseFinished(const QString& convId, const ChatMessage& fullMsg);
    void responseError(const QString& convId, const QString& error);

private:
    QJsonObject buildRequestPayload(const QString& convId,
                                    const QString& text,
                                    const QList<QImage>& frames,
                                    const VideoContext& videoCtx);
    static QString buildSystemPrompt(const VideoContext& ctx);
    static QJsonObject makeUserMessage(const QString& text,
                                       const QList<QImage>& frames);

    NetworkClient*   m_network = nullptr;
    SettingsService* m_settings = nullptr;
    QString          m_model;
    QString          m_endpoint;

    // 每会话历史（不含 system；元素为 OpenAI message 对象）
    QHash<QString, QJsonArray> m_histories;

    // 当前进行中的流
    QString m_currentConvId;
    QString m_accumulated;
    bool    m_streaming = false;
};

#endif // FRAMEMIND_AGENTSERVICE_H
