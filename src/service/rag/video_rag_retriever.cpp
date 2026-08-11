#include "service/rag/video_rag_retriever.h"

#include "service/rag/video_rag_store.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/clip_service.h"
#  include "service/embedding_service.h"
#endif

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

int64_t parseClockMs(const QRegularExpressionMatch& match)
{
    const int first = match.captured(1).isEmpty() ? 0 : match.captured(1).toInt();
    const int second = match.captured(2).toInt();
    const int third = match.captured(3).toInt();
    if (second >= 60 || third >= 60) return -1;
    return (static_cast<int64_t>(first) * 3600 + second * 60 + third) * 1000;
}

QString evidenceLabel(const RetrievalResult& result)
{
    const QString type = result.chunk.metadata.value(QStringLiteral("evidence_type")).toString();
    return type.isEmpty() ? result.hitPath : type;
}

int64_t timeGapMs(const VideoChunk& left, const VideoChunk& right)
{
    if (left.endMs < right.startMs) return right.startMs - left.endMs;
    if (right.endMs < left.startMs) return left.startMs - right.endMs;
    return 0;
}

bool isDerivedFromSameTranscript(const VideoChunk& left, const VideoChunk& right)
{
    const QString leftSource = left.metadata.value(QStringLiteral("source")).toString();
    const QString rightSource = right.metadata.value(QStringLiteral("source")).toString();
    return leftSource.startsWith(QStringLiteral("whisper"))
        && rightSource.startsWith(QStringLiteral("whisper"));
}

bool isIndependentOrUnknownAudio(const VideoChunk& chunk)
{
    const QString evidenceType = chunk.metadata.value(QStringLiteral("evidence_type")).toString();
    if (evidenceType == QLatin1String("visual")) return false;
    const QString relation = chunk.metadata.value(QStringLiteral("audio_relation")).toString();
    return relation == QLatin1String("independent") || relation == QLatin1String("unknown");
}

} // namespace

VideoRAGRetriever::VideoRAGRetriever(VideoRAGStore* store, QObject* parent)
    : QObject(parent), m_store(store)
{
}

// ============================================================
// 证据类型偏好
// ============================================================

float VideoRAGRetriever::QueryIntent::evidenceWeight(
    VideoChunk::ChunkType t) const
{
    switch (t) {
    case VideoChunk::SceneSummary:   // 纯视觉证据
        if (prefersVisualEvidence) return 1.15f;
        if (prefersAudioEvidence)  return 0.75f;
        return 1.0f;
    case VideoChunk::SceneAudio:     // 场景音频摘要
        if (prefersAudioEvidence)  return 1.15f;
        if (prefersVisualEvidence) return 0.70f;
        return 0.95f;
    case VideoChunk::SpeechSegment:  // 原始台词，回答"谁说了什么"最直接
        if (prefersAudioEvidence)  return 1.20f;
        if (prefersVisualEvidence) return 0.70f;
        return 1.0f;
    case VideoChunk::SceneFused:     // 融合证据，叙事类问题的默认首选
        if (prefersFusedEvidence)  return 1.10f;
        return 1.0f;
    default:
        return 1.0f;
    }
}

void VideoRAGRetriever::applyEvidencePreference(
    QVector<RetrievalResult>& results, const QueryIntent& intent)
{
    for (RetrievalResult& r : results) {
        float weight = intent.evidenceWeight(r.chunk.chunkType);
        if (r.chunk.metadata.value(QStringLiteral("evidence_type")).toString()
            == QLatin1String("speech_semantic")) {
            if (intent.prefersAudioEvidence) weight = 1.15f;
            else if (intent.prefersVisualEvidence) weight = 0.75f;
        }
        r.score *= weight;
    }
    std::sort(results.begin(), results.end());
}

// ============================================================
// Query 意图分析（启发式）
// ============================================================

