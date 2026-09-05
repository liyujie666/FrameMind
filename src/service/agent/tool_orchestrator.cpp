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
#include <QRegularExpression>
#include <QUuid>

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
    m_activeTools = m_registry->allDefinitions();
    if (toolChoice.isObject()) {
        const QString requiredName = toolChoice.toObject()
                                         .value(QStringLiteral("function")).toObject()
                                         .value(QStringLiteral("name")).toString();
        QJsonArray filteredTools;
        for (const QJsonValue& value : std::as_const(m_activeTools)) {
            const QJsonObject tool = value.toObject();
            if (tool.value(QStringLiteral("function")).toObject()
                    .value(QStringLiteral("name")).toString() == requiredName) {
                filteredTools.append(tool);
            }
        }
        if (filteredTools.isEmpty()) {
            m_running = false;
            if (onError) {
                onError(QStringLiteral("请求的工具未注册: %1").arg(requiredName));
            }
            return;
        }
        m_activeTools = filteredTools;
    }
    m_currentRound = 0;
    m_totalToolCalls = 0;
    m_streamingText.clear();
    m_toolTrace.clear();
    m_lastAssistantToolCalls = QJsonArray();

    m_onProgress = std::move(onProgress);
    m_onDone     = std::move(onDone);
    m_onError    = std::move(onError);

    startRound(0);
}

void ToolOrchestrator::runPlayerCommand(
    const QString& question,
    std::function<void(const QString&)> onDone,
    std::function<void(const QString&)> onError)
{
    if (m_running) {
        if (onError) onError(QStringLiteral("播放器命令正在执行"));
        return;
    }
    if (!m_registry) {
        if (onError) onError(QStringLiteral("ToolRegistry 未注入"));
        return;
    }
    m_running = true;
    m_convId.clear();
    m_question = question;
    m_toolTrace.clear();
    m_streamingText.clear();
    m_lastAssistantToolCalls = QJsonArray();
    m_toolChoice = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("control_player")}}}};
    m_onDone = [onDone](const QString& answer, const QVector<ToolResult>&, int) {
        if (onDone) onDone(answer);
    };
    m_onError = std::move(onError);
    const QVector<ToolCall> calls = fallbackPlayerCalls();
    if (calls.isEmpty()) {
        abortWithError(QStringLiteral("无法从播放器命令中解析目标"));
        return;
    }
    m_totalToolCalls = 1;
    executeToolsThenContinue(calls, 0);
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
    qDebug() << "[ToolOrchestrator] 开始请求模型"
             << "会话=" << m_convId << "轮次=" << round;
    emit roundStarted(round);

    if (round == 0) {
        m_agent->sendMessageWithTools(m_convId, m_question, m_userFrames,
                                        m_videoCtx, m_activeTools, m_toolChoice);
    }
    // 后续轮次由 executeToolsThenContinue 触发（在 onAgentFinished 中）
}

void ToolOrchestrator::onAgentChunk(const QString& convId, const QString& delta)
{
    if (convId != m_convId) return;
    m_streamingText += delta;
    if (m_onProgress) m_onProgress(delta);
}

