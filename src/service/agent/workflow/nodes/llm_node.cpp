#include "llm_node.h"

#include "service/agentservice.h"
#include "service/agent/tool_orchestrator.h"

#include <QJsonArray>
#include <QUuid>

LLMNode::LLMNode(const QString& id, Config config,
                 AgentService* agentService,
                 ToolOrchestrator* orchestrator)
    : m_id(id)
    , m_config(std::move(config))
    , m_agentService(agentService)
    , m_orchestrator(orchestrator)
{
}

void LLMNode::execute(WorkflowState& state, NodeCallback done)
{
    if (!m_agentService) {
        done(NodeResult{.nextRoute = {}, .success = false,
                        .error = "AgentService is null"});
        return;
    }

    if (m_config.enableToolCalling && m_orchestrator) {
        executeWithTools(state, std::move(done));
    } else {
        executeDirectLLM(state, std::move(done));
    }
}

void LLMNode::executeWithTools(WorkflowState& state, NodeCallback done)
{
    QString question = state.get(m_config.questionKey).toString();
    if (question.isEmpty()) {
        done(NodeResult{.nextRoute = {}, .success = false,
                        .error = "No question found in state"});
        return;
    }

    // 构建 VideoContext 从 state 获取
    VideoContext videoCtx;
    QVariant ctxVar = state.get("video_context");
    if (ctxVar.canConvert<VideoContext>()) {
        videoCtx = ctxVar.value<VideoContext>();
    }

    // 获取附带帧
    QList<QImage> frames;
    QVariant framesVar = state.get("user_frames");
    if (framesVar.canConvert<QList<QImage>>()) {
        frames = framesVar.value<QList<QImage>>();
    }

    QString convId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // 如果有 system prompt，先seed history
    if (!m_config.systemPrompt.isEmpty()) {
        ChatMessage sysMsg;
        sysMsg.role = ChatMessage::System;
        sysMsg.content = m_config.systemPrompt;
        m_agentService->seedHistory(convId, {sysMsg});
    }

    m_orchestrator->runQuery(
        convId, question, frames, videoCtx,
        // onProgress
        [](const QString&) {},
        // onDone
        [&state, done, this](const QString& answer,
                              const QVector<ToolResult>& toolTrace,
                              int rounds) {
            state.set(m_config.answerKey, answer);

            // 保存工具调用轨迹
            QJsonArray traceArray;
            for (const auto& tr : toolTrace) {
                QJsonObject obj;
                obj["toolName"] = tr.toolName;
                obj["success"] = tr.success;
                obj["data"] = tr.data;
                if (!tr.error.isEmpty()) obj["error"] = tr.error;
                traceArray.append(obj);
            }
            state.addArtifact("tool_trace", traceArray);
            state.set("tool_rounds", rounds);

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        },
        // onError
        [done](const QString& error) {
            done(NodeResult{.nextRoute = {}, .success = false, .error = error});
        }
    );
}

void LLMNode::executeDirectLLM(WorkflowState& state, NodeCallback done)
{
    QString question = state.get(m_config.questionKey).toString();
    if (question.isEmpty()) {
        done(NodeResult{.nextRoute = {}, .success = false,
                        .error = "No question found in state"});
        return;
    }

    QString convId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Seed system prompt
    if (!m_config.systemPrompt.isEmpty()) {
        ChatMessage sysMsg;
        sysMsg.role = ChatMessage::System;
        sysMsg.content = m_config.systemPrompt;
        m_agentService->seedHistory(convId, {sysMsg});
    }

    VideoContext videoCtx;
    QVariant ctxVar = state.get("video_context");
    if (ctxVar.canConvert<VideoContext>()) {
        videoCtx = ctxVar.value<VideoContext>();
    }

    QList<QImage> frames;
    QVariant framesVar = state.get("user_frames");
    if (framesVar.canConvert<QList<QImage>>()) {
        frames = framesVar.value<QList<QImage>>();
    }

    // 连接信号接收响应
    QMetaObject::Connection connFinished;
    QMetaObject::Connection connError;

    connFinished = QObject::connect(
        m_agentService, &AgentService::responseFinished,
        m_agentService, [&state, done, convId, this,
                          &connFinished, &connError](const QString& cId, const ChatMessage& msg) {
            if (cId != convId) return;

            QObject::disconnect(connFinished);
            QObject::disconnect(connError);

            state.set(m_config.answerKey, msg.content);
            state.addMessage(msg);

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });

    connError = QObject::connect(
        m_agentService, &AgentService::responseError,
        m_agentService, [done, convId, &connFinished, &connError](const QString& cId, const QString& error) {
            if (cId != convId) return;

            QObject::disconnect(connFinished);
            QObject::disconnect(connError);

            done(NodeResult{.nextRoute = {}, .success = false, .error = error});
        });

    m_agentService->sendMessage(convId, question, frames, videoCtx);
}
