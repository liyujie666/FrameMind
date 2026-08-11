#include "service/rag/video_rag_store.h"

#include "infrastructure/databasemanager.h"

#include <QMutex>
#include <QMutexLocker>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
// 集合表名映射
QString colName(VideoRAGStore::Collection c)
{
    switch (c) {
    case VideoRAGStore::VisualFrames:   return QStringLiteral("visual_frames");
    case VideoRAGStore::TextSegments:   return QStringLiteral("text_segments");
    case VideoRAGStore::EntityProfiles: return QStringLiteral("entity_profiles");
    case VideoRAGStore::QACache:        return QStringLiteral("qa_cache");
    }
    return {};
}

QByteArray embeddingToBlob(const std::vector<float>& emb)
{
    return QByteArray(reinterpret_cast<const char*>(emb.data()),
                      static_cast<int>(emb.size() * sizeof(float)));
}

std::vector<float> blobToEmbedding(const QByteArray& blob)
{
    std::vector<float> out(blob.size() / sizeof(float));
    if (!out.empty()) {
        std::memcpy(out.data(), blob.constData(), out.size() * sizeof(float));
    }
    return out;
}

QString chunkTypeToString(VideoChunk::ChunkType t)
{
    switch (t) {
    case VideoChunk::SceneSummary:   return QStringLiteral("scene_summary");
    case VideoChunk::SpeechSegment:  return QStringLiteral("speech_segment");
    case VideoChunk::Event:          return QStringLiteral("event");
    case VideoChunk::FrameDesc:      return QStringLiteral("frame_desc");
    case VideoChunk::QAcache:        return QStringLiteral("qa_cache");
    case VideoChunk::SceneAudio:     return QStringLiteral("scene_audio");
    case VideoChunk::SceneFused:     return QStringLiteral("scene_fused");
    }
    return QStringLiteral("unknown");
}

VideoChunk::ChunkType chunkTypeFromString(const QString& s)
{
    if (s == QLatin1String("scene_summary"))  return VideoChunk::SceneSummary;
    if (s == QLatin1String("speech_segment")) return VideoChunk::SpeechSegment;
    if (s == QLatin1String("event"))          return VideoChunk::Event;
    if (s == QLatin1String("frame_desc"))     return VideoChunk::FrameDesc;
    if (s == QLatin1String("qa_cache"))       return VideoChunk::QAcache;
    if (s == QLatin1String("scene_audio"))    return VideoChunk::SceneAudio;
    if (s == QLatin1String("scene_fused"))    return VideoChunk::SceneFused;
    return VideoChunk::SceneSummary;
}

QSet<QString> lexicalTerms(const QString& text)
{
    const QString normalized = text.toLower().simplified();
    QSet<QString> terms;
    static const QRegularExpression asciiWords(QStringLiteral(R"([a-z0-9_]+)"));
    auto iterator = asciiWords.globalMatch(normalized);
    while (iterator.hasNext()) terms.insert(iterator.next().captured());

    QString cjk;
    for (const QChar character : normalized) {
        if (character.isLetterOrNumber() && character.unicode() >= 0x2E80) {
            cjk.append(character);
        } else {
            cjk.append(QLatin1Char(' '));
        }
    }
    const QStringList runs = cjk.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& run : runs) {
        if (run.size() == 1) {
            terms.insert(run);
        } else {
            for (int index = 0; index < run.size() - 1; ++index) {
                terms.insert(run.mid(index, 2));
            }
        }
    }
    return terms;
}

float lexicalScore(const QSet<QString>& queryTerms, const QString& content)
{
    if (queryTerms.isEmpty()) return 0.0f;
    const QSet<QString> documentTerms = lexicalTerms(content);
    int matches = 0;
    for (const QString& term : queryTerms) {
        if (documentTerms.contains(term)) ++matches;
    }
    return static_cast<float>(matches) / static_cast<float>(queryTerms.size());
}
} // namespace

// ================= Impl =================

struct VideoRAGStore::Impl {
    DatabaseManager* db = nullptr;

