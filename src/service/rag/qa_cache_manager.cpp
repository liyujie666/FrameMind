#include "service/rag/qa_cache_manager.h"

#include "service/rag/video_rag_store.h"

#include <QUuid>
#include <QVariant>
#include <QDateTime>
#include <QRegularExpression>

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

bool QACacheManager::isCacheableQuestion(const QString& question)
{
    const QString q = question.trimmed();
    if (q.isEmpty()) return false;

    // 副作用指令必须每次重新执行，不能把上一次的确认文本当作答案复用。
    static const QRegularExpression sideEffectIntent(
        QStringLiteral(
            u"(seek|go\\s*to|jump\\s*to|^\\s*(play|pause)\\s*[!！。.]?$|"
            u"跳转|跳到|定位到|快进|快退|继续播放|停止播放|播放到|"
            u"(请|帮我|麻烦|立即|现在|开始|继续|停止).{0,8}(播放|暂停)|"
            u"调到|切到|移到|拖到|音量|静音|倍速|截图|截屏)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forceFresh(
        QStringLiteral(u"(重新|再一次|再分析|重新分析|重新检查|实时|当前|现在)"),
        QRegularExpression::CaseInsensitiveOption);
    return !sideEffectIntent.match(q).hasMatch()
           && !forceFresh.match(q).hasMatch();
}

std::vector<float> QACacheManager::encodeQuery(const QString& question) const
{
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()) {
        return m_embedder->embed(question);
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
    if (!m_store || !isCacheableQuestion(question)) return;

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
    if (!m_store || !isCacheableQuestion(question)) return std::nullopt;
    const std::vector<float> emb = encodeQuery(question);
    if (emb.empty()) return std::nullopt;

    VideoRAGStore::Filter filter;
    filter.videoId  = videoId;
    filter.minScore = m_threshold;

    const auto results = m_store->search(VideoRAGStore::QACache, emb, filter, 1);
    if (results.isEmpty()) return std::nullopt;

    const VideoChunk& c = results.first().first;
    const float sim = results.first().second;
    if (sim < m_threshold) return std::nullopt;

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
