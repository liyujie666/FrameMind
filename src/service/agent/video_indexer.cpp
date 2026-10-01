#include "service/agent/video_indexer.h"
#include "infrastructure/databasemanager.h"
#include "model/video_representation_codec.h"
#include "service/agent/media_probe.h"
#include "service/rag/video_rag_store.h"
#include "util/video_file_identity.h"
#include <QDebug>
#include <QFileInfo>
#include <QMutexLocker>
#include <algorithm>

VideoIndexer::VideoIndexer(PlayerService *player, SceneDetector *sceneDetector, VideoRAGStore *ragStore,
                           DatabaseManager *db, QObject *parent)
    : QObject(parent), m_player(player), m_sceneDet(sceneDetector), m_ragStore(ragStore), m_db(db) {
    m_pool.setMaxThreadCount(1);
}
VideoIndexer::~VideoIndexer() {
    for (const auto &weak : m_cancellations)
        if (auto flag = weak.lock())
            flag->store(true);
    m_pool.clear();
    m_pool.waitForDone();
}
QString VideoIndexer::computeVideoId(const QString &path) { return VideoFileIdentity::legacyId(path); }

QSharedPointer<VideoRepresentation> VideoIndexer::representation(const QString &videoPath) const {
    QMutexLocker l(&m_reprMutex);
    const QString path = videoPath.isEmpty() ? m_currentPath : videoPath;

    if (m_repr.contains(path)) {
        const auto value = m_repr.value(path);
        if (value->videoId != computeVideoId(path) ||
            (!value->build.fileFingerprint.isEmpty() &&
             value->build.fileFingerprint != VideoFileIdentity::fingerprint(path)))
            return nullptr;
        return value;
    }
    if (!m_db || !m_ragStore || path.isEmpty())
        return nullptr;

    const QString videoId = computeVideoId(path);
    m_ragStore->loadVideo(videoId);
    const auto active = m_ragStore->activeBuild(videoId);
    if (!active.buildId.isEmpty()) {
        auto restored = representationFromJson(m_ragStore->loadRawSnapshot(active.rawSnapshotId));
        if (!restored.isValid() || active.fileFingerprint != VideoFileIdentity::fingerprint(path))
            return nullptr;
        restored.metadata.filePath = path;
        restored.build = active;
        restored.semanticUnits = m_ragStore->listUnits(active.buildId);
        restored.videoSummary = active.summary;
        restored.level = VideoRepresentation::Level2;
        auto value = QSharedPointer<VideoRepresentation>::create(restored);
        const_cast<VideoIndexer *>(this)->m_repr.insert(path, value);
        return value;
    }
    const auto chunks = m_ragStore->listChunks(VideoRAGStore::TextSegments, videoId);

    const QMap<int, VideoChunk> sceneChunks = [&chunks]() {
        QMap<int, VideoChunk> result;
        for (const VideoChunk &chunk : chunks) {
            if (chunk.chunkType != VideoChunk::SceneSummary)
                continue;
            const int sceneId = chunk.metadata.value(QStringLiteral("scene_id")).toInt();
            if (sceneId < 0 || result.contains(sceneId))
                continue;
            result.insert(sceneId, chunk);
        }
        return result;
    }();

    auto repr = QSharedPointer<VideoRepresentation>::create();
    repr->videoId = videoId;
    repr->metadata.filePath = path;
    repr->metadata.fileName = QFileInfo(path).fileName();

    for (auto it = sceneChunks.constBegin(); it != sceneChunks.constEnd(); ++it) {
        const VideoChunk &chunk = it.value();
        Scene scene;
        scene.id = it.key();
        scene.startMs = chunk.startMs;
        scene.endMs = chunk.endMs;
        scene.keyframePath = chunk.keyframePath;
        scene.visualDescription = chunk.textContent;
        repr->scenes.append(scene);
    }
    std::sort(repr->scenes.begin(), repr->scenes.end(), [](const Scene &a, const Scene &b) {
        if (a.startMs != b.startMs)
            return a.startMs < b.startMs;
        return a.id < b.id;
    });

    for (const VideoChunk &chunk : chunks) {
        if (chunk.chunkType != VideoChunk::SpeechSegment)
            continue;
        SpeechSegment segment;
        segment.startMs = chunk.startMs;
        segment.endMs = chunk.endMs;
        segment.text = chunk.textContent;
        if (segment.isValid())
            repr->speechSegments.append(segment);
    }
    std::sort(repr->speechSegments.begin(), repr->speechSegments.end(),
              [](const SpeechSegment &a, const SpeechSegment &b) { return a.startMs < b.startMs; });

    repr->videoSummary = m_db->loadVideoSummary(videoId);
    repr->sceneDescriptions = m_db->loadSceneDescriptions(videoId);
    repr->sceneVisualDescriptions = m_db->loadSceneVisualDescriptions(videoId);
    for (Scene &scene : repr->scenes) {
        scene.description = repr->sceneDescriptions.value(scene.id);
        if (scene.description.isEmpty())
            scene.description = scene.visualDescription;
        scene.fusedDescription = scene.description;
    }

    if (repr->scenes.isEmpty() && repr->speechSegments.isEmpty())
        return nullptr;
    repr->build.profile.source = QStringLiteral("legacy_scene_v2");
    repr->build.state = ArtifactState::Partial;
    repr->metadata = MediaProbe::inspect(path);
    for (const Scene &scene : repr->scenes) {
        repr->metadata.durationMs = qMax(repr->metadata.durationMs, scene.endMs);
    }
    repr->level = (repr->videoSummary.isEmpty() && repr->sceneDescriptions.isEmpty())
                      ? VideoRepresentation::Level1
                      : VideoRepresentation::Level2;

    const_cast<QHash<QString, QSharedPointer<VideoRepresentation>> &>(m_repr).insert(path, repr);

    qDebug() << "[VideoIndexer] 从数据库加载视频表示"
             << "videoId=" << videoId << "场景数=" << repr->scenes.size()
             << "语音段数=" << repr->speechSegments.size() << "摘要长度=" << repr->videoSummary.size()
             << "场景描述数=" << repr->sceneDescriptions.size();
    return repr;
}

QString VideoIndexer::makeChunkId(const QString &videoId, VideoChunk::ChunkType chunkType, int64_t startMs,
                                  int64_t endMs, const QString &discriminator) {
    const QByteArray identity = QStringLiteral("%1|%2|%3|%4|%5|v1")
                                    .arg(videoId)
                                    .arg(static_cast<int>(chunkType))
                                    .arg(startMs)
                                    .arg(endMs)
                                    .arg(discriminator)
                                    .toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex());
}
