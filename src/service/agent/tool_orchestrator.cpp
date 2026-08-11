#include "service/agent/tool_orchestrator.h"

#include "service/agentservice.h"
#include "service/agent/tool_registry.h"
#include "service/agent/tools/search_video_content_tool.h"
#include "service/agent/tools/get_transcript_tool.h"
#include "service/agent/tools/get_scene_info_tool.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QTimer>
#include <QDebug>
#include <utility>

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
    ++m_runGeneration;
    m_currentRound = 0;
    m_totalToolCalls = 0;
    m_forcingFinalAnswer = false;
    m_streamingText.clear();
    m_toolTrace.clear();
    m_lastAssistantToolCalls = QJsonArray();

    m_onProgress = std::move(onProgress);
    m_onDone     = std::move(onDone);
    m_onError    = std::move(onError);

    startRound(0);
}

void ToolOrchestrator::cancel()
{
    ++m_runGeneration;
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
    if (convId != m_convId || !m_running) return;
    m_streamingText += delta;
    if (m_onProgress) m_onProgress(delta);
}

void ToolOrchestrator::onAgentFinished(const QString& convId,
                                         const QJsonArray& toolCalls,
                                         const QString& finishReason,
                                         const QString& textContent)
{
    qDebug() << "[ToolOrchestrator] onAgentFinished called"
             << "convId=" << convId << "m_convId=" << m_convId
             << "m_running=" << m_running
             << "finishReason=" << finishReason
             << "toolCalls.size=" << toolCalls.size()
             << "textContent.length=" << textContent.length();
    
    if (convId != m_convId || !m_running) {
        qWarning() << "[ToolOrchestrator] onAgentFinished: convId mismatch or not running, ignoring";
        return;
    }

    // 情况 1: LLM 直接生成回答（stop / length）
    if (finishReason == QLatin1String("stop") || finishReason == QLatin1String("length")
        || toolCalls.isEmpty()) {
        qDebug() << "[ToolOrchestrator] finishing with direct answer";
        finishWithAnswer(textContent.isEmpty() ? m_streamingText : textContent);
        return;
    }

    // 情况 2: 请求工具调用（tool_calls）
    if (finishReason == QLatin1String("tool_calls") || !toolCalls.isEmpty()) {
        qDebug() << "[ToolOrchestrator] processing tool_calls";
        // 标准化整组调用：内部执行与回填历史必须使用完全相同的 ID。
        QVector<ToolCall> calls;
        QJsonArray normalizedToolCalls;
        for (int i = 0; i < toolCalls.size(); ++i) {
            QJsonObject normalized = toolCalls.at(i).toObject();
            ToolCall c;
            c.id = normalized.value(QStringLiteral("id")).toString();
            if (c.id.isEmpty())
                c.id = QStringLiteral("call_%1_%2_%3")
                           .arg(m_runGeneration).arg(m_currentRound).arg(i);
            normalized.insert(QStringLiteral("id"), c.id);
            normalized.insert(QStringLiteral("type"), QStringLiteral("function"));

            QJsonObject fn = normalized.value(QStringLiteral("function")).toObject();
            c.name = fn.value(QStringLiteral("name")).toString();
            const QString argsStr = fn.value(QStringLiteral("arguments")).toString();
            QJsonParseError parseError;
            const QJsonDocument argsDoc =
                QJsonDocument::fromJson(argsStr.toUtf8(), &parseError);
            if (c.name.isEmpty()) {
                c.validationError = QStringLiteral("工具名为空");
            } else if (parseError.error != QJsonParseError::NoError
                       || !argsDoc.isObject()) {
                c.validationError = QStringLiteral("工具参数不是有效 JSON 对象: %1")
                                        .arg(parseError.errorString());
            } else {
                c.arguments = argsDoc.object();
            }
            calls.append(c);
            normalizedToolCalls.append(normalized);
        }
        m_lastAssistantToolCalls = normalizedToolCalls;

        if (calls.isEmpty()) {
            finishWithAnswer(textContent.isEmpty() ? m_streamingText : textContent);
            return;
        }

        // 不截断 assistant 声明的调用；超预算调用由执行器返回对应失败结果，保持协议闭合。
        m_totalToolCalls += calls.size();
        executeToolsThenContinue(calls, m_currentRound);
        return;
    }

    // 其它情况：结束
    qDebug() << "[ToolOrchestrator] unknown finish reason, finishing with answer";
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
    const quint64 generation = m_runGeneration;
    const int total = calls.size();
    const int previouslyUsed = m_totalToolCalls - total;
    auto results = QSharedPointer<QVector<QJsonObject>>::create(total);
    auto finished = QSharedPointer<QVector<bool>>::create(total, false);
    auto completed = QSharedPointer<int>::create(0);

    auto complete = QSharedPointer<std::function<void(int, const ToolResult&)>>::create();
    *complete = [this, generation, round, total, results, finished, completed]
                (int index, const ToolResult& result) {
        if (!m_running || generation != m_runGeneration
            || index < 0 || index >= total || finished->at(index)) return;
        (*finished)[index] = true;
        ++(*completed);
        m_toolTrace.append(result);
        emit toolCallFinished(result);

        QJsonObject payload = result.success
                                  ? result.data
                                  : QJsonObject{{QStringLiteral("error"), result.error},
                                                {QStringLiteral("retryable"), false}};
        (*results)[index] = QJsonObject{
            {QStringLiteral("role"), QStringLiteral("tool")},
            {QStringLiteral("tool_call_id"), result.toolCallId},
            {QStringLiteral("name"), result.toolName},
            {QStringLiteral("content"), QString::fromUtf8(
                 QJsonDocument(payload).toJson(QJsonDocument::Compact))}};
        if (*completed < total) return;

        QJsonArray toolMessages;
        for (const QJsonObject& message : std::as_const(*results))
            toolMessages.append(message);
        QJsonArray assistantMessages;
        assistantMessages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("assistant")},
            {QStringLiteral("content"), QJsonValue::Null},
            {QStringLiteral("tool_calls"), m_lastAssistantToolCalls}});

        const bool budgetExhausted = m_totalToolCalls >= MAX_TOOL_CALLS_PER_ANSWER;
        const bool roundsExhausted = round + 1 >= MAX_ROUNDS;
        m_forcingFinalAnswer = budgetExhausted || roundsExhausted;
        m_currentRound = round + 1;
        emit roundStarted(m_currentRound);
        m_streamingText.clear();
        m_agent->continueWithToolResults(
            m_convId, assistantMessages, toolMessages,
            m_forcingFinalAnswer ? QJsonArray() : m_registry->allDefinitions(),
            m_forcingFinalAnswer ? QJsonValue(QStringLiteral("none"))
                                 : QJsonValue(QStringLiteral("auto")));
    };

    for (int i = 0; i < total; ++i) {
        ToolCall c = calls.at(i);
        emit toolCallStarted(c);

        if (previouslyUsed + i >= MAX_TOOL_CALLS_PER_ANSWER) {
            (*complete)(i, ToolResult::fail(
                c.id, c.name, QStringLiteral("工具调用预算已耗尽，请基于已有证据作答")));
            continue;
        }
        if (!c.validationError.isEmpty()) {
            (*complete)(i, ToolResult::fail(c.id, c.name, c.validationError));
            continue;
        }

        ITool* tool = m_registry->getTool(c.name);
        if (!tool) {
            (*complete)(i, ToolResult::fail(
                c.id, c.name, QStringLiteral("Tool 未注册: %1").arg(c.name)));
            continue;
        }
        const QString validationError = normalizeArguments(tool, c.arguments);
        if (!validationError.isEmpty()) {
            (*complete)(i, ToolResult::fail(c.id, c.name, validationError));
            continue;
        }

        QTimer::singleShot(DEFAULT_TOOL_TIMEOUT_MS, this,
            [complete, i, c]() {
                (*complete)(i, ToolResult::fail(
                    c.id, c.name, QStringLiteral("工具执行超时")));
            });

        tool->executeAsync(c.id, c.arguments,
            [complete, i](const ToolResult& result) {
                (*complete)(i, result);
            });
    }
}

