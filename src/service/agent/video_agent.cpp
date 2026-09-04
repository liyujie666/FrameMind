#include "service/agent/video_agent.h"

#include "service/agentservice.h"
#include "service/agent/video_analysis_service.h"
#include "service/agent/tool_orchestrator.h"
#include "service/agent/perception_strategy.h"
#include "service/agent/reflection_engine.h"
#include "service/rag/video_rag_retriever.h"
#include "service/rag/qa_cache_manager.h"
#include "service/rag/entity_tracker.h"
#include "service/rag/evidence_composer.h"
#include "model/entity_profile.h"
#include "model/video_representation.h"

#include "service/agent/workflow/workflow_executor.h"
#include "service/agent/workflow/workflow_factory.h"
#include "service/agent/workflow/workflow_checkpoint.h"
#include "service/agent/workflow/workflow_state.h"

#include <QDebug>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <limits>

namespace {

/// 判断问题是否属于播放器操作意图（seek/play/pause）
/// 用于跳过 QA 缓存、强制 tool_choice
bool isPlayerOp(const QString& question)
{
    static const QRegularExpression englishRe(
        QStringLiteral("(?<![A-Za-z])(seek|play|pause|go\\s*to|jump\\s*to)(?![A-Za-z])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression chineseRe(
        QStringLiteral("跳转|跳到|播放|暂停|快进|快退|定位"));
    return englishRe.match(question).hasMatch()
           || chineseRe.match(question).hasMatch();
}

bool requestsFreshAnalysis(const QString& question)
{
    static const QRegularExpression re(
        QStringLiteral("重新(?:分析|检查|识别|看|检索)|再(?:分析|检查|识别|看一次)|"
                       "(?:重新|再)\\s*(?:analy[sz]e|check|inspect|retrieve)"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(question).hasMatch();
}

QVector<int> evidenceSceneIds(const QVector<RetrievalResult>& evidence)
{
    QSet<int> uniqueIds;
    for (const RetrievalResult& result : evidence) {
        const QVariant sceneId = result.chunk.metadata.value(QStringLiteral("scene_id"));
        if (sceneId.isValid()) uniqueIds.insert(sceneId.toInt());
    }
    return QVector<int>(uniqueIds.cbegin(), uniqueIds.cend());
}

} // namespace

VideoAgent::VideoAgent(AgentService*          agent,
                        VideoAnalysisService*  analysis,
                        VideoRAGRetriever*     retriever,
                        QACacheManager*        qaCache,
                        ToolOrchestrator*      orchestrator,
                        PerceptionStrategy*    perception,
                        ReflectionEngine*      reflection,
                        EntityTracker*         entities,
                        QObject*               parent)
    : QObject(parent)
    , m_agent(agent)
    , m_analysis(analysis)
    , m_retriever(retriever)
    , m_qaCache(qaCache)
    , m_orchestrator(orchestrator)
    , m_perception(perception)
    , m_reflection(reflection)
    , m_entities(entities)
{
}

// ============================================================
// 视频切换
// ============================================================

void VideoAgent::setActiveVideo(const QString& videoPath, const QString& videoId)
{
    m_activeVideoPath = videoPath;
    m_activeVideoId   = videoId;
    if (m_entities) m_entities->reload(videoId);
    if (m_orchestrator) m_orchestrator->setActiveVideoContext(videoPath, videoId);
}

// ============================================================
// 主入口
// ============================================================

void VideoAgent::ask(const QString& conversationId,
                      const QString& question,
                      const QList<QImage>& userFrames,
                      const VideoContext& videoCtx,
                      int64_t currentPlayerPosMs,
                      std::function<void(const QString&)> onProgress,
                      std::function<void(const AgentAnswer&)> onDone,
                      std::function<void(const QString&)> onError)
{
    if (m_busy) {
        if (onError) onError(QStringLiteral("Agent 正在处理，请稍后"));
        return;
    }
    m_busy = true;
    m_currentConvId = conversationId;
    m_currentQuestion = question;
    m_currentPlayerPosMs = currentPlayerPosMs;
    m_retrievedEvidence.clear();
    m_reflectionRetries = 0;
    m_onProgress = std::move(onProgress);
    m_onDone     = std::move(onDone);
    m_onError    = std::move(onError);

    emit stageChanged(QStringLiteral("PERCEIVE"));

    // ===快速路径：QA 缓存命中 ===
    // 播放器操作（seek/play/pause）必须每次真正执行，不走缓存
    const bool playerOp = isPlayerOp(question);
    const bool forceFreshAnalysis = requestsFreshAnalysis(question);
    if (!playerOp && !forceFreshAnalysis && m_qaCache && !m_activeVideoId.isEmpty()) {
        auto cached = m_qaCache->tryAnswer(m_activeVideoId, question);
        if (cached) {
            AgentAnswer ans;
            ans.conversationId = conversationId;
            ans.question       = question;
            ans.answer         = QStringLiteral("[历史分析结论，相似度 %1%%]\n%2")
                                     .arg(int(cached->similarity * 100))
                                     .arg(cached->answer);
            ans.confidence     = cached->originalConfidence;
            ans.fromCache      = true;
            finishAnswer(ans);
            return;
        }
    }

    // === Workflow 模式（优先） ===
    if (m_workflowExecutor && m_workflowFactory && !playerOp) {
        askViaWorkflow(conversationId, question, userFrames, videoCtx, currentPlayerPosMs);
        return;
    }

    // === PERCEIVE: 感知策略 ===
    QSharedPointer<VideoRepresentation> repr;
    if (m_analysis) repr = m_analysis->representation(m_activeVideoPath);

    SamplingPlan samplingPlan;
    QuestionType questionType = QuestionType::Unknown;
    if (m_perception) {
        questionType = m_perception->classifyQuestion(question);
        samplingPlan = m_perception->decideSampling(question, repr, currentPlayerPosMs);
        qDebug() << "[VideoAgent] PERCEIVE"
                 << "qType=" << static_cast<int>(questionType)
                 << "needPerception=" << samplingPlan.needNewPerception
                 << "density=" << static_cast<int>(samplingPlan.density)
                 << "budget=" << samplingPlan.frameBudget
                 << "ranges=" << samplingPlan.timeRanges.size();
    }

    // === REPRESENT: RAG 检索 — 受 PerceptionStrategy 约束 ===
    if (m_retriever && !m_activeVideoId.isEmpty()) {
        VideoRAGRetriever::Constraints c;
        c.videoId = m_activeVideoId;
        c.currentPositionMs = currentPlayerPosMs;
        if (repr) c.videoDurationMs = repr->metadata.durationMs;
        const QueryPlan queryPlan = m_retriever->compileQueryPlan(question, c);

        // 采样计划只在问题本身没有明确时间约束时限制检索，避免“00:30”
        // 被感知策略的候选范围覆盖。
        if (!queryPlan.hasTimeRange() && !samplingPlan.timeRanges.isEmpty()
            && questionType != QuestionType::GlobalSummary) {
            // 取采样计划中最大的时间范围作为检索约束
            int64_t minStart = std::numeric_limits<int64_t>::max();
            int64_t maxEnd = 0;
            for (const auto& range : samplingPlan.timeRanges) {
                minStart = std::min(minStart, range.first);
                maxEnd = std::max(maxEnd, range.second);
            }
            c.startMsGte = minStart;
            c.endMsLte = maxEnd;
        }

        // P1: 根据问题类型调整检索数量和路径偏好
        int topK = 5;
        switch (questionType) {
        case QuestionType::CurrentFrame:
            topK = 3; // 当前帧问题不需要太多检索
            break;
        case QuestionType::GlobalSummary:
            topK = 8; // 全局问题需要更多证据
            break;
        case QuestionType::EntityQuery:
            topK = 6;
            c.preferPath = QStringLiteral("entity"); // 优先实体路径
            break;
        case QuestionType::TemporalLocalization:
        case QuestionType::CausalReasoning:
            topK = 6;
            break;
        default:
            topK = 5;
            break;
        }

        m_retrievedEvidence = m_retriever->retrieve(question, c, topK);
        qDebug() << "[VideoAgent] RAG retrieve"
                 << "videoId=" << m_activeVideoId
                 << "hits=" << m_retrievedEvidence.size()
                 << "qType=" << static_cast<int>(questionType)
                 << "constrained=" << (!samplingPlan.timeRanges.isEmpty());
    }

    // === REASON + ACT: 通过 ToolOrchestrator 让 LLM 决定是否调工具 ===
    phaseReasonAndAct(conversationId, question, userFrames, videoCtx);
}

void VideoAgent::cancel()
{
    if (m_workflowExecutor && m_workflowExecutor->isRunning()) {
        m_workflowExecutor->cancel();
    }
    if (m_orchestrator) m_orchestrator->cancel();
    m_busy = false;
}

// ============================================================
// 阶段实现
// ============================================================

void VideoAgent::phasePerceive(const QString& question,
                                 QSharedPointer<VideoRepresentation> repr,
                                 int64_t currentPosMs)
{
    if (!m_perception) return;
    // 生成采样计划（当前仅记录/上报，具体执行留给 Tool 层按需触发）
    const SamplingPlan plan =
        m_perception->decideSampling(question, repr, currentPosMs);
    if (plan.needNewPerception) {
        qDebug() << "[VideoAgent] SamplingPlan: density=" << int(plan.density)
                 << "budget=" << plan.frameBudget
                 << "ranges=" << plan.timeRanges.size();
    }
}

void VideoAgent::phaseReasonAndAct(const QString& convId,
                                     const QString& question,
                                     const QList<QImage>& userFrames,
                                     const VideoContext& ctx)
{
    if (!m_orchestrator) {
        failWith(QStringLiteral("ToolOrchestrator 未初始化"));
        return;
    }
    emit stageChanged(QStringLiteral("REASON+ACT"));

    VideoContext enrichedCtx = ctx;
    enrichedCtx.retrievalEvidence = EvidenceComposer::formatText(m_retrievedEvidence);
    enrichedCtx.currentPositionMs = m_currentPlayerPosMs;
    const QList<QImage> evidenceFrames = EvidenceComposer::mergeFrames(
        userFrames, m_retrievedEvidence);

    // === P2: 实体档案显式注入 ===
    if (m_entities && !m_activeVideoId.isEmpty()) {
        const auto entities = m_entities->listEntities(m_activeVideoId);
        if (!entities.isEmpty()) {
            QString entityText;
            int idx = 1;
            for (const auto& e : entities) {
                entityText += QStringLiteral("## 实体 %1: %2\n")
                                  .arg(idx).arg(e.primaryDescription.left(100));
                entityText += QStringLiteral("- 类型: %1\n")
                                  .arg(e.type == EntityProfile::Person
                                           ? QStringLiteral("人物")
                                           : e.type == EntityProfile::Object
                                                 ? QStringLiteral("物体")
                                                 : QStringLiteral("其他"));
                if (!e.aliases.isEmpty())
                    entityText += QStringLiteral("- 别名: %1\n")
                                      .arg(e.aliases.join(QStringLiteral(", ")));
                entityText += QStringLiteral("- 出现次数: %1\n\n").arg(e.appearances.size());
                ++idx;
                if (idx > 10) break; // 最多注入 10 个实体
            }
            enrichedCtx.entityContext = entityText;
        }
    }

    qDebug() << "[VideoAgent] inject context"
             << "evidenceChars=" << enrichedCtx.retrievalEvidence.size()
             << "entityChars=" << enrichedCtx.entityContext.size()
             << "sceneOverviewChars=" << enrichedCtx.sceneOverview.size();

    // 播放器操作强制指定 tool_choice，防止 LM 绕过工具直接输出文字
    QJsonValue toolChoice = QJsonValue(QStringLiteral("auto"));
    if (isPlayerOp(question)) {
        toolChoice = QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"),
              QJsonObject{{ QStringLiteral("name"),
                            QStringLiteral("control_player") }} }
        };
    }

    m_orchestrator->runQuery(convId, question, evidenceFrames, enrichedCtx,
        [this](const QString& delta) {
            if (m_onProgress) m_onProgress(delta);
        },
        [this, convId, question, userFrames, enrichedCtx, toolChoice]
        (const QString& answer, const QVector<ToolResult>& toolTrace, int rounds) {
            emit stageChanged(QStringLiteral("REFLECT"));

            AgentAnswer result;
            result.conversationId = m_currentConvId;
            result.question       = m_currentQuestion;
            result.answer         = answer;
            result.rounds         = rounds;
            result.evidence       = m_retrievedEvidence;
            for (const auto& t : toolTrace) result.toolCallsTrace.append(t.toolName);

            if (m_reflection) {
                auto repr = m_analysis ? m_analysis->representation(m_activeVideoPath)
                                          : QSharedPointer<VideoRepresentation>();
                const auto rr = m_reflection->reflect(answer, m_retrievedEvidence, repr);
                result.confidence = rr.confidence;
                for (const auto& iss : rr.issues) emit reflectionIssueFound(iss.detail);

                // === P2: 反思闭环 ===
                // 当反思发现严重问题（置信度 < 0.5）且尚未重试，触发补充检索并重新推理
                if (!rr.valid && rr.confidence < 0.5f
                    && m_reflectionRetries < kMaxReflectionRetries
                    && !isPlayerOp(m_currentQuestion)) {
                    ++m_reflectionRetries;
                    qDebug() << "[VideoAgent] REFLECT: low confidence"
                             << rr.confidence << ", retrying with expanded retrieval"
                             << "(attempt" << m_reflectionRetries << ")";

                    emit stageChanged(QStringLiteral("REFLECT_RETRY"));

                    // 补充检索：扩大 topK 和去除时间约束
                    if (m_retriever && !m_activeVideoId.isEmpty()) {
                        VideoRAGRetriever::Constraints expandedC;
                        expandedC.videoId = m_activeVideoId;
                        // 不设时间约束，扩大检索范围
                        auto expanded = m_retriever->retrieve(question, expandedC, 10);
                        // 合并去重
                        for (const auto& e : expanded) {
                            bool exists = false;
                            for (const auto& existing : m_retrievedEvidence) {
                                if (existing.chunk.startMs == e.chunk.startMs
                                    && existing.chunk.endMs == e.chunk.endMs) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) m_retrievedEvidence.append(e);
                        }
                    }

                    // 用扩展后的证据重新调用 REASON+ACT
                    VideoContext retryCtx = enrichedCtx;
                    retryCtx.retrievalEvidence = EvidenceComposer::formatText(m_retrievedEvidence);
                    const QList<QImage> retryFrames = EvidenceComposer::mergeFrames(
                        userFrames, m_retrievedEvidence);

                    // 在证据中附加反思反馈，告知 LLM 上次答案的问题
                    retryCtx.retrievalEvidence += QStringLiteral(
                        "\n# 反思反馈（上次回答存在以下问题，请修正）\n%1\n")
                                                      .arg(rr.fixSuggestion);

                    m_orchestrator->runQuery(convId, question, retryFrames, retryCtx,
                        [this](const QString& delta) {
                            if (m_onProgress) m_onProgress(delta);
                        },
                        [this](const QString& retryAnswer,
                                const QVector<ToolResult>& retryTrace, int retryRounds) {
                            AgentAnswer retryResult;
                            retryResult.conversationId = m_currentConvId;
                            retryResult.question       = m_currentQuestion;
                            retryResult.answer         = retryAnswer;
                            retryResult.rounds         = retryRounds;
                            retryResult.evidence       = m_retrievedEvidence;
                            for (const auto& t : retryTrace)
                                retryResult.toolCallsTrace.append(t.toolName);

                            // 重新反思（仅评估，不再递归重试）
                            if (m_reflection) {
                                auto repr2 = m_analysis
                                                 ? m_analysis->representation(m_activeVideoPath)
                                                 : QSharedPointer<VideoRepresentation>();
                                const auto rr2 = m_reflection->reflect(
                                    retryAnswer, m_retrievedEvidence, repr2);
                                retryResult.confidence = rr2.confidence;
                            } else {
                                retryResult.confidence = 0.7f;
                            }

                            const QVector<int> retrySceneIds = evidenceSceneIds(m_retrievedEvidence);
                            if (!isPlayerOp(m_currentQuestion)
                                && m_qaCache && !m_activeVideoId.isEmpty()
                                && !retrySceneIds.isEmpty()
                                && retryResult.confidence >= 0.7f) {
                                m_qaCache->cache(m_activeVideoId,
                                                 m_currentQuestion, retryAnswer,
                                                 retryResult.confidence, retrySceneIds);
                            }
                            finishAnswer(retryResult);
                        },
                        [this](const QString& err) { failWith(err); },
                        toolChoice);
                    return; // 不执行下面的 finishAnswer
                }
            } else {
                result.confidence = 0.8f;
            }

            // 只缓存能回溯到实际场景证据的高可信结论，避免错误答案被相似查询复用。
            const QVector<int> sceneIds = evidenceSceneIds(m_retrievedEvidence);
            if (!isPlayerOp(m_currentQuestion)
                && m_qaCache && !m_activeVideoId.isEmpty()
                && !sceneIds.isEmpty() && result.confidence >= 0.7f) {
                m_qaCache->cache(m_activeVideoId, m_currentQuestion, answer,
                                 result.confidence, sceneIds);
            }

            finishAnswer(result);
        },
        [this](const QString& err) { failWith(err); },
        toolChoice);
}

// ============================================================
// 结果回调
// ============================================================

void VideoAgent::finishAnswer(const AgentAnswer& answer)
{
    m_busy = false;
    if (m_onDone) m_onDone(answer);
}

void VideoAgent::failWith(const QString& err)
{
    m_busy = false;
    if (m_onError) m_onError(err);
}

// ============================================================
// Workflow 引擎集成
// ============================================================

void VideoAgent::setWorkflowExecutor(WorkflowExecutor* executor)
{
    m_workflowExecutor = executor;
}

void VideoAgent::setWorkflowFactory(WorkflowFactory* factory)
{
    m_workflowFactory = factory;
}

void VideoAgent::setWorkflowCheckpoint(WorkflowCheckpoint* checkpoint)
{
    m_workflowCheckpoint = checkpoint;
}

void VideoAgent::askViaWorkflow(const QString& conversationId,
                                 const QString& question,
                                 const QList<QImage>& userFrames,
                                 const VideoContext& videoCtx,
                                 int64_t currentPlayerPosMs)
{
    // 预设中的 function 节点必须在加载图之前绑定到真实服务；未绑定时 FunctionNode
    // 会静默直通，导致默认 Workflow 从未执行检索与质量评估。
    m_workflowFactory->registerFunctionHandler(
        QStringLiteral("PerceptionStrategy::decide"),
        [this](WorkflowState& workflowState, NodeCallback done) {
            QSharedPointer<VideoRepresentation> repr;
            if (m_analysis) repr = m_analysis->representation(m_activeVideoPath);
            SamplingPlan plan;
            if (m_perception) {
                plan = m_perception->decideSampling(
                    workflowState.get(QStringLiteral("question")).toString(), repr,
                    workflowState.get(QStringLiteral("current_pos_ms")).toLongLong());
            }
            workflowState.set(QStringLiteral("sampling_plan"), QVariant::fromValue(plan));
            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });
    m_workflowFactory->registerFunctionHandler(
        QStringLiteral("VideoRAGRetriever::retrieve"),
        [this](WorkflowState& workflowState, NodeCallback done) {
            const QString workflowQuestion = workflowState.get(QStringLiteral("question")).toString();
            const QString workflowVideoId = workflowState.get(QStringLiteral("video_id")).toString();
            m_retrievedEvidence.clear();
            if (m_retriever && !workflowQuestion.isEmpty() && !workflowVideoId.isEmpty()) {
                VideoRAGRetriever::Constraints constraints;
                constraints.videoId = workflowVideoId;
                constraints.currentPositionMs = workflowState.get(
                    QStringLiteral("current_pos_ms")).toLongLong();
                if (m_analysis) {
                    const auto repr = m_analysis->representation(m_activeVideoPath);
                    if (repr) constraints.videoDurationMs = repr->metadata.durationMs;
                }
                m_retrievedEvidence = m_retriever->retrieve(workflowQuestion, constraints, 6);
            }

            VideoContext context;
            const QVariant contextValue = workflowState.get(QStringLiteral("video_context"));
            if (contextValue.canConvert<VideoContext>()) context = contextValue.value<VideoContext>();
            workflowState.addArtifact(QStringLiteral("retrieval_evidence"),
                                      EvidenceComposer::toJson(m_retrievedEvidence));
            context.retrievalEvidence = EvidenceComposer::formatText(m_retrievedEvidence);
            context.currentPositionMs = workflowState.get(QStringLiteral("current_pos_ms")).toLongLong();
            workflowState.set(QStringLiteral("video_context"), QVariant::fromValue(context));

            QList<QImage> userFrames;
            const QVariant framesValue = workflowState.get(QStringLiteral("user_frames"));
            if (framesValue.canConvert<QList<QImage>>()) userFrames = framesValue.value<QList<QImage>>();
            workflowState.set(QStringLiteral("user_frames"), QVariant::fromValue(
                EvidenceComposer::mergeFrames(userFrames, m_retrievedEvidence)));

            // 没有静态检索命中时仍进入带工具的推理节点，让模型可发起局部复核，
            // 不在 perceive/retrieve 间空转。
            workflowState.set(QStringLiteral("sufficiency"), 0.7);
            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });
    m_workflowFactory->registerFunctionHandler(
        QStringLiteral("ReflectionEngine::check"),
        [this](WorkflowState& workflowState, NodeCallback done) {
            const QVector<RetrievalResult> evidence = EvidenceComposer::fromJson(
                workflowState.artifact(QStringLiteral("retrieval_evidence")).toArray());
            float confidence = 0.7f;
            if (m_reflection) {
                QSharedPointer<VideoRepresentation> repr;
                if (m_analysis) repr = m_analysis->representation(m_activeVideoPath);
                const auto reflection = m_reflection->reflect(
                    workflowState.get(QStringLiteral("answer")).toString(), evidence, repr);
                confidence = reflection.confidence;
                for (const auto& issue : reflection.issues) emit reflectionIssueFound(issue.detail);
            }
            workflowState.set(QStringLiteral("confidence"), confidence);
            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        });

    // 从预设加载视频 QA 工作流，优先从资源文件加载，失败则尝试文件系统。
    WorkflowGraph graph = m_workflowFactory->fromFile(
        QStringLiteral(":/workflow/presets/video_qa.json"));
    QString validateError;
    if (!graph.validate(&validateError)) {
        qWarning() << "[VideoAgent] Cannot load workflow from resources, trying filesystem...";
        graph = m_workflowFactory->fromFile(
            QStringLiteral("src/service/agent/workflow/presets/video_qa.json"));
    }
    if (!graph.validate(&validateError)) {
        qWarning() << "[VideoAgent] Workflow graph invalid:" << validateError
                   << "- falling back to legacy mode";
        emit stageChanged(QStringLiteral("PERCEIVE"));
        QSharedPointer<VideoRepresentation> repr;
        if (m_analysis) repr = m_analysis->representation(m_activeVideoPath);
        phasePerceive(question, repr, currentPlayerPosMs);
        if (m_retriever && !m_activeVideoId.isEmpty()) {
            VideoRAGRetriever::Constraints constraints;
            constraints.videoId = m_activeVideoId;
            constraints.currentPositionMs = currentPlayerPosMs;
            if (repr) constraints.videoDurationMs = repr->metadata.durationMs;
            m_retrievedEvidence = m_retriever->retrieve(question, constraints, 5);
        }
        phaseReasonAndAct(conversationId, question, userFrames, videoCtx);
        return;
    }

    // 构建初始 state
    WorkflowState state;
    state.set("question", question);
    state.set("conversation_id", conversationId);
    state.set("video_path", m_activeVideoPath);
    state.set("video_id", m_activeVideoId);
    state.set("current_pos_ms", static_cast<qlonglong>(currentPlayerPosMs));
    state.set("video_context", QVariant::fromValue(videoCtx));
    if (!userFrames.isEmpty()) {
        state.set("user_frames", QVariant::fromValue(userFrames));
    }

    // 设置 checkpoint
    QString workflowId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_workflowExecutor->setWorkflowId(workflowId);
    if (m_workflowCheckpoint) {
        m_workflowExecutor->setCheckpoint(m_workflowCheckpoint);
    }

    // 使用上下文对象确保本次工作流的连接在完成或失败后自动断开。
    auto* context = new QObject(this);
    const QString workflowVideoId = m_activeVideoId;
    connect(m_workflowExecutor, &WorkflowExecutor::workflowCompleted,
            context, [this, conversationId, question, workflowVideoId, context]
            (const WorkflowState& finalState) {
        AgentAnswer result;
        result.conversationId = conversationId;
        result.question = question;
        result.answer = finalState.get(QStringLiteral("answer")).toString();
        result.confidence = finalState.get(QStringLiteral("confidence")).toFloat();
        result.rounds = finalState.get(QStringLiteral("tool_rounds")).toInt();
        result.evidence = EvidenceComposer::fromJson(
            finalState.artifact(QStringLiteral("retrieval_evidence")).toArray());

        const QVector<int> sceneIds = evidenceSceneIds(result.evidence);
        if (m_qaCache && !workflowVideoId.isEmpty()
            && !sceneIds.isEmpty() && result.confidence >= 0.7f) {
            m_qaCache->cache(workflowVideoId, question, result.answer,
                             result.confidence, sceneIds);
        }

        finishAnswer(result);
        context->deleteLater();
    });

    connect(m_workflowExecutor, &WorkflowExecutor::workflowFailed,
            context, [this, context](const QString& error) {
        failWith(error);
        context->deleteLater(); // 清理连接
    });

    connect(m_workflowExecutor, &WorkflowExecutor::streamingChunk,
            context, [this](const QString& chunk) {
        if (m_onProgress) m_onProgress(chunk);
    });

    connect(m_workflowExecutor, &WorkflowExecutor::nodeEntered,
            context, [this](const QString& nodeId) {
        emit stageChanged(nodeId.toUpper());
    });

    // 启动工作流
    m_workflowExecutor->run(graph, std::move(state));
}