    // 内存索引：collection → chunkId → VideoChunk
    // 单机场景直接线性扫描，规模够用；后续替换 FAISS 时可平滑迁移
    QHash<Collection, QHash<QString, VideoChunk>> inMemory;

    // 记录当前已加载的 videoId 集合，避免重复 load
    QSet<QString> loadedVideos;

    QMutex mtx;
};

// ================= 生命周期 =================

VideoRAGStore::VideoRAGStore(DatabaseManager* db, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->db = db;
}

VideoRAGStore::~VideoRAGStore() = default;

bool VideoRAGStore::initialize()
{
    if (!d->db) {
        qWarning() << "[VideoRAGStore] DatabaseManager 未注入";
        return false;
    }

    // 建表：rag_chunks
    const QString createChunks = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS rag_chunks ("
        "  chunk_id      TEXT PRIMARY KEY,"
        "  collection    TEXT NOT NULL,"
        "  video_id      TEXT NOT NULL,"
        "  start_ms      INTEGER NOT NULL,"
        "  end_ms        INTEGER NOT NULL,"
        "  chunk_type    TEXT NOT NULL,"
        "  text_content  TEXT,"
        "  text_embedding    BLOB,"
        "  frame_embedding   BLOB,"
        "  keyframe_path TEXT,"
        "  metadata_json TEXT,"
        "  created_at    DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  accessed_at   DATETIME DEFAULT CURRENT_TIMESTAMP"
        ")");
    if (!d->db->exec(createChunks)) return false;

    d->db->exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_rag_chunks_video "
        "ON rag_chunks(video_id, collection)"));

    // 建表：rag_entities
    const QString createEntities = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS rag_entities ("
        "  entity_id           TEXT PRIMARY KEY,"
        "  video_id            TEXT NOT NULL,"
        "  entity_type         TEXT,"
        "  primary_description TEXT,"
        "  aliases_json        TEXT,"
        "  appearances_json    TEXT,"
        "  description_embedding BLOB,"
        "  created_at          DATETIME DEFAULT CURRENT_TIMESTAMP"
        ")");
    if (!d->db->exec(createEntities)) return false;

    d->db->exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_rag_entities_video "
        "ON rag_entities(video_id)"));

    return true;
}

// ================= 加载 / 失效 =================

void VideoRAGStore::loadVideo(const QString& videoId)
{
    QMutexLocker lock(&d->mtx);
    if (d->loadedVideos.contains(videoId)) return;
    if (!d->db) return;

    int count = 0;
    const auto rows = d->db->query(
        QStringLiteral("SELECT chunk_id, collection, video_id, start_ms, end_ms, "
                       "chunk_type, text_content, text_embedding, frame_embedding, "
                       "keyframe_path, metadata_json FROM rag_chunks "
                       "WHERE video_id = ?"),
        { videoId });

    for (const auto& row : rows) {
        VideoChunk c;
        c.chunkId       = row.value(QStringLiteral("chunk_id")).toString();
        c.videoId       = row.value(QStringLiteral("video_id")).toString();
        c.startMs       = row.value(QStringLiteral("start_ms")).toLongLong();
        c.endMs         = row.value(QStringLiteral("end_ms")).toLongLong();
        c.chunkType     = chunkTypeFromString(row.value(QStringLiteral("chunk_type")).toString());
        c.textContent   = row.value(QStringLiteral("text_content")).toString();
        c.textEmbedding = blobToEmbedding(row.value(QStringLiteral("text_embedding")).toByteArray());
        c.frameEmbedding= blobToEmbedding(row.value(QStringLiteral("frame_embedding")).toByteArray());
        c.keyframePath  = row.value(QStringLiteral("keyframe_path")).toString();

        const QString metaStr = row.value(QStringLiteral("metadata_json")).toString();
        if (!metaStr.isEmpty()) {
            c.metadata = QJsonDocument::fromJson(metaStr.toUtf8()).object().toVariantMap();
        }

        const QString colStr = row.value(QStringLiteral("collection")).toString();
        Collection col = TextSegments;
        if (colStr == QLatin1String("visual_frames"))   col = VisualFrames;
        else if (colStr == QLatin1String("entity_profiles")) col = EntityProfiles;
        else if (colStr == QLatin1String("qa_cache"))   col = QACache;

        d->inMemory[col][c.chunkId] = c;
        ++count;
    }

    d->loadedVideos.insert(videoId);
    emit videoIndexLoaded(videoId, count);
}

