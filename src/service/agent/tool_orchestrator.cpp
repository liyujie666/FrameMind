#include "service/agent/tool_orchestrator.h"

#include "service/agentservice.h"
#include "service/agent/tool_registry.h"
#include "service/agent/tools/search_video_content_tool.h"
#include "service/agent/tools/get_transcript_tool.h"
#include "service/agent/tools/get_scene_info_tool.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

ToolOrchestrator::ToolOrchestrator(AgentService* agent,
                                     ToolRegistry* registry,
                                     QObject* parent)
    : QObject(parent)
    , m_agent(agent)
    , m_registry(registry)
{
    if (m_agent) {
        connect(m_agent, &AgentService::responseChunk,
                this, &ToolOrchestrator::onAgentChunk);
        connect(m_agent, &AgentService::responseFinishedWithTools,
                this, &ToolOrchestrator::onAgentFinished);
        connect(m_agent, &AgentService::responseError,
                this, &ToolOrchestrator::onAgentError);
    }
}

// ============================================================
// 主入口
// ============================================================

void ToolOrchestrator::runQuery(
    const QString& conversationId,
    const QString& question,
    const QList<QImage>& userFrames,
    const VideoContext& videoCtx,
    std::function<void(const QString&)> onProgress,
    std::function<void(const QString&, const QVector<ToolResult>&, int)> onDone,
    std::function<void(const QString&)> onError,
    const QJsonValue& toolChoice)
{
    if (m_running) {
        if (onError) onError(QStringLiteral("Agent 正在执行，请稍后"));
        return;
    }
    if (!m_agent || !m_registry) {
        if (onError) onError(QStringLiteral("Agent 或 ToolRegistry 未注入"));
        return;
    }

    m_running    = true;
    m_convId     = conversationId;
    m_question   = question;
    m_userFrames = userFrames;
    m_videoCtx   = videoCtx;
    m_toolChoice = toolChoice;
    m_currentRound = 0;
    m_totalToolCalls = 0;
    m_streamingText.clear();
    m_toolTrace.clear();

    m_onProgress = std::move(onProgress);
    m_onDone     = std::move(onDone);
    m_onError    = std::move(onError);

    startRound(0);
}

void ToolOrchestrator::cancel()
{
    m_running = false;
    if (m_agent) m_agent->stopGeneration();
}

void ToolOrchestrator::setActiveVideoContext(const QString& videoPath,
                                                const QString& videoId)
{
    if (!m_registry) return;
    // 分发给需要 videoId / videoPath 的 Tool
    if (auto* t = dynamic_cast<SearchVideoContentTool*>(
            m_registry->getTool(QStringLiteral("search_video_content")))) {
        t->setVideoId(videoId);
    }
    if (auto* t = dynamic_cast<GetTranscriptTool*>(
            m_registry->getTool(QStringLiteral("get_transcript")))) {
        t->setVideoId(videoId);
    }
    if (auto* t = dynamic_cast<GetSceneInfoTool*>(
            m_registry->getTool(QStringLiteral("get_scene_info")))) {
        t->setVideoPath(videoPath);
    }
}

// ============================================================
// 轮次控制
// ============================================================

void ToolOrchestrator::startRound(int round)
{
    m_currentRound = round;
    m_streamingText.clear();
    emit roundStarted(round);

    if (round == 0) {
        m_agent->sendMessageWithTools(m_convId, m_question, m_userFrames,
                                        m_videoCtx, m_registry->allDefinitions(),
                                        m_toolChoice);
    }
    // 后续轮次由 executeToolsThenContinue 触发（在 onAgentFinished 中）
}

void ToolOrchestrator::onAgentChunk(const QString& convId, const QString& delta)
{
    if (convId != m_convId) return;
    m_streamingText += delta;
    if (m_onProgress) m_onProgress(delta);
}

void ToolOrchestrator::onAgentFinished(const QString& convId,
                                         const QJsonArray& toolCalls,
                                         const QString& finishReason,
                                         const QString& textContent)
{
    if (convId != m_convId || !m_running) return;

    // 情况 1: LLM 直接生成回答（stop / length）
    if (finishReason == QLatin1String("stop") || finishReason == QLatin1String("length")
        || toolCalls.isEmpty()) {
        finishWithAnswer(textContent.isEmpty() ? m_streamingText : textContent);
        return;
    }

    // 情况 2: 请求工具调用（tool_calls）
    if (finishReason == QLatin1String("tool_calls") || !toolCalls.isEmpty()) {
        // 缓存本轮 assistant tool_calls 消息，供回填下一轮 continueWithToolResults
        m_lastAssistantToolCalls = toolCalls;

        // 解析 ToolCall
        QVector<ToolCall> calls;
        for (const auto& v : toolCalls) {
            const QJsonObject o = v.toObject();
            ToolCall c;
            c.id   = o.value(QStringLiteral("id")).toString();
            const QJsonObject fn = o.value(QStringLiteral("function")).toObject();
            c.name = fn.value(QStringLiteral("name")).toString();
            const QString argsStr = fn.value(QStringLiteral("arguments")).toString();
            c.arguments = QJsonDocument::fromJson(argsStr.toUtf8()).object();
            if (c.isValid()) calls.append(c);
        }
        if (calls.isEmpty()) {
            finishWithAnswer(m_streamingText);
            return;
        }

        // 限流：单次回答工具数上限
        if (m_totalToolCalls + calls.size() > MAX_TOOL_CALLS_PER_ANSWER) {
            const int allow = MAX_TOOL_CALLS_PER_ANSWER - m_totalToolCalls;
            if (allow <= 0) {
                // 已达上限，强制结束
                finishWithAnswer(m_streamingText.isEmpty()
                                     ? QStringLiteral("[已达工具调用上限，基于现有信息回答] ")
                                       + m_streamingText
                                     : m_streamingText);
                return;
            }
            calls.resize(allow);
        }
        m_totalToolCalls += calls.size();

        // 保存 assistant tool_call 消息给下一轮回填
        QJsonObject assistantMsg;
        assistantMsg.insert(QStringLiteral("role"), QStringLiteral("assistant"));
        assistantMsg.insert(QStringLiteral("content"), QJsonValue::Null);
        assistantMsg.insert(QStringLiteral("tool_calls"), toolCalls);

        executeToolsThenContinue(calls, m_currentRound);
        return;
    }

    // 其它情况：结束
    finishWithAnswer(textContent.isEmpty() ? m_streamingText : textContent);
}

