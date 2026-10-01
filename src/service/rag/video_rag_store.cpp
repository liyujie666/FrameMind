#include "service/rag/video_rag_store.h"

#include "infrastructure/databasemanager.h"

#include <QThread>
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
    case VideoChunk::UnitSummary: return QStringLiteral("unit_summary");
    case VideoChunk::UnitFact: return QStringLiteral("unit_fact");
    case VideoChunk::TextEvidence: return QStringLiteral("text_evidence");
    case VideoChunk::ChapterSummary: return QStringLiteral("chapter_summary");
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
    if (s == QLatin1String("unit_summary")) return VideoChunk::UnitSummary;
    if (s == QLatin1String("unit_fact")) return VideoChunk::UnitFact;
    if (s == QLatin1String("text_evidence")) return VideoChunk::TextEvidence;
    if (s == QLatin1String("chapter_summary")) return VideoChunk::ChapterSummary;
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

namespace {
bool visible(const VideoChunk& c, const VideoBuildManifest& active,
             const QString& explicitBuild = {}, const QString& explicitRaw = {}) {
    const QString build = c.metadata.value(QStringLiteral("build_id")).toString();
    const QString raw = c.metadata.value(QStringLiteral("raw_snapshot_id")).toString();
    const QString targetBuild = explicitBuild.isEmpty() ? active.buildId : explicitBuild;
    const QString targetRaw = explicitRaw.isEmpty() ? active.rawSnapshotId : explicitRaw;
    if (targetBuild.isEmpty()) return build.isEmpty() && raw.isEmpty(); // explicit legacy view
    if (!build.isEmpty()) return build == targetBuild;
    return !targetRaw.isEmpty() && raw == targetRaw;
}
QString json(const QJsonObject& j) {return QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact));}
bool writeChunk(DatabaseManager* db, VideoRAGStore::Collection col, const VideoChunk& c) {
    if (!c.isValid()) return false;
    if (!db) return true;
    return db->exec(QStringLiteral("INSERT OR REPLACE INTO rag_chunks (chunk_id,collection,video_id,start_ms,end_ms,chunk_type,text_content,text_embedding,frame_embedding,keyframe_path,metadata_json,build_id,raw_snapshot_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"),
        {c.chunkId,colName(col),c.videoId,qlonglong(c.startMs),qlonglong(c.endMs),chunkTypeToString(c.chunkType),c.textContent,
         embeddingToBlob(c.textEmbedding),embeddingToBlob(c.frameEmbedding),c.keyframePath,json(QJsonObject::fromVariantMap(c.metadata)),
         c.metadata.value(QStringLiteral("build_id"),QString()),c.metadata.value(QStringLiteral("raw_snapshot_id"),QString())});
}
}

// ================= Impl =================

struct VideoRAGStore::Impl {
    DatabaseManager* db = nullptr;

    // 内存索引：collection → chunkId → VideoChunk
    // 单机场景直接线性扫描，规模够用；后续替换 FAISS 时可平滑迁移
    QHash<Collection, QHash<QString, VideoChunk>> inMemory;

    // 记录当前已加载的 videoId 集合，避免重复 load
    QSet<QString> loadedVideos;
    QHash<QString, VideoBuildManifest> active;
    QHash<QString, QVector<SemanticUnit>> units;

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