QVector<ToolCall> ToolOrchestrator::fallbackPlayerCalls() const
{
    QVector<ToolCall> calls;
    const QString question = m_question.trimmed();
    ToolCall call;
    call.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    call.name = QStringLiteral("control_player");

    if (question.contains(QRegularExpression(QStringLiteral("暂停|pause"),
                                               QRegularExpression::CaseInsensitiveOption))) {
        call.arguments = QJsonObject{{QStringLiteral("action"), QStringLiteral("pause")}};
        calls.append(call);
        return calls;
    }
    if (question.contains(QRegularExpression(QStringLiteral("播放|play"),
                                               QRegularExpression::CaseInsensitiveOption))) {
        call.arguments = QJsonObject{{QStringLiteral("action"), QStringLiteral("play")}};
        calls.append(call);
        return calls;
    }

    static const QRegularExpression timeRe(
        QStringLiteral("(?:第\\s*)?(?:(\\d+(?:\\.\\d+)?)\\s*(?:分钟|分)\\s*)?"
                       "(\\d+(?:\\.\\d+)?)?\\s*(?:秒|s)") ,
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = timeRe.match(question);
    if (!match.hasMatch()) return calls;

    const double minutes = match.captured(1).toDouble();
    const double seconds = match.captured(2).isEmpty()
        ? 0.0 : match.captured(2).toDouble();
    const qint64 timestampMs = qRound64((minutes * 60.0 + seconds) * 1000.0);
    call.arguments = QJsonObject{
        {QStringLiteral("action"), QStringLiteral("seek")},
        {QStringLiteral("timestamp_ms"), timestampMs}};
    calls.append(call);
    return calls;
}
void ToolOrchestrator::onAgentFinished(const QString& convId,
                                         const QJsonArray& toolCalls,
                                         const QString& finishReason,
                                         const QString& textContent)
{
    if (convId != m_convId || !m_running) return;

    qDebug() << "[ToolOrchestrator] 收到模型响应"
             << "会话=" << convId
             << "轮次=" << m_currentRound
             << "finish_reason=" << finishReason
             << "tool_calls数量=" << toolCalls.size()
             << "文本内容长度=" << textContent.size();
    
    // 如果模型返回了文本但没有工具调用，打印文本内容前100字符用于诊断
    if (toolCalls.isEmpty() && !textContent.isEmpty()) {
        const QString preview = textContent.left(100);
        qDebug() << "[ToolOrchestrator] 模型返回了纯文本（前100字符）:" << preview;
    }

    // 只要服务端返回了工具调用，就必须优先执行工具；部分兼容接口会同时
    // 把 finish_reason 错误地标成 stop。不能仅依据 finish_reason 判断流程结束。
    if (!toolCalls.isEmpty()) {
        // 缓存本轮 assistant tool_calls 消息，供回填下一轮 continueWithToolResults
        m_lastAssistantToolCalls = toolCalls;

        QVector<ToolCall> calls;
        for (const auto& v : toolCalls) {
            const QJsonObject o = v.toObject();
            ToolCall c;
            c.id   = o.value(QStringLiteral("id")).toString();
            const QJsonObject fn = o.value(QStringLiteral("function")).toObject();
            c.name = fn.value(QStringLiteral("name")).toString();
            const QJsonValue argsValue = fn.value(QStringLiteral("arguments"));
            if (argsValue.isObject()) {
                c.arguments = argsValue.toObject();
            } else if (argsValue.isString()) {
                QJsonParseError parseError{};
                const QJsonDocument argsDoc = QJsonDocument::fromJson(
                    argsValue.toString().toUtf8(), &parseError);
                if (parseError.error == QJsonParseError::NoError && argsDoc.isObject()) {
                    c.arguments = argsDoc.object();
                }
            }
            if (c.isValid()) calls.append(c);
        }
        if (calls.isEmpty()) {
            abortWithError(QStringLiteral("模型返回了无法解析的工具调用"));
            return;
        }

        if (m_totalToolCalls + calls.size() > MAX_TOOL_CALLS_PER_ANSWER) {
            const int allow = MAX_TOOL_CALLS_PER_ANSWER - m_totalToolCalls;
            if (allow <= 0) {
                qWarning() << "[ToolOrchestrator] 达到工具调用上限"
                           << "总调用数=" << m_totalToolCalls
                           << "上限=" << MAX_TOOL_CALLS_PER_ANSWER
                           << "轮次=" << m_currentRound;
                abortWithError(QStringLiteral("已达到工具调用上限（%1次），操作被终止。\n"
                                              "提示：这通常是因为问题范围过大，建议缩小搜索范围或使用更具体的关键词。")
                                   .arg(MAX_TOOL_CALLS_PER_ANSWER));
                return;
            }
            qWarning() << "[ToolOrchestrator] 接近工具调用上限，截断工具列表"
                       << "已调用=" << m_totalToolCalls
                       << "本轮请求=" << calls.size()
                       << "允许=" << allow;
            calls.resize(allow);
        }
        m_totalToolCalls += calls.size();
        QStringList toolNames;
        for (const ToolCall& call : calls) toolNames.append(call.name);
        qDebug() << "[ToolOrchestrator] 模型请求调用工具"
                 << "会话=" << m_convId
                 << "轮次=" << m_currentRound
                 << "工具列表=" << toolNames.join(QStringLiteral(", "));
        executeToolsThenContinue(calls, m_currentRound);
        return;
    }

    // 仅首轮要求强制工具调用。工具结果回填后的后续轮次负责生成最终确认文本，
    // 此时不应再次要求模型调用同一个播放器工具。
    if (m_currentRound == 0 && m_toolChoice.isObject()) {
        const QVector<ToolCall> fallbackCalls = fallbackPlayerCalls();
        if (!fallbackCalls.isEmpty()) {
            m_totalToolCalls += fallbackCalls.size();
            executeToolsThenContinue(fallbackCalls, m_currentRound);
            return;
        }
        const QString forcedName = m_toolChoice.toObject()
                                       .value(QStringLiteral("function")).toObject()
                                       .value(QStringLiteral("name")).toString();
        abortWithError(QStringLiteral("模型未调用强制工具 %1，且无法从请求中解析播放器参数")
                           .arg(forcedName));
        return;
    }

    // 没有工具调用时，stop / length 才表示普通最终回答。
    if (finishReason == QLatin1String("stop")
        || finishReason == QLatin1String("length") || toolCalls.isEmpty()) {
        finishWithAnswer(textContent.isEmpty() ? m_streamingText : textContent);
        return;
    }

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
    // 检查工具调用预算
    if (m_totalToolCalls >= MAX_TOOL_CALLS_PER_ANSWER * 0.8) {
        qWarning() << "[ToolOrchestrator] 工具调用接近上限"
                   << "已使用=" << m_totalToolCalls
                   << "上限=" << MAX_TOOL_CALLS_PER_ANSWER
                   << "使用率=" << (m_totalToolCalls * 100 / MAX_TOOL_CALLS_PER_ANSWER) << "%";
    }

    // 顺序执行（简化；后续可并行独立工具）
    auto pendingResults = QSharedPointer<QJsonArray>::create();
    auto executed       = QSharedPointer<int>::create(0);
    const int total     = calls.size();

    for (const ToolCall& c : calls) {
        const QString argsText = QString::fromUtf8(
            QJsonDocument(c.arguments).toJson(QJsonDocument::Compact));
        qDebug() << "[ToolOrchestrator] 开始调用工具"
                 << "会话=" << m_convId
                 << "轮次=" << round
                 << "工具=" << c.name
                 << "参数=" << argsText;
        emit toolCallStarted(c);
        ITool* tool = m_registry->getTool(c.name);
        if (!tool) {
            ToolResult r = ToolResult::fail(c.id, c.name,
                                            QStringLiteral("Tool 未注册: %1").arg(c.name));
            m_toolTrace.append(r);
            pendingResults->append(QJsonObject{
                { QStringLiteral("role"), QStringLiteral("tool") },
                { QStringLiteral("tool_call_id"), c.id },
                { QStringLiteral("content"),
                  QString::fromUtf8(QJsonDocument(QJsonObject{
                      { QStringLiteral("error"), r.error } }).toJson(QJsonDocument::Compact)) } });
            emit toolCallFinished(r);
            ++(*executed);
            if (*executed >= total) {
                abortWithError(QStringLiteral("Tool 执行失败：%1").arg(r.error));
            }
            continue;
        }

        // 记录 assistant tool_calls 消息用于下一轮回填
        tool->executeAsync(c.id, c.arguments,
            [this, c, pendingResults, executed, total, round](const ToolResult& result) {
                if (!m_running) return;
                m_toolTrace.append(result);
                qDebug() << "[ToolOrchestrator] 工具调用结束"
                         << "会话=" << m_convId
                         << "轮次=" << round
                         << "工具=" << result.toolName
                         << "结果=" << (result.success ? QStringLiteral("成功") : QStringLiteral("失败"))
                         << (result.success ? QString() : QStringLiteral("错误=") + result.error);
                emit toolCallFinished(result);

                // 组装 tool 消息
                QJsonObject toolMsg;
                toolMsg.insert(QStringLiteral("role"), QStringLiteral("tool"));
                toolMsg.insert(QStringLiteral("tool_call_id"), c.id);
                const QJsonDocument payload(result.success ? result.data
                                              : QJsonObject{{ QStringLiteral("error"), result.error }});
                toolMsg.insert(QStringLiteral("content"),
                                QString::fromUtf8(payload.toJson(QJsonDocument::Compact)));
                pendingResults->append(toolMsg);
                ++(*executed);

                if (*executed >= total) {
                    const bool isForcedPlayerTool = m_toolChoice.isObject()
                        && m_toolChoice.toObject()
                               .value(QStringLiteral("function")).toObject()
                               .value(QStringLiteral("name")).toString()
                               == QLatin1String("control_player");
                    if (isForcedPlayerTool) {
                        if (result.success) {
                            const QString action = result.data.value(
                                QStringLiteral("action")).toString();
                            if (action == QLatin1String("seek")) {
                                const double requested = result.data.value(
                                    QStringLiteral("requested_timestamp_ms")).toDouble() / 1000.0;
                                const double actual = result.data.value(
                                    QStringLiteral("actual_timestamp_ms")).toDouble() / 1000.0;
                                finishWithAnswer(QStringLiteral("已成功跳转到 %1 秒（实际位置 %2 秒）")
                                                     .arg(requested, 0, 'f', 1)
                                                     .arg(actual, 0, 'f', 1));
                            } else {
                                finishWithAnswer(action == QLatin1String("play")
                                                     ? QStringLiteral("已开始播放")
                                                     : QStringLiteral("已暂停播放"));
                            }
                        } else {
                            abortWithError(QStringLiteral("播放器操作失败：%1").arg(result.error));
                        }
                        return;
                    }

                    if (round + 1 >= MAX_ROUNDS) {
                        finishWithAnswer(QStringLiteral("[已达最大工具轮次] ") + m_streamingText);
                        return;
                    }

                    QJsonArray assistantMsg;
                    QJsonObject assistantEntry;
                    assistantEntry.insert(QStringLiteral("role"), QStringLiteral("assistant"));
                    assistantEntry.insert(QStringLiteral("content"), QString());
                    assistantEntry.insert(QStringLiteral("tool_calls"), m_lastAssistantToolCalls);
                    assistantMsg.append(assistantEntry);

                    m_currentRound = round + 1;
                    emit roundStarted(m_currentRound);
                    m_streamingText.clear();
                    m_agent->continueWithToolResults(
                        m_convId, assistantMsg, *pendingResults, m_activeTools);
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
