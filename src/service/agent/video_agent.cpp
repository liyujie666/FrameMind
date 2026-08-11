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
    const QString q = question.trimmed();
    static const QRegularExpression directAction(
        QStringLiteral(
            u"(seek|go\\s*to|jump\\s*to|跳转|跳到|定位到|快进|快退|"
            u"调到|切到|移到|拖到|播放到)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression playbackCommand(
        QStringLiteral(
            u"(^\\s*(play|pause)\\s*[!！。.]?$|"
            u"(请|帮我|麻烦|立即|现在|开始|继续|停止).{0,8}(播放|暂停)|"
            u"(播放|暂停)(一下|视频|当前视频)?\\s*[!！。.]?$)"),
        QRegularExpression::CaseInsensitiveOption);
    return directAction.match(q).hasMatch()
           || playbackCommand.match(q).hasMatch();
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

QString formatSamplingPlan(const SamplingPlan& plan,
                           const SufficiencyCheck& sufficiency)
{
    QStringList lines;
    lines << QStringLiteral("信息充分性: %1")
                 .arg(sufficiency.isEnough ? QStringLiteral("充分")
                                           : QStringLiteral("不足"));
    if (!sufficiency.reason.isEmpty())
        lines << QStringLiteral("缺口: %1").arg(sufficiency.reason);
    if (!sufficiency.suggestedAction.isEmpty())
        lines << QStringLiteral("建议动作: %1").arg(sufficiency.suggestedAction);
    if (!plan.needNewPerception) {
        lines << QStringLiteral("无需额外视觉采样，先基于现有证据回答；证据冲突时再调用工具验证。");
        return lines.join(QLatin1Char('\n'));
    }

    lines << QStringLiteral("需要额外感知，帧预算: %1").arg(plan.frameBudget);
    if (!plan.focusHint.isEmpty())
        lines << QStringLiteral("分析关注点: %1").arg(plan.focusHint);
    for (int i = 0; i < plan.exactTimestampsMs.size(); ++i) {
        lines << QStringLiteral("步骤 %1: 调用 seek_and_analyze，timestamp_ms=%2")
                     .arg(i + 1).arg(plan.exactTimestampsMs.at(i));
    }
    const int remaining = qMax(1, plan.frameBudget
                                      - plan.exactTimestampsMs.size());
    const int perRange = plan.timeRanges.isEmpty()
                             ? remaining
                             : qMax(2, remaining / plan.timeRanges.size());
    for (int i = 0; i < plan.timeRanges.size(); ++i) {
        const auto range = plan.timeRanges.at(i);
        lines << QStringLiteral(
                     "步骤 %1: 调用 analyze_time_range，start_ms=%2，end_ms=%3，sample_count=%4")
                     .arg(lines.size() + 1).arg(range.first).arg(range.second)
                     .arg(qMin(perRange, 10));
    }
    lines << QStringLiteral(
        "观察每一步结果；若候选不明确，先调用 search_video_content 缩小区间；"
        "若失败则修正参数或改用 get_scene_info/get_transcript。信息充分后停止调用并回答。");
    return lines.join(QLatin1Char('\n'));
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
    m_reflectionAttempts = 0;
    m_totalRounds = 0;
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

    const SamplingPlan samplingPlan =
        phasePerceive(question, repr, currentPlayerPosMs);

    // === REPRESENT: RAG 检索作为上下文补充 ===
    // 播放器命令属于副作用操作，不检索知识证据，避免历史 QA 干扰工具决策。
    if (!playerOp && m_retriever && !m_activeVideoId.isEmpty()) {
        VideoRAGRetriever::Constraints c;
        c.videoId = m_activeVideoId;
        m_retrievedEvidence = m_retriever->retrieve(question, c, 5);
        qDebug() << "[VideoAgent] RAG retrieve"
                 << "videoId=" << m_activeVideoId
                 << "hits=" << m_retrievedEvidence.size();
    }

    // === REASON + ACT: 通过 ToolOrchestrator 让 LLM 决定是否调工具 ===
    VideoContext plannedCtx = videoCtx;
    if (!playerOp && m_perception) {
        const SufficiencyCheck sufficiency =
            m_perception->checkSufficiency(question, repr);
        plannedCtx.agentPlan = formatSamplingPlan(samplingPlan, sufficiency);
    } else if (playerOp) {
        plannedCtx.agentPlan = QStringLiteral(
            "这是播放器副作用指令，必须调用 control_player。"
            "不得使用历史回答或视频知识库替代实际执行。工具成功后简短确认。");
    }
    phaseReasonAndAct(conversationId, question, userFrames, plannedCtx);
}

void VideoAgent::cancel()
{
    if (m_orchestrator) m_orchestrator->cancel();
    m_busy = false;
}

// ============================================================
// 阶段实现
// ============================================================

SamplingPlan VideoAgent::phasePerceive(const QString& question,
                                           QSharedPointer<VideoRepresentation> repr,
                                           int64_t currentPosMs)
{
    if (!m_perception) return {};
    const SamplingPlan plan =
        m_perception->decideSampling(question, repr, currentPosMs);
    if (plan.needNewPerception) {
        qDebug() << "[VideoAgent] SamplingPlan: density=" << int(plan.density)
                 << "budget=" << plan.frameBudget
                 << "ranges=" << plan.timeRanges.size();
    }
    return plan;
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
    
    // 检测当前模型是否支持 Tool Calling
    const bool supportsTools = m_agent ? m_agent->currentModelSupportsToolCalling() : true;
    
    // 播放器操作判断
    const bool isPlayerOperation = isPlayerOp(question);
    
    // 如果是播放器操作但模型不支持工具调用，返回友好提示
    if (isPlayerOperation && !supportsTools) {
        const QString hint = QStringLiteral(
            "当前使用的视觉模型（qwen-vl-max等）不支持播放器控制功能。\n\n"
            "建议方案：\n"
            "1. 切换到支持工具调用的模型（如 qwen-plus、qwen-max）来使用播放器控制\n"
            "2. 或使用播放器界面的控制按钮手动操作\n\n"
            "视觉模型专注于理解画面内容，而播放器控制需要纯文本模型的工具调用能力。");
        failWith(hint);
        return;
    }
    
    emit stageChanged(QStringLiteral("REASON+ACT"));

    VideoContext enrichedCtx = ctx;
    enrichedCtx.retrievalEvidence = formatRetrievalEvidence(m_retrievedEvidence);
    enrichedCtx.currentPositionMs = m_currentPlayerPosMs;
    qDebug() << "[VideoAgent] inject context"
             << "evidenceChars=" << enrichedCtx.retrievalEvidence.size()
             << "sceneOverviewChars=" << enrichedCtx.sceneOverview.size()
             << "supportsTools=" << supportsTools;

    // 播放器操作强制指定 tool_choice，防止 LM 绕过工具直接输出文字
    QJsonValue toolChoice = QJsonValue(QStringLiteral("auto"));
    if (isPlayerOperation && supportsTools) {
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
        [this, enrichedCtx](const QString& answer,
                const QVector<ToolResult>& toolTrace, int rounds) {
            handleReasoningResult(answer, toolTrace, rounds, enrichedCtx);
        },
        [this](const QString& err) { failWith(err); },
        toolChoice);
}

void VideoAgent::handleReasoningResult(
    const QString& answer, const QVector<ToolResult>& toolTrace,
    int rounds, const VideoContext& context)
{
    emit stageChanged(QStringLiteral("REFLECT"));
    m_totalRounds += rounds;

    ReflectionResult reflection;
    reflection.valid = true;
    reflection.confidence = 0.8f;
    if (m_reflection) {
        auto repr = m_analysis ? m_analysis->representation(m_activeVideoPath)
                               : QSharedPointer<VideoRepresentation>();
        reflection = m_reflection->reflect(answer, m_retrievedEvidence,
                                           repr, toolTrace);
        for (const auto& issue : reflection.issues)
            emit reflectionIssueFound(issue.detail);
    }

    // 副作用操作不能在反思轮重复执行；知识问答最多自动修正一次。
    if (!isPlayerOp(m_currentQuestion) && !reflection.valid
        && m_reflectionAttempts < 1 && m_orchestrator) {
        ++m_reflectionAttempts;
        VideoContext retryContext = context;
        retryContext.agentPlan = QStringLiteral(
            "上一次回答未通过校验。问题：%1。"
            "请优先调用最少量的工具验证这些问题，然后输出修正后的完整最终答案；"
            "不得复述错误答案。")
            .arg(reflection.fixSuggestion);
        m_orchestrator->runQuery(
            m_currentConvId,
            QStringLiteral("请重新检查并修正对原问题「%1」的回答。上一答案：%2")
                .arg(m_currentQuestion, answer.left(1200)),
            {}, retryContext,
            [this](const QString& delta) {
                if (m_onProgress) m_onProgress(delta);
            },
            [this, retryContext](const QString& corrected,
                                  const QVector<ToolResult>& retryTrace,
                                  int retryRounds) {
                handleReasoningResult(corrected, retryTrace,
                                      retryRounds, retryContext);
            },
            [this](const QString& error) { failWith(error); },
            QJsonValue(QStringLiteral("auto")));
        return;
    }

    AgentAnswer result;
    result.conversationId = m_currentConvId;
    result.question = m_currentQuestion;
    result.answer = answer;
    result.rounds = m_totalRounds;
    result.evidence = m_retrievedEvidence;
    result.confidence = reflection.confidence;
    for (const ToolResult& item : toolTrace)
        result.toolCallsTrace.append(item.toolName);

    if (!isPlayerOp(m_currentQuestion) && m_qaCache
        && !m_activeVideoId.isEmpty() && reflection.valid
        && result.confidence >= 0.6f) {
        m_qaCache->cache(m_activeVideoId, m_currentQuestion,
                         answer, result.confidence);
    }
    finishAnswer(result);
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