VideoRAGRetriever::QueryIntent VideoRAGRetriever::analyzeQuery(const QString& query) const
{
    QueryIntent intent;

    // 视觉关键词判断：颜色/形状/位置/画面主语
    static const QRegularExpression visualCues(
        QStringLiteral(u"(画面|颜色|穿着|红色|蓝色|绿色|黄色|白色|黑色|衣服|图案|"
                       u"左|右|上|下|物体|画面里|镜头|背景|人[^名字]|车|建筑)"));
    // 文本/语音关键词
    static const QRegularExpression textCues(
        QStringLiteral(u"(说|讲|台词|对话|字幕|文字|招牌|屏幕|界面|PPT|讨论|提到|口播|朗读|发言|转写)"));
    // 实体指代
    static const QRegularExpression entityCues(
        QStringLiteral(u"(那个|这个|刚才的|开头的|结尾的|之前的|视频里的.*[人|物])"));

    // 视觉与文本默认并行召回；关键词只用于调权，不能再把复合问题错误压缩为单路。
    const bool visualCueMatched = visualCues.match(query).hasMatch();
    const bool textCueMatched = textCues.match(query).hasMatch();
    intent.needsVisualSearch = true;
    intent.needsTextSearch = true;

    const auto entityMatch = entityCues.match(query);
    if (entityMatch.hasMatch()) {
        intent.hasEntityReference = true;
        intent.entityDesc = query;   // 简化：整句作为实体描述
    }

    // 关键词只调整两路权重，任一路都不会因未命中关键词而被关闭。
    if (visualCueMatched && !textCueMatched) {
        intent.weightVisual = 0.6; intent.weightText = 0.3; intent.weightEntity = 0.1;
    } else if (!visualCueMatched && textCueMatched) {
        intent.weightVisual = 0.3; intent.weightText = 0.6; intent.weightEntity = 0.1;
    } else {
        intent.weightVisual = 0.45; intent.weightText = 0.45; intent.weightEntity = 0.1;
    }

    // 证据类型偏好：画面细节类问题只信纯视觉证据，台词类问题优先语音证据
    static const QRegularExpression visualOnlyCues(
        QStringLiteral(u"(穿|衣服|颜色|长什么样|画面里有|画面中有|左上|右上|"
                       u"左下|右下|背景里|几个人|多少人|表情|手里拿)"));
    static const QRegularExpression audioOnlyCues(
        QStringLiteral(u"(说了什么|谁说|台词|对白|对话|字幕|提到|讲了|"
                       u"原话|口播|旁白|解说|念)"));
    static const QRegularExpression narrativeCues(
        QStringLiteral(u"(发生了什么|讲了什么|剧情|为什么|怎么回事|经过|"
                       u"起因|总结|概括|这一段|这段)"));

    intent.prefersVisualEvidence = visualOnlyCues.match(query).hasMatch();
    intent.prefersAudioEvidence  = audioOnlyCues.match(query).hasMatch();
    // 两侧都命中时不做压制，交给融合证据裁决
    if (intent.prefersVisualEvidence && intent.prefersAudioEvidence) {
        intent.prefersVisualEvidence = false;
        intent.prefersAudioEvidence  = false;
    }
    intent.prefersFusedEvidence =
        narrativeCues.match(query).hasMatch()
        || (!intent.prefersVisualEvidence && !intent.prefersAudioEvidence);

    return intent;
}

