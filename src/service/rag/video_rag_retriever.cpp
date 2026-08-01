#include "service/rag/video_rag_retriever.h"

#include "service/rag/video_rag_store.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/clip_service.h"
#  include "service/embedding_service.h"
#endif

#include <QRegularExpression>
#include <algorithm>

VideoRAGRetriever::VideoRAGRetriever(VideoRAGStore* store, QObject* parent)
    : QObject(parent), m_store(store)
{
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
        QStringLiteral(u"(说|讲|台词|对话|字幕|讨论|提到|口播|朗读|发言|转写)"));
    // 实体指代
    static const QRegularExpression entityCues(
        QStringLiteral(u"(那个|这个|刚才的|开头的|结尾的|之前的|视频里的.*[人|物])"));

    intent.needsVisualSearch = visualCues.match(query).hasMatch();
    intent.needsTextSearch   = textCues.match(query).hasMatch()
                               || !intent.needsVisualSearch;  // 兜底：至少走一路

    const auto entityMatch = entityCues.match(query);
    if (entityMatch.hasMatch()) {
        intent.hasEntityReference = true;
        intent.entityDesc = query;   // 简化：整句作为实体描述
    }

    // 权重调整
    if (intent.needsVisualSearch && !intent.needsTextSearch) {
        intent.weightVisual = 0.7; intent.weightText = 0.2; intent.weightEntity = 0.1;
    } else if (!intent.needsVisualSearch && intent.needsTextSearch) {
        intent.weightVisual = 0.2; intent.weightText = 0.7; intent.weightEntity = 0.1;
    } else {
        intent.weightVisual = 0.4; intent.weightText = 0.4; intent.weightEntity = 0.2;
    }
    return intent;
}

// ============================================================
// 主入口
// ============================================================

QVector<RetrievalResult> VideoRAGRetriever::retrieve(const QString& query,
                                                      const Constraints& c,
                                                      int topK)
{
    if (!m_store || query.isEmpty()) return {};

    const QueryIntent intent = analyzeQuery(query);
    QMap<QString, QVector<RetrievalResult>> perPath;
    QMap<QString, double> weights;

    if (intent.needsTextSearch) {
        perPath.insert(QStringLiteral("text"), textPathSearch(query, c, topK * 2));
        weights.insert(QStringLiteral("text"), intent.weightText);
    }
    if (intent.needsVisualSearch) {
        perPath.insert(QStringLiteral("visual"), visualPathSearch(query, c, topK * 2));
        weights.insert(QStringLiteral("visual"), intent.weightVisual);
    }
    if (intent.hasEntityReference && !intent.entityDesc.isEmpty()) {
        perPath.insert(QStringLiteral("entity"),
                       entityPathSearch(intent.entityDesc, c, 5));
        weights.insert(QStringLiteral("entity"), intent.weightEntity);
    }

    auto fused = reciprocalRankFusion(perPath, weights, 60);
    fused = deduplicate(fused);
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
    const auto emb = m_embedder->embed(query);

    VideoRAGStore::Filter f;
    f.videoId    = c.videoId;
    f.startMsGte = c.startMsGte;
    f.endMsLte   = c.endMsLte;
    f.chunkType  = c.chunkType;
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
    const auto emb = m_embedder->embed(entityDesc);

    VideoRAGStore::Filter f;
    f.videoId = c.videoId;
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
    QVector<RetrievalResult> out;
    for (const auto& r : results) {
        bool overlap = false;
        for (const auto& kept : out) {
            if (timeOverlapRatio(r.chunk, kept.chunk) > 0.7f) {
                overlap = true;
                break;
            }
        }
        if (!overlap) out.append(r);
    }
    return out;
}

float VideoRAGRetriever::timeOverlapRatio(const VideoChunk& a, const VideoChunk& b)
{
    const int64_t overlap = std::max<int64_t>(
        0, std::min(a.endMs, b.endMs) - std::max(a.startMs, b.startMs));
    const int64_t total = std::min(a.durationMs(), b.durationMs());
    if (total <= 0) return 0.0f;
    return static_cast<float>(overlap) / total;
}
