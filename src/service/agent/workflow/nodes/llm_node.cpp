#include "llm_node.h"

#include "service/agentservice.h"
#include "service/agent/tool_orchestrator.h"

#include <QJsonArray>
#include <QUuid>
#include <memory>

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

    // 使用用户的原始 conversationId 保持对话上下文连贯
    QString convId = state.get("conversation_id").toString();
    if (convId.isEmpty()) {
        convId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    // 捕获 state 指针和所需的 key，避免引用悬挂
    WorkflowState* statePtr = &state;
    QString answerKey = m_config.answerKey;

    m_orchestrator->runQuery(
        convId, question, frames, videoCtx,
        [this](const QString& chunk) {
            if (m_streamingCallback) {
                m_streamingCallback(chunk);
            }
        },
        [statePtr, answerKey, done](const QString& answer,
                              const QVector<ToolResult>& toolTrace,
                              int rounds) {
            statePtr->set(answerKey, answer);

            QJsonArray traceArray;
            for (const auto& tr : toolTrace) {
                QJsonObject obj;
                obj["toolName"] = tr.toolName;
                obj["success"] = tr.success;
                obj["data"] = tr.data;
                if (!tr.error.isEmpty()) obj["error"] = tr.error;
                traceArray.append(obj);
            }
            statePtr->addArtifact("tool_trace", traceArray);
            statePtr->set("tool_rounds", rounds);

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        },
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

    // 使用用户的原始 conversationId 保持对话上下文连贯
    QString convId = state.get("conversation_id").toString();
    if (convId.isEmpty()) {
        convId = QUuid::createUuid().toString(QUuid::WithoutBraces);
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

    WorkflowState* statePtr = &state;
    QString answerKey = m_config.answerKey;

    // 使用 shared_ptr 管理连接的生命周期，避免局部变量引用悬挂
    auto conns = std::make_shared<std::pair<QMetaObject::Connection, QMetaObject::Connection>>();

    conns->first = QObject::connect(
        m_agentService, &AgentService::responseFinished,
        m_agentService, [statePtr, answerKey, done, convId, conns](const QString& cId, const ChatMessage& msg) {
            if (cId != convId) return;

            QObject::disconnect(conns->first);
            QObject::disconnect(conns->second);

            statePtr->set(answerKey, msg.content);
            statePtr->addMessage(msg);

            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });

    conns->second = QObject::connect(
        m_agentService, &AgentService::responseError,
        m_agentService, [done, convId, conns](const QString& cId, const QString& error) {
            if (cId != convId) return;

            QObject::disconnect(conns->first);
            QObject::disconnect(conns->second);

            done(NodeResult{.nextRoute = {}, .success = false, .error = error});
        });

    m_agentService->sendMessage(convId, question, frames, videoCtx);
}