QString ToolOrchestrator::normalizeArguments(ITool* tool,
                                              QJsonObject& args) const
{
    if (!tool) return QStringLiteral("工具不存在");
    const QJsonObject schema = tool->parameters();
    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();

    // 不同提供商会把数字/布尔序列化成字符串，先按 schema 归一化再校验，
    // 否则合法意图会被误判为参数错误而拒绝执行。
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        const QString key = it.key();
        if (!args.contains(key)) continue;
        const QJsonValue value = args.value(key);
        if (value.isNull() || value.isUndefined()) {
            args.remove(key);
            continue;
        }
        const QString type = it.value().toObject().value(QStringLiteral("type")).toString();
        if (type == QLatin1String("integer") || type == QLatin1String("number")) {
            if (value.isString()) {
                bool ok = false;
                const double number = value.toString().trimmed().toDouble(&ok);
                if (!ok)
                    return QStringLiteral("参数 %1 应为数字，收到: %2")
                               .arg(key, value.toString());
                args.insert(key, number);
            } else if (!value.isDouble()) {
                return QStringLiteral("参数 %1 应为数字").arg(key);
            }
        } else if (type == QLatin1String("boolean") && value.isString()) {
            const QString text = value.toString().trimmed().toLower();
            if (text == QLatin1String("true") || text == QLatin1String("false"))
                args.insert(key, text == QLatin1String("true"));
        } else if (type == QLatin1String("string") && value.isDouble()) {
            args.insert(key, QString::number(value.toDouble()));
        }
    }

    for (const QJsonValue& requiredValue
         : schema.value(QStringLiteral("required")).toArray()) {
        const QString key = requiredValue.toString();
        if (!args.contains(key))
            return QStringLiteral("缺少必填参数: %1").arg(key);
    }
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        const QJsonArray allowed =
            it.value().toObject().value(QStringLiteral("enum")).toArray();
        if (allowed.isEmpty() || !args.contains(it.key())) continue;
        if (!allowed.contains(args.value(it.key())))
            return QStringLiteral("参数 %1 不在允许范围内").arg(it.key());
    }
    return {};
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
