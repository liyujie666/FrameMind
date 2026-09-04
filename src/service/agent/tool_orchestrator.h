#ifndef FRAMEMIND_TOOL_ORCHESTRATOR_H
#define FRAMEMIND_TOOL_ORCHESTRATOR_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

#include "model/tool_types.h"
#include "model/videocontext.h"

class ToolRegistry;
class AgentService;

/**
 * Tool 编排器：管理 Agent 多步 Tool Calling 循环（agent-core-design.md §7.2）。
 *
 * 循环协议（与 api-protocol.md §3.4 对齐）：
 *   round 0: 用户消息 → sendMessage with tools
 *            LLM 或直接回复（finish_reason=stop）
 *            或请求工具调用（finish_reason=tool_calls）
 *   round N: 上一轮 tool 结果作为 role=tool 消息回填 → 再次发送
 *   最多 MAX_ROUNDS 轮，超过则强制生成最终回答
 *
 * 与 AgentService 的关系：
 *   - Orchestrator 不直接调用 LLM，转发到 AgentService::sendMessageWithTools
 *   - 每轮 SSE 完成后由 AgentService 通知 orchestrator
 *   - Orchestrator 判断是否继续工具循环
 *
 * 使用方式：
 *   orchestrator->runQuery("conv-1", question, videoCtx, [](const AgentAnswer& a){...});
 */
class ToolOrchestrator : public QObject {
    Q_OBJECT
public:
    static constexpr int MAX_ROUNDS = 5;
    static constexpr int MAX_TOOL_CALLS_PER_ANSWER = 10;

    explicit ToolOrchestrator(AgentService* agent,
                               ToolRegistry* registry,
                               QObject* parent = nullptr);

    /**
     * 切换活跃视频：把 videoPath / videoId 分发给需要的 Tool
     * （SearchVideoContentTool / GetTranscriptTool / GetSceneInfoTool）。
     * 由 VideoAgent::setActiveVideo 调用。
     */
    void setActiveVideoContext(const QString& videoPath, const QString& videoId);

    /// 单次问答完整流程（含 Tool 循环）。
    /// - onProgress: 流式增量文本（供 UI 展示 LLM 最终回答的流式过程）
    /// - onDone:     完整 Answer
    /// - onError:    出错时
    void runQuery(const QString& conversationId,
                   const QString& question,
                   const QList<QImage>& userFrames,
                   const VideoContext& videoCtx,
                   std::function<void(const QString&)> onProgress,
                   std::function<void(const QString& answer,
                                       const QVector<ToolResult>& toolTrace,
                                       int rounds)> onDone,
                   std::function<void(const QString&)> onError,
                   const QJsonValue& toolChoice = QStringLiteral("auto"));

    void runPlayerCommand(const QString& question,
                          std::function<void(const QString&)> onDone,
                          std::function<void(const QString&)> onError);

    /// 中断当前 Agent 循环
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void toolCallStarted(const ToolCall& call);
    void toolCallFinished(const ToolResult& result);
    void roundStarted(int roundIndex);

private slots:
    void onAgentChunk(const QString& convId, const QString& delta);
    void onAgentFinished(const QString& convId,
                          const QJsonArray& toolCalls,
                          const QString& finishReason,
                          const QString& textContent);
    void onAgentError(const QString& convId, const QString& error);

private:
    void startRound(int round);
    void executeToolsThenContinue(const QVector<ToolCall>& calls, int round);
    QVector<ToolCall> fallbackPlayerCalls() const;
    void finishWithAnswer(const QString& answer);
    void abortWithError(const QString& err);

    QPointer<AgentService> m_agent;
    QPointer<ToolRegistry> m_registry;

    // 当前会话状态
    bool         m_running = false;
    QString      m_convId;
    QString      m_question;
    QList<QImage> m_userFrames;
    VideoContext m_videoCtx;

    int          m_currentRound = 0;
    int          m_totalToolCalls = 0;
    QString      m_streamingText;
    QVector<ToolResult> m_toolTrace;
    QJsonArray   m_lastAssistantToolCalls;
    QJsonArray   m_activeTools;
    QJsonValue   m_toolChoice = QJsonValue(QStringLiteral("auto"));

    // 回调
    std::function<void(const QString&)> m_onProgress;
    std::function<void(const QString&, const QVector<ToolResult>&, int)> m_onDone;
    std::function<void(const QString&)> m_onError;
};

#endif // FRAMEMIND_TOOL_ORCHESTRATOR_H
