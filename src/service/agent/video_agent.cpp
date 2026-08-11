#include "service/agent/video_agent.h"

#include "service/agentservice.h"
#include "service/agent/video_analysis_service.h"
#include "service/agent/tool_orchestrator.h"
#include "service/agent/perception_strategy.h"
#include "service/agent/reflection_engine.h"
#include "service/rag/video_rag_retriever.h"
#include "service/rag/qa_cache_manager.h"
#include "service/rag/entity_tracker.h"
#include "model/entity_profile.h"
#include "model/video_representation.h"

#include "service/agent/workflow/workflow_executor.h"
#include "service/agent/workflow/workflow_factory.h"
#include "service/agent/workflow/workflow_checkpoint.h"
#include "service/agent/workflow/workflow_state.h"

#include <QDebug>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <limits>

namespace {

/// 判断问题是否属于播放器操作意图（seek/play/pause）
/// 用于跳过 QA 缓存、强制 tool_choice
bool isPlayerOp(const QString& question)
{
    static const QRegularExpression re(
        QStringLiteral(
            "\\b(seek|跳转|跳到|播放|暂停|play|pause|快进|快退|"
            "定位|跳|go\\s*to|jump\\s*to)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(question).hasMatch();
}

QString formatMs(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int sec = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
                   .arg(h)
                   .arg(m, 2, 10, QChar('0'))
                   .arg(sec, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

QString hitPathLabel(const QString& hitPath)
{
    if (hitPath == QLatin1String("visual")) return QStringLiteral("视觉检索");
    if (hitPath == QLatin1String("text")) return QStringLiteral("文本/语音");
    if (hitPath == QLatin1String("entity")) return QStringLiteral("实体");
    if (hitPath == QLatin1String("qa_cache")) return QStringLiteral("历史问答");
    return hitPath;
}

QString formatRetrievalEvidence(const QVector<RetrievalResult>& evidence)
{
    if (evidence.isEmpty()) return {};
    QString out;
    int idx = 1;
    for (const RetrievalResult& r : evidence) {
        const VideoChunk& c = r.chunk;
        out += QString::fromUtf8("## 证据 %1\n").arg(idx);
        out += QString::fromUtf8("时间范围：%1 - %2\n")
                   .arg(formatMs(c.startMs), formatMs(c.endMs));
        out += QString::fromUtf8("来源：%1（相似度 %2）\n")
                   .arg(hitPathLabel(r.hitPath))
                   .arg(r.score, 0, 'f', 2);
        const QVariant sceneVar = c.metadata.value(QStringLiteral("scene_id"));
        if (sceneVar.isValid()) {
            out += QString::fromUtf8("场景 ID：%1\n").arg(sceneVar.toInt());
        }
        const QString evidenceType =
            c.metadata.value(QStringLiteral("evidence_type")).toString();
        if (!evidenceType.isEmpty()) {
            out += QString::fromUtf8("证据类型：%1\n").arg(evidenceType);
        }
        const QString relation =
            c.metadata.value(QStringLiteral("audio_relation")).toString();
        if (!relation.isEmpty()) {
            out += QString::fromUtf8("音画关系：%1（置信度 %2）\n")
                       .arg(relation)
                       .arg(c.metadata.value(QStringLiteral("relation_confidence"))
                                .toFloat(), 0, 'f', 2);
        }
        if (!c.textContent.isEmpty())
            out += QString::fromUtf8("相关内容：%1\n\n").arg(c.textContent.left(200));
        ++idx;
    }
    return out;
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
    if (!playerOp && m_qaCache && !m_activeVideoId.isEmpty()) {
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

        // P1: 用采样计划的时间范围约束检索
        if (!samplingPlan.timeRanges.isEmpty()
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
    enrichedCtx.retrievalEvidence = formatRetrievalEvidence(m_retrievedEvidence);
    enrichedCtx.currentPositionMs = m_currentPlayerPosMs;

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

    m_orchestrator->runQuery(convId, question, userFrames, enrichedCtx,
        [this](const QString& delta) {
            if (m_onProgress) m_onProgress(delta);
        },
        [this, convId, question, userFrames, enrichedCtx]
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
                    retryCtx.retrievalEvidence = formatRetrievalEvidence(m_retrievedEvidence);

                    // 在证据中附加反思反馈，告知 LLM 上次答案的问题
                    retryCtx.retrievalEvidence += QStringLiteral(
                        "\n# 反思反馈（上次回答存在以下问题，请修正）\n%1\n")
                                                      .arg(rr.fixSuggestion);

                    m_orchestrator->runQuery(convId, question, userFrames, retryCtx,
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

                            // 缓存
                            if (!isPlayerOp(m_currentQuestion)
                                && m_qaCache && !m_activeVideoId.isEmpty()
                                && retryResult.confidence >= 0.6f) {
                                m_qaCache->cache(m_activeVideoId,
                                                  m_currentQuestion, retryAnswer,
                                                  retryResult.confidence);
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

            // 缓存 QA（播放器操作不缓存，避免后续相似问题命中缓存而跳过工具执行）
            if (!isPlayerOp(m_currentQuestion)
                && m_qaCache && !m_activeVideoId.isEmpty()
                && result.confidence >= 0.6f) {
                m_qaCache->cache(m_activeVideoId,
                                  m_currentQuestion, answer,
                                  result.confidence);
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

    // 从预设加载视频 QA 工作流
    WorkflowGraph graph = m_workflowFactory->fromFile(
        QStringLiteral(":/workflow/presets/video_qa.json"));

    QString validateError;
    if (!graph.validate(&validateError)) {
        // 验证失败，回退到传统模式
        qWarning() << "[VideoAgent] Workflow graph invalid:" << validateError
                   << "- falling back to legacy mode";
        emit stageChanged(QStringLiteral("PERCEIVE"));
        QSharedPointer<VideoRepresentation> repr;
        if (m_analysis) repr = m_analysis->representation(m_activeVideoPath);
        phasePerceive(question, repr, currentPlayerPosMs);
        if (m_retriever && !m_activeVideoId.isEmpty()) {
            VideoRAGRetriever::Constraints c;
            c.videoId = m_activeVideoId;
            m_retrievedEvidence = m_retriever->retrieve(question, c, 5);
        }
        phaseReasonAndAct(conversationId, question, userFrames, videoCtx);
        return;
    }

    // 设置 checkpoint
    QString workflowId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_workflowExecutor->setWorkflowId(workflowId);
    if (m_workflowCheckpoint) {
        m_workflowExecutor->setCheckpoint(m_workflowCheckpoint);
    }

    // 连接信号
    auto connCompleted = QObject::connect(
        m_workflowExecutor, &WorkflowExecutor::workflowCompleted,
        this, [this, conversationId, question](const WorkflowState& finalState) {
            AgentAnswer result;
            result.conversationId = conversationId;
            result.question = question;
            result.answer = finalState.get("answer").toString();
            result.confidence = finalState.get("confidence").toFloat();
            result.rounds = finalState.get("tool_rounds").toInt();

            // 缓存
            if (m_qaCache && !m_activeVideoId.isEmpty() && result.confidence >= 0.6f) {
                m_qaCache->cache(m_activeVideoId, question, result.answer, result.confidence);
            }

            finishAnswer(result);
        });

    auto connFailed = QObject::connect(
        m_workflowExecutor, &WorkflowExecutor::workflowFailed,
        this, [this](const QString& error) {
            failWith(error);
        });

    auto connProgress = QObject::connect(
        m_workflowExecutor, &WorkflowExecutor::streamingChunk,
        this, [this](const QString& chunk) {
            if (m_onProgress) m_onProgress(chunk);
        });

    auto connNode = QObject::connect(
        m_workflowExecutor, &WorkflowExecutor::nodeEntered,
        this, [this](const QString& nodeId) {
            emit stageChanged(nodeId.toUpper());
        });

    // 工作流完成后断开信号
    QObject::connect(m_workflowExecutor, &WorkflowExecutor::workflowCompleted,
                     this, [connCompleted, connFailed, connProgress, connNode]() {
        QObject::disconnect(connCompleted);
        QObject::disconnect(connFailed);
        QObject::disconnect(connProgress);
        QObject::disconnect(connNode);
    });
    QObject::connect(m_workflowExecutor, &WorkflowExecutor::workflowFailed,
                     this, [connCompleted, connFailed, connProgress, connNode]() {
        QObject::disconnect(connCompleted);
        QObject::disconnect(connFailed);
        QObject::disconnect(connProgress);
        QObject::disconnect(connNode);
    });

    // 启动工作流
    m_workflowExecutor->run(graph, std::move(state));
}
