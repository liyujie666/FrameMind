#include "service/rag/entity_tracker.h"

#include "service/rag/video_rag_store.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/embedding_service.h"
#endif

#include <QRegularExpression>
#include <algorithm>

EntityTracker::EntityTracker(VideoRAGStore* store,
                              EmbeddingService* embedder,
                              QObject* parent)
    : QObject(parent), m_store(store), m_embedder(embedder)
{
}

// ============================================================
// 加载
// ============================================================

void EntityTracker::reload(const QString& videoId)
{
    if (!m_store) return;
    const auto list = m_store->listEntities(videoId);
    m_cache[videoId] = list;
    // 更新计数器：找出最大序号
    int maxCounter = 0;
    for (const auto& e : list) {
        // id 格式："person_5" / "object_3" ...
        const int idx = e.id.lastIndexOf('_');
        if (idx > 0) {
            bool ok = false;
            const int n = e.id.mid(idx + 1).toInt(&ok);
            if (ok && n > maxCounter) maxCounter = n;
        }
    }
    m_counters[videoId] = maxCounter + 1;
}

// ============================================================
// 匹配 & 注册
// ============================================================

std::vector<float> EntityTracker::encodeDesc(const QString& desc) const
{
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()) {
        return m_embedder->embed(desc);
    }
#endif
    (void)desc;
    return {};
}

QString EntityTracker::generateEntityId(EntityProfile::EntityType type) const
{
    // 简单串行 id：type_serial
    return QStringLiteral("%1_%2").arg(EntityProfile::typeToString(type));
}

std::optional<EntityProfile> EntityTracker::findMatching(
    const QString& videoId, const QString& description) const
{
    const auto embQuery = encodeDesc(description);
    if (embQuery.empty()) return std::nullopt;

    const auto& list = m_cache.value(videoId);
    if (list.isEmpty()) return std::nullopt;

    float bestSim = -1.0f;
    int   bestIdx = -1;
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].descriptionEmbedding.empty()) continue;
        const float sim = VideoRAGStore::cosineSimilarity(
            embQuery, list[i].descriptionEmbedding);
        if (sim > bestSim) { bestSim = sim; bestIdx = i; }
    }
    if (bestIdx < 0 || bestSim < m_similarity) return std::nullopt;
    return list[bestIdx];
}

QString EntityTracker::registerEntity(const QString& videoId,
                                       EntityProfile::EntityType type,
                                       const QString& description,
                                       int sceneId,
                                       int64_t timestampMs)
{
    // 保证缓存已加载
    if (!m_cache.contains(videoId)) reload(videoId);

    // 尝试匹配已有实体
    auto matched = findMatching(videoId, description);
    if (matched) {
        // 更新出现记录
        EntityAppearance ap;
        ap.sceneId     = sceneId;
        ap.timestampMs = timestampMs;
        ap.description = description;
        matched->appearances.append(ap);

        // 更新缓存
        for (auto& e : m_cache[videoId]) {
            if (e.id == matched->id) { e = *matched; break; }
        }
        if (m_store) m_store->upsertEntity(*matched);
        return matched->id;
    }

    // 新实体
    EntityProfile e;
    const int counter = m_counters.value(videoId, 1);
    e.id      = QStringLiteral("%1_%2").arg(EntityProfile::typeToString(type)).arg(counter);
    e.videoId = videoId;
    e.type    = type;
    e.primaryDescription = description;
    e.descriptionEmbedding = encodeDesc(description);

    EntityAppearance ap;
    ap.sceneId     = sceneId;
    ap.timestampMs = timestampMs;
    ap.description = description;
    e.appearances.append(ap);

    m_cache[videoId].append(e);
    m_counters[videoId] = counter + 1;
    if (m_store) m_store->upsertEntity(e);
    emit entityRegistered(e);
    return e.id;
}

// ============================================================
// 指代消解
// ============================================================

std::optional<EntityProfile> EntityTracker::resolveReference(
    const QString& videoId, const QString& userReference, int64_t anchorTsMs) const
{
    const auto& list = m_cache.value(videoId);
    if (list.isEmpty()) return std::nullopt;

    // 1) 时间线索
    // "刚才的" / "之前的" → 时间 < anchor 且最近
    static const QRegularExpression justNowRe(QStringLiteral(u"(刚才的?|之前的?|上一个)"));
    static const QRegularExpression beginRe(QStringLiteral(u"(开头的?|开始的?|一开始的?|最初的?)"));
    static const QRegularExpression endRe(QStringLiteral(u"(结尾的?|最后的?|结束的?)"));

    auto pickBestBySim = [this, &userReference, &list]() -> std::optional<EntityProfile> {
        const auto emb = encodeDesc(userReference);
        if (emb.empty()) return std::nullopt;
        float bestSim = -1.0f;
        int   bestIdx = -1;
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].descriptionEmbedding.empty()) continue;
            const float sim = VideoRAGStore::cosineSimilarity(
                emb, list[i].descriptionEmbedding);
            if (sim > bestSim) { bestSim = sim; bestIdx = i; }
        }
        if (bestIdx < 0 || bestSim < 0.6f) return std::nullopt;
        return list[bestIdx];
    };

    if (justNowRe.match(userReference).hasMatch() && anchorTsMs > 0) {
        // 找到 firstAppearMs < anchor 且最接近的
        int bestIdx = -1;
        int64_t bestDist = std::numeric_limits<int64_t>::max();
        for (int i = 0; i < list.size(); ++i) {
            const int64_t last = list[i].lastAppearMs();
            if (last < 0 || last > anchorTsMs) continue;
            const int64_t d = anchorTsMs - last;
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        if (bestIdx >= 0) return list[bestIdx];
    }

    if (beginRe.match(userReference).hasMatch()) {
        // 找到 firstAppearMs 最小的
        auto it = std::min_element(list.begin(), list.end(),
            [](const EntityProfile& a, const EntityProfile& b) {
                return a.firstAppearMs() < b.firstAppearMs();
            });
        if (it != list.end()) return *it;
    }

    if (endRe.match(userReference).hasMatch()) {
        auto it = std::max_element(list.begin(), list.end(),
            [](const EntityProfile& a, const EntityProfile& b) {
                return a.lastAppearMs() < b.lastAppearMs();
            });
        if (it != list.end()) return *it;
    }

    // 2) 语义相似度兜底
    return pickBestBySim();
}

// ============================================================
QVector<EntityProfile> EntityTracker::listEntities(const QString& videoId) const
{
    return m_cache.value(videoId);
}
