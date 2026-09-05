#include "service/agent/perception_strategy.h"

#include "service/rag/video_rag_retriever.h"

#include <QRegularExpression>
#include <algorithm>

PerceptionStrategy::PerceptionStrategy(VideoRAGRetriever* retriever, QObject* parent)
    : QObject(parent), m_retriever(retriever)
{
}

// ============================================================
// 问题分类（启发式，可后续改为 LLM few-shot 分类）
// ============================================================

QuestionType PerceptionStrategy::classifyQuestion(const QString& q) const
{
    // 关键词分类：命中优先级从高到低
    struct Rule { QuestionType type; const char* pattern; };
    static const Rule rules[] = {
        { QuestionType::CurrentFrame,        "(当前|现在|这一帧|眼前|画面里现在)" },
        { QuestionType::TemporalLocalization, "(什么时候|何时|多少秒|哪一段|哪个时间点|第几分钟)" },
        { QuestionType::Counting,             "(几个|多少个|几次|多少次|出现了几)" },
        { QuestionType::CausalReasoning,      "(为什么|为何|是因为|导致|原因)" },
        { QuestionType::Counterfactual,       "(如果|假如|要是)" },
        { QuestionType::Comparison,           "(区别|不同|对比|相比)" },
        { QuestionType::TemporalOrder,        "(之前|之后|先|后|顺序)" },
        { QuestionType::Duration,             "(持续|多久|多长时间)" },
        { QuestionType::EntityQuery,          "(那个|谁|什么人|穿|戴|是谁)" },
        { QuestionType::SpatialQuery,         "(左边|右边|上面|下面|角落|画面.*角)" },
        { QuestionType::ActionRecognition,    "(在做什么|干什么|动作)" },
        { QuestionType::GlobalSummary,        "(讲什么|讲了什么|大意|总结|概括|主题)" },
        { QuestionType::DetailDescription,    "(详细描述|具体描述|细节)" },
    };
    for (const auto& r : rules) {
        QRegularExpression re(QString::fromUtf8(r.pattern));
        if (re.match(q).hasMatch()) return r.type;
    }
    return QuestionType::Unknown;
}

// ============================================================
// 充分性检查（简单规则版：LLM 版可用于精细判断）
// ============================================================

SufficiencyCheck PerceptionStrategy::checkSufficiency(
    const QString& question, QSharedPointer<VideoRepresentation> repr)
{
    SufficiencyCheck result;
    if (!repr) {
        result.isEnough = false;
        result.reason = QStringLiteral("视频尚未索引");
        result.suggestedAction = QStringLiteral("wait_indexing");
        return result;
    }

    const QuestionType t = classifyQuestion(question);

    switch (t) {
    case QuestionType::GlobalSummary:
        result.isEnough = !repr->videoSummary.isEmpty();
        if (!result.isEnough) {
            result.suggestedAction = QStringLiteral("build_summary");
            result.reason = QStringLiteral("视频摘要尚未生成");
        }
        break;

    case QuestionType::CurrentFrame:
        result.isEnough = false;   // 总是需要一帧新的
        result.suggestedAction = QStringLiteral("seek_and_analyze");
        break;

    case QuestionType::TemporalLocalization:
    case QuestionType::EntityQuery:
    case QuestionType::ActionRecognition:
        // 依赖 RAG 检索是否有匹配
        if (m_retriever) {
            VideoRAGRetriever::Constraints c;
            c.videoId = repr->videoId;
            const auto hits = m_retriever->retrieve(question, c, 3);
            result.isEnough = !hits.isEmpty();
            if (!result.isEnough) {
                result.suggestedAction = QStringLiteral("search_and_verify");
                result.reason = QStringLiteral("RAG 未命中，需要新的视觉分析");
            }
        } else {
            result.isEnough = false;
            result.suggestedAction = QStringLiteral("search_and_verify");
        }
        break;

    default:
        // 兜底：Level 2 完成认为足够
        result.isEnough = (repr->level >= VideoRepresentation::Level2);
        if (!result.isEnough) result.suggestedAction = QStringLiteral("build_summary");
        break;
    }
    return result;
}

// ============================================================
// 采样规划入口
// ============================================================

SamplingPlan PerceptionStrategy::decideSampling(
    const QString& question, QSharedPointer<VideoRepresentation> repr,
    int64_t currentPlayerPosMs)
{
    // 1) 检查是否需要额外感知
    const auto suff = const_cast<PerceptionStrategy*>(this)->checkSufficiency(question, repr);
    if (suff.isEnough) {
        SamplingPlan plan;
        plan.needNewPerception = false;
        return plan;
    }

    // 2) 分类 → 规划
    const QuestionType type = classifyQuestion(question);
    switch (type) {
    case QuestionType::GlobalSummary:
        return planUniform(repr, SamplingPlan::Sparse);
    case QuestionType::TemporalLocalization:
        return planTargeted(question, repr);
    case QuestionType::EntityQuery:
    case QuestionType::ActionRecognition:
        return planEntityTracking(question, repr);
    case QuestionType::CausalReasoning:
        return planCausalContext(question, repr);
    case QuestionType::CurrentFrame:
        return planCurrentFrame(currentPlayerPosMs);
    case QuestionType::Counting:
    case QuestionType::Comparison:
        return planExhaustive(question, repr);
    default:
        return planUniform(repr, SamplingPlan::Medium);
    }
}