bool VideoRAGStore::hasIndexedContent(const QString& videoId) const
{
    QMutexLocker lock(&d->mtx);
    for (const Collection collection : {VisualFrames, TextSegments}) {
        const auto chunks = d->inMemory.constFind(collection);
        if (chunks == d->inMemory.constEnd()) continue;
        for (auto it = chunks->constBegin(); it != chunks->constEnd(); ++it) {
            if (it.value().videoId == videoId) return true;
        }
    }
    return false;
}

void VideoRAGStore::invalidateVideo(const QString& videoId)
{
    {
        QMutexLocker lock(&d->mtx);
        for (auto it = d->inMemory.begin(); it != d->inMemory.end(); ++it) {
            auto& map = it.value();
            for (auto cit = map.begin(); cit != map.end();) {
                if (cit.value().videoId == videoId) cit = map.erase(cit);
                else ++cit;
            }
        }
        d->loadedVideos.remove(videoId);
    }

    if (d->db) {
        d->db->exec(QStringLiteral("DELETE FROM rag_chunks WHERE video_id = ?"), { videoId });
        d->db->exec(QStringLiteral("DELETE FROM rag_entities WHERE video_id = ?"), { videoId });
    }
    emit videoIndexInvalidated(videoId);
}

void VideoRAGStore::cleanupStale(int maxAgeDays)
{
    if (!d->db) return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    // 简化：按 accessed_at 清理 chunks；对应视频的 entities 顺带清
    d->db->exec(
        QStringLiteral("DELETE FROM rag_chunks WHERE accessed_at < ?"),
        { cutoff });
}

// ================= 写入 =================

bool VideoRAGStore::insertChunk(Collection col, const VideoChunk& chunk)
{
    if (!chunk.isValid()) return false;
    // DatabaseManager 的 SQLite 连接属于主线程。Level-1 索引在工作线程运行时，
    // 统一封送到 Store 所在线程完成内存与 SQLite 的原子写入。
    if (QThread::currentThread() != thread()) {
        bool inserted = false;
        const VideoChunk copy = chunk;
        QMetaObject::invokeMethod(this, [this, col, copy, &inserted]() {
            inserted = insertChunk(col, copy);
        }, Qt::BlockingQueuedConnection);
        return inserted;
    }
    {
        QMutexLocker lock(&d->mtx);
        d->inMemory[col][chunk.chunkId] = chunk;
        d->loadedVideos.insert(chunk.videoId);
    }

    if (!d->db) return true;

    return d->db->exec(
        QStringLiteral(
            "INSERT OR REPLACE INTO rag_chunks "
            "(chunk_id, collection, video_id, start_ms, end_ms, chunk_type, "
            " text_content, text_embedding, frame_embedding, keyframe_path, metadata_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
        {
            chunk.chunkId,
            colName(col),
            chunk.videoId,
            static_cast<qlonglong>(chunk.startMs),
            static_cast<qlonglong>(chunk.endMs),
            chunkTypeToString(chunk.chunkType),
            chunk.textContent,
            embeddingToBlob(chunk.textEmbedding),
            embeddingToBlob(chunk.frameEmbedding),
            chunk.keyframePath,
            QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(chunk.metadata))
                              .toJson(QJsonDocument::Compact))
        });
}

bool VideoRAGStore::insertChunks(Collection col, const std::vector<VideoChunk>& chunks)
{
    bool ok = true;
    for (const auto& c : chunks) ok = insertChunk(col, c) && ok;
    return ok;
}

