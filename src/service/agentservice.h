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
class LLMProviderService;
class ToolRegistry;

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
                          LLMProviderService* providers,
                          QObject* parent = nullptr);

    void sendMessage(const QString& conversationId,
                     const QString& text,
                     const QList<QImage>& frames = {},
                     const VideoContext& videoCtx = {});

    /**
     * 带 Tool Calling 的发送（M4）。
     *
     * 与 sendMessage 差异：
     *   - 请求体中携带 tools 字段
     *   - 使用 NetworkClient::streamPostRaw，可解析 delta.tool_calls
     *   - 结束时通过 responseFinishedWithTools 传出 tool_calls 与 finish_reason
     *
     * 后续工具执行 / 循环调度由 ToolOrchestrator 负责。
     *
     * @param tools JSON 数组（ToolRegistry::allDefinitions() 输出）
     */
    void sendMessageWithTools(const QString& conversationId,
                                const QString& text,
                                const QList<QImage>& frames,
                                const VideoContext& videoCtx,
                                const QJsonArray& tools);

    /**
     * 把已有的 tool 结果作为 role=tool 消息回填并继续下一轮 LLM 请求
     * （不新增 user 消息）。
     * @param toolMessages 一组 { tool_call_id, name, content } 对象
     * @param tools        本轮仍需携带的 tools 定义
     */
    void continueWithToolResults(const QString& conversationId,
                                   const QJsonArray& assistantToolCallMsg,
                                   const QJsonArray& toolMessages,
                                   const QJsonArray& tools);

    void stopGeneration();
    void setModel(const QString& modelName);
    void setEndpoint(const QString& endpoint);

    /// 用历史消息重建某会话的上下文（切换会话/启动后调用）
    void seedHistory(const QString& conversationId,
                     const QList<ChatMessage>& messages);
    void clearHistory(const QString& conversationId);

    /// 切换视频上下文（供 Orchestrator 更新 system prompt）
    void setActiveVideoContext(const VideoContext& ctx) { m_activeCtx = ctx; }
    VideoContext activeVideoContext() const { return m_activeCtx; }

signals:
    void responseChunk(const QString& convId, const QString& delta);
    void responseFinished(const QString& convId, const ChatMessage& fullMsg);
    void responseError(const QString& convId, const QString& error);

    /**
     * Tool Calling 版本的结束信号（M4）。
     * @param toolCalls   本轮 LLM 请求的工具调用列表（QJsonArray of ToolCall）
     * @param finishReason "stop" | "tool_calls" | "length"
     * @param textContent  纯文本累积（若模型直接回复也有值）
     */
    void responseFinishedWithTools(const QString& convId,
                                    const QJsonArray& toolCalls,
                                    const QString& finishReason,
                                    const QString& textContent);

private:
    QJsonObject buildRequestPayload(const QString& convId,
                                    const QString& text,
                                    const QList<QImage>& frames,
                                    const VideoContext& videoCtx);
    static QString buildSystemPrompt(const VideoContext& ctx);
    static QJsonObject makeUserMessage(const QString& text,
                                       const QList<QImage>& frames);

    void applyActiveProvider();

    NetworkClient*       m_network = nullptr;
    SettingsService*     m_settings = nullptr;
    LLMProviderService*  m_providers = nullptr;
    QString              m_model;
    QString              m_endpoint;
    QString              m_apiKey;

    // 每会话历史（不含 system；元素为 OpenAI message 对象）
    QHash<QString, QJsonArray> m_histories;

    // 当前进行中的流
    QString m_currentConvId;
    QString m_accumulated;
    bool    m_streaming = false;

    // 当前活跃视频上下文（会用于 system prompt）
    VideoContext m_activeCtx;

    // Tool Calling 状态（sendMessageWithTools 使用）
    // toolCalls[index] = { id, name, arguments(拼接后) }
    QJsonArray m_pendingToolCalls;
    QString    m_pendingFinishReason;

    // 内部：Tool Calling 版本的流式发起
    void sendStreamWithTools(const QString& convId,
                              const QJsonObject& payload);
};

#endif // FRAMEMIND_AGENTSERVICE_H
