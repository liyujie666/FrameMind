#include "service/rag/qa_cache_manager.h"

#include "service/rag/video_rag_store.h"

#include <QUuid>
#include <QVariant>
#include <QDateTime>

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/embedding_service.h"
#endif

QACacheManager::QACacheManager(VideoRAGStore* store,
                               EmbeddingService* embedder,
                               QObject* parent)
    : QObject(parent)
    , m_store(store)
    , m_embedder(embedder)
{
}

std::vector<float> QACacheManager::encodeQuery(const QString& question) const
{
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()) {
        return m_embedder->embedQuery(question);
    }
#endif
    (void)question;
    return {};
}

void QACacheManager::cache(const QString& videoId,
                           const QString& question,
                           const QString& answer,
                           float confidence,
                           const QVector<int>& evidenceSceneIds)
{
    if (!m_store || videoId.isEmpty() || question.trimmed().isEmpty()
        || answer.trimmed().isEmpty() || confidence < 0.7f || evidenceSceneIds.isEmpty()) {
        return;
    }

    const std::vector<float> emb = encodeQuery(question);
    if (emb.empty()) return;    // 无 embedding 不缓存（不能被检索）

    VideoChunk c;
    c.chunkId       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.videoId       = videoId;
    c.chunkType     = VideoChunk::QAcache;
    // start/end 用 0..duration 兜底；调用方若知具体范围可后续更新
    c.startMs       = 0;
    c.endMs         = 0;
    c.textContent   = QStringLiteral("Q: %1\nA: %2").arg(question, answer);
    c.textEmbedding = emb;

    QVariantMap meta;
    meta.insert(QStringLiteral("question"), question);
    meta.insert(QStringLiteral("answer"), answer);
    meta.insert(QStringLiteral("confidence"), confidence);
    meta.insert(QStringLiteral("embedding_model_id"), QStringLiteral("bge_text"));
    meta.insert(QStringLiteral("embedding_version"), QStringLiteral("query_v2"));
    meta.insert(QStringLiteral("cached_at"),
                QDateTime::currentDateTime().toString(Qt::ISODate));
    QVariantList sceneList;
    for (int id : evidenceSceneIds) sceneList << id;
    meta.insert(QStringLiteral("evidence_scene_ids"), sceneList);
    c.metadata = std::move(meta);

    m_store->insertChunk(VideoRAGStore::QACache, c);
}

std::optional<QACacheManager::CachedAnswer> QACacheManager::tryAnswer(
    const QString& videoId, const QString& question)
{
    if (!m_store) return std::nullopt;
    const std::vector<float> emb = encodeQuery(question);
    if (emb.empty()) return std::nullopt;

    VideoRAGStore::Filter filter;
    filter.videoId  = videoId;
    filter.expectedEmbeddingModelId = QStringLiteral("bge_text");
    filter.expectedEmbeddingVersion = QStringLiteral("query_v2");
    filter.minScore = m_threshold;

    const auto results = m_store->search(VideoRAGStore::QACache, emb, filter, 1);
    if (results.isEmpty()) return std::nullopt;

    const VideoChunk& c = results.first().first;
    const float sim = results.first().second;
    const auto evidenceIds = c.metadata.value(QStringLiteral("evidence_scene_ids")).toList();
    const float originalConfidence = c.metadata.value(QStringLiteral("confidence")).toFloat();
    const QString modelId = c.metadata.value(QStringLiteral("embedding_model_id")).toString();
    const QString modelVersion = c.metadata.value(QStringLiteral("embedding_version")).toString();
    const QDateTime cachedAt = QDateTime::fromString(
        c.metadata.value(QStringLiteral("cached_at")).toString(), Qt::ISODate);
    const bool expired = m_maxAgeDays > 0 && (!cachedAt.isValid()
        || cachedAt < QDateTime::currentDateTime().addDays(-m_maxAgeDays));
    if (sim < m_threshold || evidenceIds.isEmpty() || originalConfidence < 0.7f
        || modelId != QLatin1String("bge_text")
        || modelVersion != QLatin1String("query_v2") || expired) {
        return std::nullopt;
    }

    CachedAnswer ans;
    ans.question           = c.metadata.value(QStringLiteral("question")).toString();
    ans.answer             = c.metadata.value(QStringLiteral("answer")).toString();
    ans.originalConfidence = c.metadata.value(QStringLiteral("confidence")).toFloat();
    ans.similarity         = sim;
    ans.startMs            = c.startMs;
    ans.endMs              = c.endMs;
    const auto ids = c.metadata.value(QStringLiteral("evidence_scene_ids")).toList();
    for (const auto& v : ids) ans.evidenceSceneIds.append(v.toInt());
    return ans;
}

void QACacheManager::clearVideo(const QString& videoId)
{
    if (!m_store) return;
    const auto list = m_store->listChunks(VideoRAGStore::QACache, videoId);
    for (const auto& c : list) {
        m_store->removeChunk(VideoRAGStore::QACache, c.chunkId);
    }
}