bool VideoRAGStore::removeVideoChunks(Collection col, const QString& videoId)
{
    if (videoId.isEmpty()) return false;
    {
        QMutexLocker lock(&d->mtx);
        auto& chunks = d->inMemory[col];
        for (auto it = chunks.begin(); it != chunks.end();) {
            if (it.value().videoId == videoId) it = chunks.erase(it);
            else ++it;
        }
    }
    if (!d->db) return true;
    return d->db->exec(
        QStringLiteral("DELETE FROM rag_chunks WHERE video_id = ? AND collection = ?"),
        { videoId, colName(col) });
}

bool VideoRAGStore::removeChunk(Collection col, const QString& chunkId)
{
    {
        QMutexLocker lock(&d->mtx);
        d->inMemory[col].remove(chunkId);
    }
    if (!d->db) return true;
    return d->db->exec(QStringLiteral("DELETE FROM rag_chunks WHERE chunk_id = ?"), { chunkId });
}

// ================= 检索 =================

QVector<QPair<VideoChunk, float>> VideoRAGStore::search(Collection col,
                                                         const std::vector<float>& queryVector,
                                                         const Filter& filter,
                                                         int topK)
{
    QVector<QPair<VideoChunk, float>> results;
    if (queryVector.empty() || topK <= 0) return results;

    QMutexLocker lock(&d->mtx);
    const auto it = d->inMemory.constFind(col);
    if (it == d->inMemory.constEnd()) return results;

    const auto& chunks = it.value();
    results.reserve(chunks.size());

    for (auto cit = chunks.constBegin(); cit != chunks.constEnd(); ++cit) {
        const VideoChunk& c = cit.value();

        // 过滤
        if (!filter.videoId.isEmpty() && c.videoId != filter.videoId) continue;
        if (filter.timeMatchMode == Filter::Overlaps) {
            if (filter.startMsGte >= 0 && c.endMs <= filter.startMsGte) continue;
            if (filter.endMsLte >= 0 && c.startMs >= filter.endMsLte) continue;
        }
        if (filter.timeMatchMode == Filter::FullyContained) {
            if (filter.startMsGte >= 0 && c.startMs < filter.startMsGte) continue;
            if (filter.endMsLte >= 0 && c.endMs > filter.endMsLte) continue;
        }
        if (static_cast<int>(filter.chunkType) >= 0 && c.chunkType != filter.chunkType) continue;

        // 向量空间由模型 ID/版本而非维度定义；未声明身份的旧 chunk 只在调用方
        // 未要求模型隔离时兼容检索，升级后的路径不会静默混用它们。
        const QString modelId = c.metadata.value(QStringLiteral("embedding_model_id")).toString();
        const QString modelVersion = c.metadata.value(QStringLiteral("embedding_version")).toString();
        if (!filter.expectedEmbeddingModelId.isEmpty()
            && modelId != filter.expectedEmbeddingModelId) continue;
        if (!filter.expectedEmbeddingVersion.isEmpty()
            && modelVersion != filter.expectedEmbeddingVersion) continue;

        // 选择用哪个向量
        const std::vector<float>& target =
            (col == VisualFrames) ? c.frameEmbedding : c.textEmbedding;
        if (target.empty() || target.size() != queryVector.size()) continue;

        const float sim = cosineSimilarity(queryVector, target);
        if (sim < filter.minScore) continue;

        results.append({ c, sim });
    }

    // 取 topK
    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (results.size() > topK) results.resize(topK);

    return results;
}