    if (!d->db->exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    const QStringList migrations = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS video_raw_snapshots (snapshot_id TEXT PRIMARY KEY, video_id TEXT NOT NULL, payload_json TEXT NOT NULL, created_at DATETIME DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS video_rag_builds (build_id TEXT PRIMARY KEY, video_id TEXT NOT NULL, raw_snapshot_id TEXT NOT NULL, status TEXT NOT NULL, payload_json TEXT NOT NULL, updated_at DATETIME DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS video_semantic_units (build_id TEXT NOT NULL, unit_id TEXT NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, payload_json TEXT NOT NULL, PRIMARY KEY(build_id,unit_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS video_unit_links (build_id TEXT NOT NULL, from_unit_id TEXT NOT NULL, relation TEXT NOT NULL, target_kind TEXT NOT NULL, target_id TEXT NOT NULL, PRIMARY KEY(build_id,from_unit_id,relation,target_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_build_video ON video_rag_builds(video_id,status)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_units_time ON video_semantic_units(build_id,start_ms,end_ms)")
    };
    bool ok = true;
    for (const auto& sql : migrations) ok = d->db->exec(sql) && ok;
    const auto addColumn = [this](const QString& table, const QString& column) {
        for (const auto& row : d->db->query(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
            if (row.value(QStringLiteral("name")).toString() == column) return true;
        return d->db->exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 TEXT NOT NULL DEFAULT ''").arg(table,column));
    };
    ok = addColumn(QStringLiteral("video_metadata"),QStringLiteral("active_build_id")) && ok;
    ok = addColumn(QStringLiteral("rag_chunks"),QStringLiteral("build_id")) && ok;
    ok = addColumn(QStringLiteral("rag_chunks"),QStringLiteral("raw_snapshot_id")) && ok;
    ok = d->db->exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_chunks_scope ON rag_chunks(video_id,raw_snapshot_id,build_id)")) && ok;
    if (ok) ok=d->db->exec(QStringLiteral("INSERT OR REPLACE INTO settings(key,value) VALUES('rag_schema_version','3')"));
    if (!ok || !d->db->exec(QStringLiteral("COMMIT"))) {d->db->exec(QStringLiteral("ROLLBACK"));return false;}
    return true;
}

// ================= 加载 / 失效 =================

void VideoRAGStore::loadVideo(const QString& videoId)
{
    if (QThread::currentThread()!=thread()) {QMetaObject::invokeMethod(this,[this,videoId]{loadVideo(videoId);},Qt::BlockingQueuedConnection);return;}
    QMutexLocker lock(&d->mtx);
    if (d->loadedVideos.contains(videoId)) return;
    if (!d->db) return;

    const auto builds = d->db->query(QStringLiteral("SELECT b.payload_json FROM video_metadata m JOIN video_rag_builds b ON b.build_id=m.active_build_id WHERE m.video_id=?"),{videoId});
    if (!builds.isEmpty()) {
        const auto manifest=VideoBuildManifest::fromJson(QJsonDocument::fromJson(builds.first().value("payload_json").toString().toUtf8()).object());
        d->active[videoId]=manifest;
        QVector<SemanticUnit> units;
        for(const auto& row:d->db->query(QStringLiteral("SELECT payload_json FROM video_semantic_units WHERE build_id=? ORDER BY start_ms,unit_id"),{manifest.buildId}))
            units<<SemanticUnit::fromJson(QJsonDocument::fromJson(row.value("payload_json").toString().toUtf8()).object());
        d->units[manifest.buildId]=units;
    }

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
    lock.unlock();
    emit videoIndexLoaded(videoId, count);
}

bool VideoRAGStore::hasIndexedContent(const QString& videoId) const
{
    QMutexLocker lock(&d->mtx);
    for (const Collection collection : {VisualFrames, TextSegments}) {
        const auto chunks = d->inMemory.constFind(collection);
        if (chunks == d->inMemory.constEnd()) continue;
        for (auto it = chunks->constBegin(); it != chunks->constEnd(); ++it) {
            if (it.value().videoId == videoId && visible(it.value(),d->active.value(videoId))) return true;
        }
    }
    return false;
}

void VideoRAGStore::invalidateVideo(const QString& videoId)
{
    if(QThread::currentThread()!=thread()){QMetaObject::invokeMethod(this,[this,videoId]{invalidateVideo(videoId);},Qt::BlockingQueuedConnection);return;}
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
        d->active.remove(videoId);
    }

    if (d->db) {
        d->db->exec(QStringLiteral("DELETE FROM rag_chunks WHERE video_id = ?"), { videoId });
        d->db->exec(QStringLiteral("DELETE FROM rag_entities WHERE video_id = ?"), { videoId });
        d->db->exec(QStringLiteral("DELETE FROM video_unit_links WHERE build_id IN (SELECT build_id FROM video_rag_builds WHERE video_id=?)"),{videoId});
        d->db->exec(QStringLiteral("DELETE FROM video_semantic_units WHERE build_id IN (SELECT build_id FROM video_rag_builds WHERE video_id=?)"),{videoId});
        d->db->exec(QStringLiteral("DELETE FROM video_rag_builds WHERE video_id=?"),{videoId});
        d->db->exec(QStringLiteral("DELETE FROM video_raw_snapshots WHERE video_id=?"),{videoId});
        d->db->exec(QStringLiteral("DELETE FROM scene_descriptions WHERE video_id=?"),{videoId});
        d->db->exec(QStringLiteral("DELETE FROM video_metadata WHERE video_id=?"),{videoId});
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
    if (!writeChunk(d->db,col,chunk)) return false;
    {
        QMutexLocker lock(&d->mtx);
        d->inMemory[col][chunk.chunkId] = chunk;
        d->loadedVideos.insert(chunk.videoId);
    }

    return true;
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
        if (!visible(c,d->active.value(c.videoId),filter.buildId,filter.rawSnapshotId)) continue;
        if(!filter.unitId.isEmpty() && c.metadata.value("unit_id").toString()!=filter.unitId) continue;

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
        if (!visible(chunk,d->active.value(chunk.videoId),filter.buildId,filter.rawSnapshotId)) continue;
        if(!filter.unitId.isEmpty() && chunk.metadata.value("unit_id").toString()!=filter.unitId) continue;
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
    const auto c=it.value().value(chunkId, {});
    return visible(c,d->active.value(c.videoId)) ? c : VideoChunk{};
}

QVector<VideoChunk> VideoRAGStore::listChunks(Collection col, const QString& videoId) const
{
    QVector<VideoChunk> out;
    QMutexLocker lock(&d->mtx);
    const auto it = d->inMemory.constFind(col);
    if (it == d->inMemory.constEnd()) return out;
    for (const auto& c : it.value()) {
        if (c.videoId == videoId && visible(c,d->active.value(videoId))) out.append(c);
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

VideoBuildManifest VideoRAGStore::activeBuild(const QString& videoId) const
{
    QMutexLocker lock(&d->mtx);
    return d->active.value(videoId);
}

bool VideoRAGStore::saveCandidateBuild(const VideoBuildManifest& m)
{
    Q_ASSERT(QThread::currentThread()==thread());
    if (!d->db || m.buildId.isEmpty() || m.videoId.isEmpty()) return false;
    return d->db->exec(QStringLiteral("INSERT OR REPLACE INTO video_rag_builds(build_id,video_id,raw_snapshot_id,status,payload_json) VALUES(?,?,?,?,?)"),
                       {m.buildId,m.videoId,m.rawSnapshotId,artifactStateKey(m.state),json(m.toJson())});
}

bool VideoRAGStore::saveRawSnapshot(const QString& id, const QString& video,
                                   const QJsonObject& payload, const QVector<VideoChunk>& chunks)
{
    Q_ASSERT(QThread::currentThread()==thread());
    if (!d->db || id.isEmpty() || !d->db->exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    bool ok=d->db->exec(QStringLiteral("INSERT OR IGNORE INTO video_raw_snapshots(snapshot_id,video_id,payload_json) VALUES(?,?,?)"),{id,video,json(payload)});
    for(const auto& c:chunks) {
        if(c.videoId!=video || c.metadata.value("raw_snapshot_id").toString()!=id || !c.metadata.value("build_id").toString().isEmpty()) {ok=false;break;}
        ok=writeChunk(d->db,c.chunkType==VideoChunk::FrameDesc?VisualFrames:TextSegments,c)&&ok;
    }
    if (!ok || !d->db->exec(QStringLiteral("COMMIT"))) {d->db->exec(QStringLiteral("ROLLBACK"));return false;}
    QMutexLocker lock(&d->mtx);
    for(const auto& c:chunks) d->inMemory[c.chunkType==VideoChunk::FrameDesc?VisualFrames:TextSegments][c.chunkId]=c;
    d->loadedVideos.insert(video);
    return true;
}

QJsonObject VideoRAGStore::loadRawSnapshot(const QString& id) const
{
    Q_ASSERT(QThread::currentThread()==thread());
    if (!d->db) return {};
    auto rows=d->db->query(QStringLiteral("SELECT payload_json FROM video_raw_snapshots WHERE snapshot_id=?"),{id});
    return rows.isEmpty()?QJsonObject{}:QJsonDocument::fromJson(rows.first().value("payload_json").toString().toUtf8()).object();
}

QVector<VideoChunk> VideoRAGStore::rawChunks(const QString& id) const
{
    QMutexLocker lock(&d->mtx);QVector<VideoChunk> out;
    for(const auto& collection:d->inMemory) for(const auto& c:collection)
        if(c.metadata.value("raw_snapshot_id").toString()==id && c.metadata.value("build_id").toString().isEmpty()) out<<c;
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.startMs==b.startMs?a.chunkId<b.chunkId:a.startMs<b.startMs;});
    return out;
}

bool VideoRAGStore::saveUnitBatch(const VideoBuildManifest& m,const QVector<SemanticUnit>& units,const QVector<VideoChunk>& chunks)
{
    Q_ASSERT(QThread::currentThread()==thread());
    if(!d->db || !d->db->exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    bool ok=saveCandidateBuild(m);QSet<QString> ids;
    for(const auto& u:units) ids.insert(u.unitId);
    for(const auto& u:units) {
        if(!u.isValid() || u.buildId!=m.buildId || (!u.nextUnitId.isEmpty() && !ids.contains(u.nextUnitId)) || (!u.previousUnitId.isEmpty() && !ids.contains(u.previousUnitId))) {ok=false;break;}
        ok=d->db->exec(QStringLiteral("INSERT OR REPLACE INTO video_semantic_units VALUES(?,?,?,?,?)"),{m.buildId,u.unitId,qlonglong(u.startMs),qlonglong(u.endMs),json(u.toJson())})&&ok;
        ok=d->db->exec(QStringLiteral("DELETE FROM video_unit_links WHERE build_id=? AND from_unit_id=?"),{m.buildId,u.unitId})&&ok;
        auto link=[&](const QString& relation,const QString& kind,const QString& target){
            if(!target.isEmpty()) ok=d->db->exec(QStringLiteral("INSERT OR REPLACE INTO video_unit_links VALUES(?,?,?,?,?)"),{m.buildId,u.unitId,relation,kind,target})&&ok;
        };
        for(const auto& source:u.sourceChunkIds) link("source","chunk",source);
        link("parent","unit",u.parentUnitId);link("previous","unit",u.previousUnitId);link("next","unit",u.nextUnitId);
    }
    for(const auto& c:chunks) {
        if(c.videoId!=m.videoId || c.metadata.value("build_id").toString()!=m.buildId || c.metadata.value("raw_snapshot_id").toString()!=m.rawSnapshotId) {ok=false;break;}
        ok=writeChunk(d->db,TextSegments,c)&&ok;
    }
    if(!ok || !d->db->exec(QStringLiteral("COMMIT"))) {d->db->exec(QStringLiteral("ROLLBACK"));return false;}
    QMutexLocker lock(&d->mtx);d->units[m.buildId]=units;
    for(const auto& c:chunks) d->inMemory[TextSegments][c.chunkId]=c;
    return true;
}

bool VideoRAGStore::publishBuild(const VideoBuildManifest& m,const QString& expected)
{
    Q_ASSERT(QThread::currentThread()==thread());
    if(!d->db || (m.state!=ArtifactState::Ready && m.state!=ArtifactState::Partial) || listUnits(m.buildId).isEmpty()) return false;
    if(!d->db->exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    const auto rows=d->db->query(QStringLiteral("SELECT active_build_id FROM video_metadata WHERE video_id=?"),{m.videoId});
    const QString active=rows.isEmpty()?QString():rows.first().value("active_build_id").toString();
    bool ok=active==expected;
    if(ok && active==m.buildId) {const auto old=activeBuild(m.videoId);ok=m.revision>old.revision;}
    ok=ok && !loadRawSnapshot(m.rawSnapshotId).isEmpty();
    if(ok) ok=saveCandidateBuild(m) && d->db->saveVideoMetadata(m.videoId,m.filePath,m.summary,2)
        && d->db->exec(QStringLiteral("UPDATE video_metadata SET active_build_id=? WHERE video_id=?"),{m.buildId,m.videoId});
    if(!ok || !d->db->exec(QStringLiteral("COMMIT"))) {d->db->exec(QStringLiteral("ROLLBACK"));return false;}
    QMutexLocker lock(&d->mtx);d->active[m.videoId]=m;
    return true;
}

QVector<SemanticUnit> VideoRAGStore::listUnits(const QString& buildId) const
{
    QMutexLocker lock(&d->mtx);return d->units.value(buildId);
}
SemanticUnit VideoRAGStore::getUnit(const QString& build,const QString& id) const
{
    for(const auto& u:listUnits(build)) if(u.unitId==id) return u;
    return {};
}

float VideoRAGStore::cosineSimilarity(const std::vector<float>& a,
                                       const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f, normA = 0.0f, normB = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot   += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    const float denom = std::sqrt(normA) * std::sqrt(normB);
    return denom > 0.0f ? dot / denom : 0.0f;
}
