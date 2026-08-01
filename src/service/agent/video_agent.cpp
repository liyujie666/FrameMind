#include "service/agent/video_agent.h"

#include "service/agentservice.h"
#include "service/agent/video_analysis_service.h"
#include "service/agent/tool_orchestrator.h"
#include "service/agent/perception_strategy.h"
#include "service/agent/reflection_engine.h"
#include "service/rag/video_rag_retriever.h"
#include "service/rag/qa_cache_manager.h"
#include "service/rag/entity_tracker.h"
#include "model/video_representation.h"

#include <QDebug>
#include <QJsonObject>
#include <QRegularExpression>

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
    m_onProgress = std::move(onProgress);
    m_onDone     = std::move(onDone);
    m_onError    = std::move(onError);

    emit stageChanged(QStringLiteral("PERCEIVE"));

    // === 快速路径：QA 缓存命中 ===
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

    // === PERCEIVE: 感知策略 ===
    QSharedPointer<VideoRepresentation> repr;
    if (m_analysis) repr = m_analysis->representation(m_activeVideoPath);

    phasePerceive(question, repr, currentPlayerPosMs);

    // === REPRESENT: RAG 检索作为上下文补充 ===
    if (m_retriever && !m_activeVideoId.isEmpty()) {
        VideoRAGRetriever::Constraints c;
        c.videoId = m_activeVideoId;
        m_retrievedEvidence = m_retriever->retrieve(question, c, 5);
        qDebug() << "[VideoAgent] RAG retrieve"
                 << "videoId=" << m_activeVideoId
                 << "hits=" << m_retrievedEvidence.size();
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
    qDebug() << "[VideoAgent] inject context"
             << "evidenceChars=" << enrichedCtx.retrievalEvidence.size()
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
        [this](const QString& answer,
                const QVector<ToolResult>& toolTrace, int rounds) {
            emit stageChanged(QStringLiteral("REFLECT"));
            phaseReflect(answer, toolTrace);

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

void VideoAgent::phaseReflect(const QString& /*answer*/,
                                const QVector<ToolResult>& /*toolTrace*/)
{
    // 反思在 phaseReasonAndAct 的 onDone 里同步执行
    // 这里保留占位以便未来做异步反思
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