QVector<QPair<VideoChunk, float>> VideoRAGStore::searchLexical(
    Collection col, const QString& query, const Filter& filter, int topK)
{
    QVector<QPair<VideoChunk, float>> results;
    if (query.trimmed().isEmpty() || topK <= 0) return results;
    const QSet<QString> queryTerms = lexicalTerms(query);
    if (queryTerms.isEmpty()) return results;

    QMutexLocker lock(&d->mtx);
    const auto collection = d->inMemory.constFind(col);
    if (collection == d->inMemory.constEnd()) return results;

    results.reserve(collection->size());
    for (auto it = collection->constBegin(); it != collection->constEnd(); ++it) {
        const VideoChunk& chunk = it.value();
        if (!filter.videoId.isEmpty() && chunk.videoId != filter.videoId) continue;
        if (filter.timeMatchMode == Filter::Overlaps) {
            if (filter.startMsGte >= 0 && chunk.endMs <= filter.startMsGte) continue;
            if (filter.endMsLte >= 0 && chunk.startMs >= filter.endMsLte) continue;
        } else {
            if (filter.startMsGte >= 0 && chunk.startMs < filter.startMsGte) continue;
            if (filter.endMsLte >= 0 && chunk.endMs > filter.endMsLte) continue;
        }
        if (static_cast<int>(filter.chunkType) >= 0 && chunk.chunkType != filter.chunkType) continue;

        const float score = lexicalScore(queryTerms, chunk.textContent);
        if (score < filter.minScore || score <= 0.0f) continue;
        results.append({chunk, score});
    }

    std::sort(results.begin(), results.end(),
              [](const auto& left, const auto& right) {
                  if (!qFuzzyCompare(left.second + 1.0f, right.second + 1.0f)) {
                      return left.second > right.second;
                  }
                  if (left.first.startMs != right.first.startMs) {
                      return left.first.startMs < right.first.startMs;
                  }
                  return left.first.chunkId < right.first.chunkId;
              });
    if (results.size() > topK) results.resize(topK);
    return results;
}

VideoChunk VideoRAGStore::getChunk(Collection col, const QString& chunkId) const
{
    QMutexLocker lock(&d->mtx);
    const auto it = d->inMemory.constFind(col);
    if (it == d->inMemory.constEnd()) return {};
    return it.value().value(chunkId, {});
}

QVector<VideoChunk> VideoRAGStore::listChunks(Collection col, const QString& videoId) const
{
    QVector<VideoChunk> out;
    QMutexLocker lock(&d->mtx);
    const auto it = d->inMemory.constFind(col);
    if (it == d->inMemory.constEnd()) return out;
    for (const auto& c : it.value()) {
        if (c.videoId == videoId) out.append(c);
    }
    return out;
}

// ================= 实体 =================

bool VideoRAGStore::upsertEntity(const EntityProfile& entity)
{
    if (!entity.isValid() || !d->db) return false;
    if (QThread::currentThread() != thread()) {
        bool stored = false;
        const EntityProfile copy = entity;
        QMetaObject::invokeMethod(this, [this, copy, &stored]() {
            stored = upsertEntity(copy);
        }, Qt::BlockingQueuedConnection);
        return stored;
    }

    QJsonArray aliasesArr;
    for (const auto& a : entity.aliases) aliasesArr.append(a);

    QJsonArray appearArr;
    for (const auto& ap : entity.appearances) {
        QJsonObject o;
        o.insert(QStringLiteral("scene_id"), ap.sceneId);
        o.insert(QStringLiteral("timestamp_ms"), static_cast<qint64>(ap.timestampMs));
        o.insert(QStringLiteral("description"), ap.description);
        if (ap.bboxW > 0 && ap.bboxH > 0) {
            QJsonObject bbox;
            bbox.insert(QStringLiteral("x"), ap.bboxX);
            bbox.insert(QStringLiteral("y"), ap.bboxY);
            bbox.insert(QStringLiteral("w"), ap.bboxW);
            bbox.insert(QStringLiteral("h"), ap.bboxH);
            o.insert(QStringLiteral("bbox"), bbox);
        }
        appearArr.append(o);
    }

    const bool stored = d->db->exec(
        QStringLiteral(
            "INSERT OR REPLACE INTO rag_entities "
            "(entity_id, video_id, entity_type, primary_description, "
            " aliases_json, appearances_json, description_embedding) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)"),
        {
            entity.id,
            entity.videoId,
            EntityProfile::typeToString(entity.type),
            entity.primaryDescription,
            QString::fromUtf8(QJsonDocument(aliasesArr).toJson(QJsonDocument::Compact)),
            QString::fromUtf8(QJsonDocument(appearArr).toJson(QJsonDocument::Compact)),
            embeddingToBlob(entity.descriptionEmbedding)
        });
    if (!stored) return false;

    // EntityProfiles 是检索 collection；实体表承担持久化档案，chunk 承担 RAG 向量召回。
    VideoChunk chunk;
    chunk.chunkId = QStringLiteral("entity:") + entity.videoId + QLatin1Char(':') + entity.id;
    chunk.videoId = entity.videoId;
    chunk.startMs = entity.firstAppearMs() < 0 ? 0 : entity.firstAppearMs();
    chunk.endMs = entity.lastAppearMs() < chunk.startMs
        ? chunk.startMs + 1 : entity.lastAppearMs() + 1;
    chunk.chunkType = VideoChunk::Event;
    chunk.textContent = entity.primaryDescription;
    chunk.textEmbedding = entity.descriptionEmbedding;
    chunk.metadata.insert(QStringLiteral("entity_id"), entity.id);
    chunk.metadata.insert(QStringLiteral("entity_type"), EntityProfile::typeToString(entity.type));
    chunk.metadata.insert(QStringLiteral("embedding_model_id"), QStringLiteral("bge_text"));
    chunk.metadata.insert(QStringLiteral("embedding_version"), QStringLiteral("passage_v2"));
    return insertChunk(EntityProfiles, chunk);
}