QueryPlan VideoRAGRetriever::compileQueryPlan(
    const QString& query, const Constraints& constraints) const
{
    QueryPlan plan;
    plan.normalizedQuery = query.simplified();
    const QueryIntent intent = analyzeQuery(plan.normalizedQuery);
    plan.retrieveText = intent.needsTextSearch;
    plan.retrieveVisual = intent.needsVisualSearch;
    plan.retrieveEntity = intent.hasEntityReference;
    plan.prefersVisualEvidence = intent.prefersVisualEvidence;
    plan.prefersAudioEvidence = intent.prefersAudioEvidence;
    plan.prefersFusedEvidence = intent.prefersFusedEvidence;
    plan.needsLocalVerification = plan.prefersVisualEvidence
        || plan.normalizedQuery.contains(QStringLiteral("几个"))
        || plan.normalizedQuery.contains(QStringLiteral("多少"))
        || plan.normalizedQuery.contains(QStringLiteral("什么时候"));

    // Tool 或调用方只要提供任一时间边界都优先，不能被问题文本中的时间词覆盖。
    if (constraints.startMsGte >= 0 || constraints.endMsLte >= 0) {
        plan.startMs = constraints.startMsGte;
        plan.endMs = constraints.endMsLte;
        plan.hasTemporalConstraint = true;
        plan.temporalHint = QStringLiteral("explicit_range");
        return plan;
    }

    static const QRegularExpression clockRange(
        QStringLiteral(R"(((?:\d{1,2}:)?\d{1,2}:\d{2})\s*(?:到|至|[-~～])\s*((?:\d{1,2}:)?\d{1,2}:\d{2}))"));
    static const QRegularExpression clock(QStringLiteral(R"((?:(\d{1,2}):)?(\d{1,2}):(\d{2}))"));
    static const QRegularExpression chineseMinuteSecond(
        QStringLiteral(R"(第?\s*(\d+)\s*分(?:钟)?\s*(?:(\d+)\s*秒?)?)"));

    const auto rangeMatch = clockRange.match(plan.normalizedQuery);
    if (rangeMatch.hasMatch()) {
        const auto first = clock.match(rangeMatch.captured(1));
        const auto second = clock.match(rangeMatch.captured(2));
        const int64_t start = first.hasMatch() ? parseClockMs(first) : -1;
        const int64_t end = second.hasMatch() ? parseClockMs(second) : -1;
        if (start >= 0 && end >= start) {
            if (constraints.videoDurationMs > 0
                && (start >= constraints.videoDurationMs || end <= 0)) {
                plan.hasTemporalConstraint = true;
                plan.temporalConstraintUnsatisfiable = true;
                plan.temporalHint = QStringLiteral("clock_range_outside_video");
                return plan;
            }
            plan.startMs = qMax<int64_t>(0, start);
            plan.endMs = constraints.videoDurationMs > 0
                ? qMin(constraints.videoDurationMs, end) : end;
            plan.hasTemporalConstraint = true;
            plan.temporalHint = QStringLiteral("clock_range");
            return plan;
        }
    }

    const auto setPointWindow = [&plan, &constraints](int64_t point, const QString& hint) {
        plan.hasTemporalConstraint = true;
        plan.temporalHint = hint;
        if (constraints.videoDurationMs > 0 && point > constraints.videoDurationMs) {
            plan.temporalConstraintUnsatisfiable = true;
            return;
        }
        plan.startMs = qMax<int64_t>(0, point - 5000);
        plan.endMs = constraints.videoDurationMs > 0
            ? qMin(constraints.videoDurationMs, point + 5000) : point + 5000;
    };

    const auto minuteSecond = chineseMinuteSecond.match(plan.normalizedQuery);
    if (minuteSecond.hasMatch()) {
        const int64_t point = (static_cast<int64_t>(minuteSecond.captured(1).toLongLong()) * 60
                              + minuteSecond.captured(2).toLongLong()) * 1000;
        setPointWindow(point, QStringLiteral("minute_second"));
        return plan;
    }

    const auto pointMatch = clock.match(plan.normalizedQuery);
    if (pointMatch.hasMatch()) {
        const int64_t point = parseClockMs(pointMatch);
        if (point >= 0) {
            setPointWindow(point, QStringLiteral("clock_point"));
            return plan;
        }
    }

    const int64_t duration = constraints.videoDurationMs;
    if (plan.normalizedQuery.contains(QStringLiteral("开头"))
        || plan.normalizedQuery.contains(QStringLiteral("一开始")) {
        plan.startMs = 0;
        plan.endMs = duration > 0 ? qMin<int64_t>(duration, 15000) : 15000;
        plan.hasTemporalConstraint = true;
        plan.temporalHint = QStringLiteral("beginning");
    } else if (duration > 0 && (plan.normalizedQuery.contains(QStringLiteral("结尾"))
                                 || plan.normalizedQuery.contains(QStringLiteral("最后")))) {
        plan.startMs = qMax<int64_t>(0, duration - 15000);
        plan.endMs = duration;
        plan.hasTemporalConstraint = true;
        plan.temporalHint = QStringLiteral("ending");
    } else if (duration > 0 && plan.normalizedQuery.contains(QStringLiteral("前半"))) {
        plan.startMs = 0;
        plan.endMs = duration / 2;
        plan.hasTemporalConstraint = true;
        plan.temporalHint = QStringLiteral("first_half");
    } else if (duration > 0 && plan.normalizedQuery.contains(QStringLiteral("后半"))) {
        plan.startMs = duration / 2;
        plan.endMs = duration;
        plan.hasTemporalConstraint = true;
        plan.temporalHint = QStringLiteral("second_half");
    } else if (constraints.currentPositionMs >= 0
               && (plan.normalizedQuery.contains(QStringLiteral("当前"))
                   || plan.normalizedQuery.contains(QStringLiteral("现在"))
                   || plan.normalizedQuery.contains(QStringLiteral("这里")))) {
        plan.startMs = qMax<int64_t>(0, constraints.currentPositionMs - 10000);
        plan.endMs = duration > 0 ? qMin(duration, constraints.currentPositionMs + 10000)
                                  : constraints.currentPositionMs + 10000;
        plan.hasTemporalConstraint = true;
        plan.temporalHint = QStringLiteral("current_position");
    }
    return plan;
}

// ============================================================
// 主入口
// ============================================================

QVector<RetrievalResult> VideoRAGRetriever::retrieve(const QString& query,
                                                      const Constraints& c,
                                                      int topK)
{
    if (!m_store || query.trimmed().isEmpty() || topK <= 0) return {};

    const QueryPlan plan = compileQueryPlan(query, c);
    if (plan.temporalConstraintUnsatisfiable) return {};
    const QueryIntent intent = analyzeQuery(plan.normalizedQuery);
    Constraints effective = c;
    if (plan.hasTimeRange()) {
        effective.startMsGte = plan.startMs;
        effective.endMsLte = plan.endMs;
    }

    QMap<QString, QVector<RetrievalResult>> perPath;
    QMap<QString, double> weights;
    const int candidateCount = qMax(topK, topK * plan.candidateMultiplier);

    if (plan.retrieveText) {
        auto textHits = textPathSearch(plan.normalizedQuery, effective, candidateCount);
        applyEvidencePreference(textHits, intent);
        perPath.insert(QStringLiteral("text_dense"), textHits);
        weights.insert(QStringLiteral("text_dense"), intent.weightText);

        auto lexicalHits = lexicalTextPathSearch(plan.normalizedQuery, effective, candidateCount);
        applyEvidencePreference(lexicalHits, intent);
        perPath.insert(QStringLiteral("text_lexical"), lexicalHits);
        weights.insert(QStringLiteral("text_lexical"), intent.weightText * 0.85);
    }
    if (plan.retrieveVisual) {
        perPath.insert(QStringLiteral("visual"),
                       visualPathSearch(plan.normalizedQuery, effective, candidateCount));
        weights.insert(QStringLiteral("visual"), intent.weightVisual);
    }
    if (plan.retrieveEntity && !intent.entityDesc.isEmpty()) {
        perPath.insert(QStringLiteral("entity"),
                       entityPathSearch(intent.entityDesc, effective, candidateCount));
        weights.insert(QStringLiteral("entity"), intent.weightEntity);
    }
    if (effective.preferPath == QLatin1String("text")) {
        weights[QStringLiteral("text_dense")] *= 1.25;
        weights[QStringLiteral("text_lexical")] *= 1.25;
    } else if (!effective.preferPath.isEmpty() && weights.contains(effective.preferPath)) {
        weights[effective.preferPath] *= 1.25;
    }

    auto fused = reciprocalRankFusion(perPath, weights, 60);
    fused = deduplicate(fused);
    fused = applyTemporalCorroboration(fused, plan);
    if (fused.size() > topK) fused.resize(topK);
    return fused;
}

// ============================================================
// 各路检索实现
// ============================================================

QVector<RetrievalResult> VideoRAGRetriever::textPathSearch(
    const QString& query, const Constraints& c, int topK)
{
    QVector<RetrievalResult> out;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (!m_embedder || !m_embedder->isReady()) return out;
    const auto emb = m_embedder->embedQuery(query);

    VideoRAGStore::Filter f;
    f.videoId    = c.videoId;
    f.startMsGte = c.startMsGte;
    f.endMsLte   = c.endMsLte;
    if (c.startMsGte >= 0 || c.endMsLte >= 0) {
        f.timeMatchMode = VideoRAGStore::Filter::Overlaps;
    }
    f.chunkType  = c.chunkType;
    f.expectedEmbeddingModelId = QStringLiteral("bge_text");
    f.expectedEmbeddingVersion = QStringLiteral("passage_v2");
    f.minScore   = c.minScore;
    const auto results = m_store->search(VideoRAGStore::TextSegments, emb, f, topK);
    for (const auto& [chunk, sim] : results) {
        RetrievalResult r;
        r.chunk = chunk;
        r.score = sim;
        r.hitPath = QStringLiteral("text");
        if (!chunk.keyframePath.isEmpty()) {
            QImage image(chunk.keyframePath);
            if (!image.isNull()) {
                r.keyframeThumb = image.scaled(
                    320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        out.append(r);
    }
#else
    (void)query; (void)c; (void)topK;
#endif
    return out;
}

QVector<RetrievalResult> VideoRAGRetriever::lexicalTextPathSearch(
    const QString& query, const Constraints& c, int topK)
{
    QVector<RetrievalResult> out;
    if (!m_store) return out;

    VideoRAGStore::Filter filter;
    filter.videoId = c.videoId;
    filter.startMsGte = c.startMsGte;
    filter.endMsLte = c.endMsLte;
    if (c.startMsGte >= 0 || c.endMsLte >= 0) {
        filter.timeMatchMode = VideoRAGStore::Filter::Overlaps;
    }
    filter.chunkType = c.chunkType;
    const auto results = m_store->searchLexical(
        VideoRAGStore::TextSegments, query, filter, topK);
    for (const auto& [chunk, score] : results) {
        RetrievalResult result;
        result.chunk = chunk;
        result.score = score;
        result.hitPath = QStringLiteral("text_lexical");
        if (!chunk.keyframePath.isEmpty()) {
            QImage image(chunk.keyframePath);
            if (!image.isNull()) {
                result.keyframeThumb = image.scaled(
                    320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        out.append(std::move(result));
    }
    return out;
}

QVector<RetrievalResult> VideoRAGRetriever::visualPathSearch(
    const QString& query, const Constraints& c, int topK)
{
    QVector<RetrievalResult> out;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (!m_clip || !m_clip->isReady()) return out;
    const auto emb = m_clip->encodeText(query);

    VideoRAGStore::Filter f;
    f.videoId    = c.videoId;
    f.startMsGte = c.startMsGte;
    f.endMsLte   = c.endMsLte;
    if (c.startMsGte >= 0 || c.endMsLte >= 0) {
        f.timeMatchMode = VideoRAGStore::Filter::Overlaps;
    }
    f.expectedEmbeddingModelId = QStringLiteral("clip_visual");
    f.expectedEmbeddingVersion = QStringLiteral("1");
    f.minScore   = c.minScore;
    const auto results = m_store->search(VideoRAGStore::VisualFrames, emb, f, topK);
    for (const auto& [chunk, sim] : results) {
        RetrievalResult r;
        r.chunk = chunk;
        r.score = sim;
        r.hitPath = QStringLiteral("visual");
        if (!chunk.keyframePath.isEmpty()) {
            QImage image(chunk.keyframePath);
            if (!image.isNull()) {
                r.keyframeThumb = image.scaled(
                    320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }
        out.append(r);
    }
#else
    (void)query; (void)c; (void)topK;
#endif
    return out;
}

QVector<RetrievalResult> VideoRAGRetriever::entityPathSearch(
    const QString& entityDesc, const Constraints& c, int topK)
{
    QVector<RetrievalResult> out;
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (!m_embedder || !m_embedder->isReady()) return out;
    const auto emb = m_embedder->embedQuery(entityDesc);

    VideoRAGStore::Filter f;
    f.videoId = c.videoId;
    f.expectedEmbeddingModelId = QStringLiteral("bge_text");
    f.expectedEmbeddingVersion = QStringLiteral("passage_v2");
    const auto results = m_store->search(VideoRAGStore::EntityProfiles, emb, f, topK);
    for (const auto& [chunk, sim] : results) {
        RetrievalResult r;
        r.chunk = chunk;
        r.score = sim;
        r.hitPath = QStringLiteral("entity");
        out.append(r);
    }
#else
    (void)entityDesc; (void)c; (void)topK;
#endif
    return out;
}

// ============================================================
// RRF 融合
// ============================================================

QVector<RetrievalResult> VideoRAGRetriever::reciprocalRankFusion(
    const QMap<QString, QVector<RetrievalResult>>& perPath,
    const QMap<QString, double>& weights, int k)
{
    // chunkId → (score, RetrievalResult&)
    QHash<QString, RetrievalResult> merged;
    QHash<QString, double> scores;

    for (auto it = perPath.constBegin(); it != perPath.constEnd(); ++it) {
        const double w = weights.value(it.key(), 1.0);
        const auto& list = it.value();
        for (int rank = 0; rank < list.size(); ++rank) {
            const auto& res = list[rank];
            const QString id = res.chunk.chunkId;
            const double addScore = w * (1.0 / (k + rank + 1));
            scores[id] += addScore;
            if (!merged.contains(id)) {
                merged.insert(id, res);
            } else {
                // 保留最高原始 score 的 hitPath，方便调试
                if (res.score > merged[id].score) merged[id].hitPath = res.hitPath;
            }
        }
    }

    // 用 RRF 融合分数覆写 score
    QVector<RetrievalResult> result;
    result.reserve(merged.size());
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        it.value().score = static_cast<float>(scores.value(it.key(), 0.0));
        result.append(it.value());
    }
    std::sort(result.begin(), result.end());
    return result;
}

QVector<RetrievalResult> VideoRAGRetriever::deduplicate(
    const QVector<RetrievalResult>& results) const
{
    // 同一场景的视觉 / 音频 / 融合证据时间范围完全相同但内容互补，
    // 只在"同一时间段 + 同一证据类型"时才认为重复
    QVector<RetrievalResult> out;
    for (const auto& r : results) {
        bool overlap = false;
        for (const auto& kept : out) {
            if (r.chunk.chunkType != kept.chunk.chunkType) continue;
            if (timeOverlapRatio(r.chunk, kept.chunk) > 0.7f) {
                overlap = true;
                break;
            }
        }
        if (!overlap) out.append(r);
    }
    return out;
}

QVector<RetrievalResult> VideoRAGRetriever::applyTemporalCorroboration(
    const QVector<RetrievalResult>& results, const QueryPlan& plan) const
{
    QVector<RetrievalResult> reranked = results;
    constexpr int64_t kNeighborWindowMs = 4000;

    for (int index = 0; index < reranked.size(); ++index) {
        RetrievalResult& candidate = reranked[index];
        QSet<QString> supportingIds;
        QSet<QString> supportingModalities;
        const QString primaryModality = evidenceLabel(candidate);
        const bool candidateAudioInvalid = isIndependentOrUnknownAudio(candidate.chunk);

        for (int otherIndex = 0; otherIndex < results.size(); ++otherIndex) {
            if (candidateAudioInvalid || index == otherIndex) continue;
            const RetrievalResult& other = results.at(otherIndex);
            if (timeGapMs(candidate.chunk, other.chunk) > kNeighborWindowMs) continue;
            if (isIndependentOrUnknownAudio(other.chunk)
                || isDerivedFromSameTranscript(candidate.chunk, other.chunk)) {
                continue;
            }

            const QString modality = evidenceLabel(other);
            if (modality == primaryModality) continue;
            supportingIds.insert(other.chunk.chunkId);
            supportingModalities.insert(modality);
        }

        if (!supportingIds.isEmpty()) {
            const int supportCount = qMin(3, supportingIds.size());
            candidate.score *= 1.0f + 0.10f * static_cast<float>(supportCount);
            QVariantList ids;
            QStringList modalities;
            for (const QString& id : supportingIds) ids.append(id);
            for (const QString& modality : supportingModalities) modalities.append(modality);
            std::sort(modalities.begin(), modalities.end());
            candidate.chunk.metadata.insert(QStringLiteral("corroborating_chunk_ids"), ids);
            candidate.chunk.metadata.insert(QStringLiteral("corroborating_modalities"), modalities);
        }

        if (plan.hasTimeRange()) {
            candidate.chunk.metadata.insert(QStringLiteral("query_temporal_hint"),
                                            plan.temporalHint);
        }
    }

    std::sort(reranked.begin(), reranked.end());
    return reranked;
}

float VideoRAGRetriever::timeOverlapRatio(const VideoChunk& a, const VideoChunk& b)
{
    const int64_t overlap = std::max<int64_t>(
        0, std::min(a.endMs, b.endMs) - std::max(a.startMs, b.startMs));
    const int64_t total = std::min(a.durationMs(), b.durationMs());
    if (total <= 0) return 0.0f;
    return static_cast<float>(overlap) / total;
}