// ============================================================
// 各类计划实现
// ============================================================

int PerceptionStrategy::computeBudget(int64_t durationMs, const QString& purpose) const
{
    const double durationMin = durationMs / 60000.0;
    int budget = 8;
    if (purpose == QLatin1String("initial_overview")) {
        budget = std::max(8, static_cast<int>(durationMin * 3));
    } else if (purpose == QLatin1String("question_targeted")) {
        budget = 10;
    } else if (purpose == QLatin1String("full_analysis")) {
        budget = std::max(20, static_cast<int>(durationMin * 8));
    }
    return std::min(budget, 50);
}

SamplingPlan PerceptionStrategy::planUniform(
    QSharedPointer<VideoRepresentation> repr, SamplingPlan::Density density)
{
    SamplingPlan plan;
    plan.needNewPerception = true;
    plan.density = density;
    if (!repr) return plan;

    plan.frameBudget = computeBudget(repr->metadata.durationMs, QStringLiteral("initial_overview"));
    plan.timeRanges.append({ 0, repr->metadata.durationMs });
    return plan;
}

SamplingPlan PerceptionStrategy::planTargeted(
    const QString& question, QSharedPointer<VideoRepresentation> repr)
{
    SamplingPlan plan;
    plan.needNewPerception = true;
    plan.density = SamplingPlan::Dense;
    plan.frameBudget = 10;
    plan.focusHint = question;
    if (!repr) return plan;

    // 通过 RAG 检索得到候选区间
    if (m_retriever) {
        VideoRAGRetriever::Constraints c;
        c.videoId = repr->videoId;
        const auto hits = m_retriever->retrieve(question, c, 5);
        for (const auto& h : hits) {
            // 前后各扩 2s
            const int64_t s = std::max<int64_t>(0, h.chunk.startMs - 2000);
            const int64_t e = std::min<int64_t>(repr->metadata.durationMs,
                                                 h.chunk.endMs + 2000);
            plan.timeRanges.append({ s, e });
        }
    }
    if (plan.timeRanges.isEmpty()) {
        // 兜底：全视频稀疏
        plan.timeRanges.append({ 0, repr->metadata.durationMs });
        plan.density = SamplingPlan::Sparse;
    }
    return plan;
}

SamplingPlan PerceptionStrategy::planEntityTracking(
    const QString& question, QSharedPointer<VideoRepresentation> repr)
{
    // 类似 targeted，但采样密度更高（要跟踪实体动作）
    SamplingPlan plan = planTargeted(question, repr);
    plan.density = SamplingPlan::Dense;
    plan.frameBudget = 12;
    return plan;
}

SamplingPlan PerceptionStrategy::planCausalContext(
    const QString& question, QSharedPointer<VideoRepresentation> repr)
{
    SamplingPlan plan;
    plan.needNewPerception = true;
    plan.density = SamplingPlan::Dense;
    plan.frameBudget = 10;
    plan.focusHint = question;
    if (!repr) return plan;

    // 找到关键事件，往前扩 10s、往后扩 5s
    if (m_retriever) {
        VideoRAGRetriever::Constraints c;
        c.videoId = repr->videoId;
        const auto hits = m_retriever->retrieve(question, c, 1);
        if (!hits.isEmpty()) {
            const auto& h = hits.first();
            const int64_t s = std::max<int64_t>(0, h.chunk.startMs - 10000);
            const int64_t e = std::min<int64_t>(repr->metadata.durationMs,
                                                 h.chunk.endMs + 5000);
            plan.timeRanges.append({ s, e });
            return plan;
        }
    }
    plan.timeRanges.append({ 0, repr->metadata.durationMs });
    return plan;
}

SamplingPlan PerceptionStrategy::planCurrentFrame(int64_t currentPosMs)
{
    SamplingPlan plan;
    plan.needNewPerception = true;
    plan.density = SamplingPlan::Sparse;
    plan.frameBudget = 3;
    if (currentPosMs >= 0) {
        const int64_t win = 3000;
        plan.timeRanges.append({ std::max<int64_t>(0, currentPosMs - win),
                                  currentPosMs + win });
        plan.exactTimestampsMs.append(currentPosMs);
    }
    return plan;
}

SamplingPlan PerceptionStrategy::planExhaustive(
    const QString& question, QSharedPointer<VideoRepresentation> repr)
{
    SamplingPlan plan;
    plan.needNewPerception = true;
    plan.density = SamplingPlan::Dense;
    plan.focusHint = question;
    if (repr) {
        plan.frameBudget = computeBudget(repr->metadata.durationMs, QStringLiteral("full_analysis"));
        plan.timeRanges.append({ 0, repr->metadata.durationMs });
    } else {
        plan.frameBudget = 30;
    }
    return plan;
}