QVector<EntityProfile> VideoRAGStore::listEntities(const QString& videoId) const
{
    QVector<EntityProfile> out;
    if (!d->db) return out;

    const auto rows = d->db->query(
        QStringLiteral(
            "SELECT entity_id, video_id, entity_type, primary_description, "
            "aliases_json, appearances_json, description_embedding "
            "FROM rag_entities WHERE video_id = ?"),
        { videoId });

    for (const auto& row : rows) {
        EntityProfile e;
        e.id = row.value(QStringLiteral("entity_id")).toString();
        e.videoId = row.value(QStringLiteral("video_id")).toString();
        const QString typeStr = row.value(QStringLiteral("entity_type")).toString();
        if      (typeStr == QLatin1String("person"))   e.type = EntityProfile::Person;
        else if (typeStr == QLatin1String("object"))   e.type = EntityProfile::Object;
        else if (typeStr == QLatin1String("location")) e.type = EntityProfile::Location;
        else if (typeStr == QLatin1String("text"))     e.type = EntityProfile::Text;
        else                                            e.type = EntityProfile::Unknown;
        e.primaryDescription = row.value(QStringLiteral("primary_description")).toString();

        const QJsonArray aliasesArr = QJsonDocument::fromJson(
            row.value(QStringLiteral("aliases_json")).toString().toUtf8()).array();
        for (const auto& v : aliasesArr) e.aliases << v.toString();

        const QJsonArray appearArr = QJsonDocument::fromJson(
            row.value(QStringLiteral("appearances_json")).toString().toUtf8()).array();
        for (const auto& v : appearArr) {
            const QJsonObject o = v.toObject();
            EntityAppearance a;
            a.sceneId     = o.value(QStringLiteral("scene_id")).toInt(-1);
            a.timestampMs = o.value(QStringLiteral("timestamp_ms")).toVariant().toLongLong();
            a.description = o.value(QStringLiteral("description")).toString();
            if (o.contains(QStringLiteral("bbox"))) {
                const auto bbox = o.value(QStringLiteral("bbox")).toObject();
                a.bboxX = bbox.value(QStringLiteral("x")).toDouble();
                a.bboxY = bbox.value(QStringLiteral("y")).toDouble();
                a.bboxW = bbox.value(QStringLiteral("w")).toDouble();
                a.bboxH = bbox.value(QStringLiteral("h")).toDouble();
            }
            e.appearances.append(a);
        }
        e.descriptionEmbedding = blobToEmbedding(
            row.value(QStringLiteral("description_embedding")).toByteArray());
        out.append(e);
    }
    return out;
}

// ================= 工具 =================

float VideoRAGStore::cosineSimilarity(con