void ToolOrchestrator::onAgentError(const QString& convId, const QString& error)
{
    if (convId != m_convId || !m_running) return;
    abortWithError(error);
}

// ============================================================
// Tool 执行 & 回填下一轮
// ============================================================

void ToolOrchestrator::executeToolsThenContinue(
    const QVector<ToolCall>& calls, int round)
{
    // 顺序执行（简化；后续可并行独立工具）
    auto pendingResults = QSharedPointer<QJsonArray>::create();
    auto executed       = QSharedPointer<int>::create(0);
    const int total     = calls.size();

    for (const ToolCall& c : calls) {
        emit toolCallStarted(c);
        ITool* tool = m_registry->getTool(c.name);
        if (!tool) {
            ToolResult r = ToolResult::fail(c.id, c.name,
                                            QStringLiteral("Tool 未注册: %1").arg(c.name));
            m_toolTrace.append(r);
            pendingResults->append(QJsonObject{
                { QStringLiteral("role"), QStringLiteral("tool") },
                { QStringLiteral("tool_call_id"), c.id },
                { QStringLiteral("name"), c.name },
                { QStringLiteral("content"),
                  QString::fromUtf8(QJsonDocument(QJsonObject{
                      { QStringLiteral("error"), r.error } }).toJson(QJsonDocument::Compact)) } });
            emit toolCallFinished(r);
            ++(*executed);
            continue;
        }

        // 记录 assistant tool_calls 消息用于下一轮回填
        tool->executeAsync(c.id, c.arguments,
            [this, c, pendingResults, executed, total, round](const ToolResult& result) {
                m_toolTrace.append(result);
                emit toolCallFinished(result);

                // 组装 tool 消息
                QJsonObject toolMsg;
                toolMsg.insert(QStringLiteral("role"), QStringLiteral("tool"));
                toolMsg.insert(QStringLiteral("tool_call_id"), c.id);
                toolMsg.insert(QStringLiteral("name"), c.name);
                const QJsonDocument payload(result.success ? result.data
                                              : QJsonObject{{ QStringLiteral("error"), result.error }});
                toolMsg.insert(QStringLiteral("content"),
                                QString::fromUtf8(payload.toJson(QJsonDocument::Compact)));
                pendingResults->append(toolMsg);
                ++(*executed);

                if (*executed >= total) {
                    if (round + 1 >= MAX_ROUNDS) {
                        finishWithAnswer(
                            QStringLiteral("[已达最大工具轮次] ") + m_streamingText);
                        return;
                    }
                    // OpenAI API 要求 tool 消息之前必须有对应的 assistant tool_calls 消息，
                    // 否则会返回 400。使用缓存的 m_lastAssistantToolCalls 回填。
                    QJsonArray assistantMsg;
                    if (!m_lastAssistantToolCalls.isEmpty()) {
                        QJsonObject assistantEntry;
                        assistantEntry.insert(QStringLiteral("role"),
                                              QStringLiteral("assistant"));
                        assistantEntry.insert(QStringLiteral("content"), QJsonValue::Null);
                        assistantEntry.insert(QStringLiteral("tool_calls"),
                                              m_lastAssistantToolCalls);
                        assistantMsg.append(assistantEntry);
                    }
                    m_currentRound = round + 1;
                    emit roundStarted(m_currentRound);
                    m_streamingText.clear();
                    m_agent->continueWithToolResults(
                        m_convId, assistantMsg, *pendingResults,
                        m_registry->allDefinitions());
                }
            });
    }
}

void ToolOrchestrator::finishWithAnswer(const QString& answer)
{
    m_running = false;
    if (m_onDone) m_onDone(answer, m_toolTrace, m_currentRound + 1);
}

void ToolOrchestrator::abortWithError(const QString& err)
{
    m_running = false;
    if (m_onError) m_onError(err);
}
