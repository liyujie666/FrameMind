#include "service/agent/video_indexer.h"

#include "service/playerservice.h"
#include "service/agent/frame_extractor.h"
#include "service/scene_detector.h"
#include "service/rag/video_rag_store.h"
#include "service/rag/speech_segmenter.h"
#include "infrastructure/databasemanager.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/clip_service.h"
#  include "service/embedding_service.h"
#endif
#ifdef FRAMEMIND_HAS_WHISPER
#  include "service/whisper_service.h"
#  include "util/audio_decoder.h"
#endif

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QMutexLocker>
#include <QDebug>
#include <algorithm>

namespace {
// TransNetV2 模式：每秒 1 帧密集采样；回退模式（无模型）：每 10s 一帧
constexpr int kDenseFps           = 1;     // TransNetV2 需要的采样率
constexpr int kSparseSamplePerSec = 10;    // 回退模式每 N 秒一帧
constexpr int kMinSampleCount     = 20;
constexpr int kMaxDenseSample     = 600;   // 密集采样上限（10min 视频）
constexpr int kMaxSparseSample    = 200;
constexpr int kHashPrefixBytes    = 1024 * 1024;

int computeSampleCount(int64_t durationMs, bool dense)
{
    if (durationMs <= 0) return kMinSampleCount;
    if (dense) {
        const int count = static_cast<int>(durationMs / 1000 * kDenseFps);
        return std::clamp(count, kMinSampleCount, kMaxDenseSample);
    }
    const int count = static_cast<int>(durationMs / 1000 / kSparseSamplePerSec);
    return std::clamp(count, kMinSampleCount, kMaxSparseSample);
}

void assignRepresentativeFrames(QVector<Scene>& scenes,
                                const QVector<QImage>& frames,
                                const QVector<int64_t>& timestamps)
{
    constexpr int kMaxFramesPerScene = 3;
    for (Scene& scene : scenes) {
        QVector<int> candidates;
        for (int i = 0; i < frames.size() && i < timestamps.size(); ++i) {
            if (scene.contains(timestamps.at(i))) candidates.append(i);
        }
        if (candidates.isEmpty()) continue;

        const QVector<int> selected = {
            candidates.first(), candidates.at(candidates.size() / 2), candidates.last()
        };
        for (const int index : selected) {
            if (scene.representativeFrames.size() >= kMaxFramesPerScene) break;
            if (!scene.representativeFrames.isEmpty()
                && scene.representativeFrames.last().ptsMs == timestamps.at(index)) {
                continue;
            }
            SceneFrame frame;
            frame.requestedMs = timestamps.at(index);
            frame.ptsMs = timestamps.at(index);
            frame.image = frames.at(index);
            scene.representativeFrames.append(std::move(frame));
        }
    }
}
} // namespace

VideoIndexer::VideoIndexer(PlayerService* player,
                           SceneDetector* sceneDetector,
                           VideoRAGStore* ragStore,
                           DatabaseManager* db,
                           QObject* parent)
    : QObject(parent)
    , m_player(player)
    , m_sceneDet(sceneDetector)
    , m_ragStore(ragStore)
    , m_db(db)
{
    m_pool.setMaxThreadCount(2);
}

VideoIndexer::~VideoIndexer()
{
    cancel();
    m_pool.waitForDone(3000);
}

// ============================================================
// videoId 计算
// ============================================================

QString VideoIndexer::computeVideoId(const QString& videoPath)
{
    QFile file(videoPath);
    if (!file.open(QIODevice::ReadOnly)) {
        // 兜底：文件路径的 hash（不推荐但避免崩）
        return QString::fromLatin1(
            QCryptographicHash::hash(videoPath.toUtf8(),
                                     QCryptographicHash::Sha1).toHex());
    }

    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArray::number(QFileInfo(videoPath).size()));
    h.addData(file.read(kHashPrefixBytes));
    return QString::fromLatin1(h.result().toHex()).left(16);
}

// ============================================================
// 主入口
// ============================================================

void VideoIndexer::startIndex(const QString& videoPath)
{
    if (videoPath.isEmpty() || !QFileInfo::exists(videoPath)) {
        emit indexError(StageMetadata, tr("视频文件不存在: %1").arg(videoPath));
        return;
    }

    {
        QMutexLocker l(&m_reprMutex);
        if (m_running.load() && m_currentPath == videoPath) return;

        auto it = m_repr.find(videoPath);
        if (it != m_repr.end() && it.value()
                && it.value()->level >= VideoRepresentation::Level1) {
            auto cached = it.value();
            l.unlock();
            cancel();
            {
                QMutexLocker currentLock(&m_reprMutex);
                m_currentPath = videoPath;
                m_currentVideoId = cached->videoId;
            }
            emit progress(100, StageDone, tr("索引完成 (缓存命中)"));
            emit levelReady(0, cached);
            emit levelReady(1, cached);
            emit indexCompleted(cached);
            return;
        }
    }

    cancel();

    const QString videoId = computeVideoId(videoPath);
    const quint64 taskId = m_taskGeneration.fetch_add(1) + 1;
    m_cancelRequested.store(false);
    m_running.store(true);
    {
        QMutexLocker l(&m_reprMutex);
        m_currentPath = videoPath;
        m_currentVideoId = videoId;
    }

    VideoInfo initialInfo;
    if (m_player) {
        const VideoInfo activeInfo = m_player->videoInfo();
        const QString requested = QFileInfo(videoPath).canonicalFilePath();
        const QString active = QFileInfo(activeInfo.filePath).canonicalFilePath();
        if (!requested.isEmpty() && requested == active) initialInfo = activeInfo;
    }

    if (m_ragStore) {
        m_ragStore->invalidateVideo(videoId);
    }

    m_pool.start([this, videoPath, videoId, taskId, initialInfo]() {
        auto finishIfCurrent = [this, taskId, videoId]() {
            if (isTaskCurrent(taskId, videoId)) m_running.store(false);
        };

        auto repr = QSharedPointer<VideoRepresentation>::create();
        repr->videoId = videoId;
        repr->metadata = initialInfo;
        repr->metadata.filePath = videoPath;
        repr->metadata.fileName = QFileInfo(videoPath).fileName();

        if (!isTaskCurrent(taskId, videoId)) {
            finishIfCurrent();
            return;
        }

        buildLevel0(repr, videoPath, taskId);
        if (!isTaskCurrent(taskId, videoId)) {
            finishIfCurrent();
            return;
        }

        {
            QMutexLocker l(&m_reprMutex);
            if (m_cancelRequested.load()
                    || m_taskGeneration.load() != taskId
                    || m_currentVideoId != videoId) {
                return;
            }
            m_repr[videoPath] = repr;
        }
        emit levelReady(0, repr);

        buildLevel1(repr, videoPath, taskId);
        if (!isTaskCurrent(taskId, videoId)) {
            finishIfCurrent();
            return;
        }
        emit levelReady(1, repr);

        const int visualCount = m_ragStore
            ? m_ragStore->listChunks(VideoRAGStore::VisualFrames, videoId).size() : 0;
        const int textCount = m_ragStore
            ? m_ragStore->listChunks(VideoRAGStore::TextSegments, videoId).size() : 0;
        qDebug() << "[VideoIndexer] chunk 核对 | videoId:" << videoId
                 << "| visual:" << visualCount << "| text:" << textCount;

        emit progress(100, StageDone,
                      tr("索引完成 (Level 1，视觉 %1 / 文本 %2)")
                          .arg(visualCount).arg(textCount));
        emit indexCompleted(repr);
        finishIfCurrent();
    });
}

void VideoIndexer::cancel()
{
    m_cancelRequested.store(true);
    m_taskGeneration.fetch_add(1);
    m_running.store(false);
}

bool VideoIndexer::isTaskCurrent(quint64 taskId, const QString& videoId) const
{
    if (m_cancelRequested.load() || m_taskGeneration.load() != taskId) return false;
    QMutexLocker l(&m_reprMutex);
    return m_currentVideoId == videoId;
}

QSharedPointer<VideoRepresentation> VideoIndexer::representation(
    const QString& videoPath) const
{
    QMutexLocker l(&m_reprMutex);
    const QString path = videoPath.isEmpty() ? m_currentPath : videoPath;

    if (m_repr.contains(path)) return m_repr.value(path);
    if (!m_db || !m_ragStore || path.isEmpty()) return nullptr;

    const QString videoId = computeVideoId(path);
    m_ragStore->loadVideo(videoId);
    const auto chunks = m_ragStore->listChunks(VideoRAGStore::TextSegments, videoId);

    const QMap<int, VideoChunk> sceneChunks = [&chunks]() {
        QMap<int, VideoChunk> result;
        for (const VideoChunk& chunk : chunks) {
            if (chunk.chunkType != VideoChunk::SceneSummary) continue;
            const int sceneId = chunk.metadata.value(QStringLiteral("scene_id")).toInt();
            if (sceneId < 0 || result.contains(sceneId)) continue;
            result.insert(sceneId, chunk);
        }
        return result;
    }();

    auto repr = QSharedPointer<VideoRepresentation>::create();
    repr->videoId = videoId;
    repr->metadata.filePath = path;
    repr->metadata.fileName = QFileInfo(path).fileName();

    for (auto it = sceneChunks.constBegin(); it != sceneChunks.constEnd(); ++it) {
        const VideoChunk& chunk = it.value();
        Scene scene;
        scene.id = it.key();
        scene.startMs = chunk.startMs;
        scene.endMs = chunk.endMs;
        scene.keyframePath = chunk.keyframePath;
        scene.visualDescription = chunk.textContent;
        repr->scenes.append(scene);
    }
    std::sort(repr->scenes.begin(), repr->scenes.end(),
              [](const Scene& a, const Scene& b) {
                  if (a.startMs != b.startMs) return a.startMs < b.startMs;
                  return a.id < b.id;
              });

    for (const VideoChunk& chunk : chunks) {
        if (chunk.chunkType != VideoChunk::SpeechSegment) continue;
        SpeechSegment segment;
        segment.startMs = chunk.startMs;
        segment.endMs = chunk.endMs;
        segment.text = chunk.textContent;
        if (segment.isValid()) repr->speechSegments.append(segment);
    }
    std::sort(repr->speechSegments.begin(), repr->speechSegments.end(),
              [](const SpeechSegment& a, const SpeechSegment& b) {
                  return a.startMs < b.startMs;
              });

    repr->videoSummary = m_db->loadVideoSummary(videoId);
    repr->sceneDescriptions = m_db->loadSceneDescriptions(videoId);
    repr->sceneVisualDescriptions = m_db->loadSceneVisualDescriptions(videoId);
    for (Scene& scene : repr->scenes) {
        scene.description = repr->sceneDescriptions.value(scene.id);
        if (scene.description.isEmpty()) scene.description = scene.visualDescription;
        scene.fusedDescription = scene.description;
    }

    if (repr->scenes.isEmpty() && repr->speechSegments.isEmpty()) return nullptr;
    repr->metadata.durationMs = 0;
    for (const Scene& scene : repr->scenes) {
        repr->metadata.durationMs = qMax(repr->metadata.durationMs, scene.endMs);
    }
    repr->level = (repr->videoSummary.isEmpty() && repr->sceneDescriptions.isEmpty())
        ? VideoRepresentation::Level1 : VideoRepresentation::Level2;

    const_cast<QHash<QString, QSharedPointer<VideoRepresentation>>&>(m_repr).insert(path, repr);

    qDebug() << "[VideoIndexer] 从数据库加载视频表示"
             << "videoId=" << videoId
             << "场景数=" << repr->scenes.size()
             << "语音段数=" << repr->speechSegments.size()
             << "摘要长度=" << repr->videoSummary.size()
             << "场景描述数=" << repr->sceneDescriptions.size();
    return repr;
}

// ============================================================
// Level 0: 元信息 + 场景骨架
// ============================================================

void VideoIndexer::buildLevel0(QSharedPointer<VideoRepresentation> repr,
                               const QString& videoPath,
                               quint64 taskId)
{
    emit progress(0, StageMetadata, tr("解析视频元信息"));

    // metadata 在任务启动时按目标路径快照；播放器后续切换不会覆盖它。
    repr->metadata.filePath = videoPath;
    repr->metadata.fileName = QFileInfo(videoPath).fileName();

    emit progress(10, StageSceneSplit, tr("场景分割"));
    if (!isTaskCurrent(taskId, repr->videoId)) return;

    // 根据是否有 TransNetV2 决定采样密度
    const bool useTransNet = m_sceneDet && m_sceneDet->isUsingTransNet();
    const int sampleCount = computeSampleCount(repr->metadata.durationMs, useTransNet);
    qDebug() << "[VideoIndexer] 采样帧数:" << sampleCount
             << "| 时长(s):" << repr->metadata.durationMs / 1000
             << "| 模式:" << (useTransNet ? "TransNetV2 (1fps)" : "直方图 (稀疏)");
    QVector<int64_t> timestamps;
    QVector<QImage> frames = sampleFrames(
        videoPath, repr->metadata.durationMs, sampleCount, &timestamps,
        taskId, repr->videoId);

    if (m_sceneDet && !frames.isEmpty()) {
        repr->scenes = m_sceneDet->detectScenes(frames, timestamps);
    } else {
        Scene single;
        single.id = 0;
        single.startMs = 0;
        single.endMs   = repr->metadata.durationMs;
        single.keyframeMs = timestamps.isEmpty()
            ? repr->metadata.durationMs / 2 : timestamps.first();
        if (!frames.isEmpty()) single.keyframe = frames.first();
        repr->scenes.append(single);
    }
    assignRepresentativeFrames(repr->scenes, frames, timestamps);

    if (!isTaskCurrent(taskId, repr->videoId)) return;

    const int savedKeyframes = persistKeyframes(repr);
    qDebug() << "[VideoIndexer] 关键帧落盘 | videoId:" << repr->videoId
             << "| saved:" << savedKeyframes << "/" << repr->scenes.size();

    repr->level = VideoRepresentation::Level0;
    emit progress(30, StageSceneSplit,
                  tr("识别到 %1 个场景").arg(repr->scenes.size()));
}

// ============================================================
// Level 1: 关键帧 embedding + ASR + 文本 embedding
// ============================================================

void VideoIndexer::buildLevel1(QSharedPointer<VideoRepresentation> repr,
                               const QString& videoPath,
                               quint64 taskId)
{
    qDebug() << "[VideoIndexer] buildLevel1 开始 | scenes:"
             << repr->scenes.size();

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (!m_clip) {
        qDebug() << "[VideoIndexer] ClipService 未注入，跳过 CLIP embedding";
    } else if (!m_clip->isReady()) {
        qDebug() << "[VideoIndexer] ClipService 未就绪（模型未加载），跳过 CLIP embedding";
    } else if (repr->scenes.isEmpty()) {
        qDebug() << "[VideoIndexer] 无场景，跳过 CLIP embedding";
    } else {
        emit progress(40, StageKeyframeEncode, tr("编码关键帧 CLIP 向量"));

        std::vector<QImage> representativeImages;
        QVector<QPair<int, int>> frameReferences; // (sceneIndex, representativeFrameIndex)
        for (int sceneIndex = 0; sceneIndex < repr->scenes.size(); ++sceneIndex) {
            if (!isTaskCurrent(taskId, repr->videoId)) return;
            Scene& scene = repr->scenes[sceneIndex];
            if (scene.representativeFrames.isEmpty() && !scene.keyframe.isNull()) {
                SceneFrame fallback;
                fallback.requestedMs = scene.keyframeMs;
                fallback.ptsMs = scene.keyframeMs;
                fallback.imagePath = scene.keyframePath;
                fallback.image = scene.keyframe;
                scene.representativeFrames.append(std::move(fallback));
            }
            for (int frameIndex = 0; frameIndex < scene.representativeFrames.size(); ++frameIndex) {
                const SceneFrame& frame = scene.representativeFrames.at(frameIndex);
                if (frame.image.isNull()) continue;
                representativeImages.push_back(frame.image);
                frameReferences.append({sceneIndex, frameIndex});
            }
        }

        if (!representativeImages.empty()) {
            const auto embeddings = m_clip->encodeImages(representativeImages);
            const int embeddingCount = qMin(
                frameReferences.size(), static_cast<int>(embeddings.size()));
            for (int embeddingIndex = 0; embeddingIndex < embeddingCount; ++embeddingIndex) {
                if (!isTaskCurrent(taskId, repr->videoId)) return;
                const auto [sceneIndex, frameIndex] = frameReferences.at(embeddingIndex);
                const Scene& scene = repr->scenes.at(sceneIndex);
                const SceneFrame& frame = scene.representativeFrames.at(frameIndex);

                VideoChunk chunk;
                chunk.chunkId = makeChunkId(
                    repr->videoId, VideoChunk::FrameDesc,
                    frame.ptsMs, frame.ptsMs + 1,
                    QStringLiteral("%1:%2").arg(scene.id).arg(frameIndex));
                chunk.videoId = repr->videoId;
                chunk.startMs = frame.ptsMs;
                chunk.endMs = frame.ptsMs + 1;
                chunk.chunkType = VideoChunk::FrameDesc;
                chunk.textContent = tr("场景 %1 代表帧 %2").arg(scene.id).arg(frameIndex + 1);
                chunk.frameEmbedding = embeddings[embeddingIndex];
                chunk.keyframePath = frame.imagePath;
                chunk.metadata.insert(QStringLiteral("scene_id"), scene.id);
                chunk.metadata.insert(QStringLiteral("keyframe_ms"),
                                      static_cast<qlonglong>(frame.ptsMs));
                chunk.metadata.insert(QStringLiteral("frame_role"),
                                      frameIndex == 0 ? QStringLiteral("start")
                                      : frameIndex + 1 == scene.representativeFrames.size()
                                            ? QStringLiteral("end") : QStringLiteral("middle"));
                chunk.metadata.insert(QStringLiteral("image_width"), frame.image.width());
                chunk.metadata.insert(QStringLiteral("image_height"), frame.image.height());
                chunk.metadata.insert(QStringLiteral("embedding_model_id"), QStringLiteral("clip_visual"));
                chunk.metadata.insert(QStringLiteral("embedding_version"), QStringLiteral("1"));
                chunk.metadata.insert(QStringLiteral("embedding_dimension"),
                                      static_cast<int>(chunk.frameEmbedding.size()));
                chunk.metadata.insert(QStringLiteral("index_version"), 2);
                chunk.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);

                if (m_ragStore) {
                    m_ragStore->insertChunk(VideoRAGStore::VisualFrames, chunk);
                }
            }
        }
        qDebug() << "[VideoIndexer] CLIP embedding 完成 | representative frames:"
                 << representativeImages.size();
    }
#endif

#ifdef FRAMEMIND_HAS_WHISPER
    if (!m_whisper) {
        qDebug() << "[VideoIndexer] WhisperService 未注入，跳过语音转写";
    } else if (!m_whisper->isReady()) {
        qDebug() << "[VideoIndexer] WhisperService 未就绪（模型未加载），跳过语音转写";
    } else {
        emit progress(70, StageTranscribe, tr("语音转写"));

        AudioDecoder decoder;
        decoder.setProgressCallback([this](int percent) {
            emit progress(70 + percent / 5, StageTranscribe,
                          tr("音频解码 %1%").arg(percent));
        });

        auto pcmData = decoder.decodeToFloat32(videoPath);
        if (!pcmData.empty() && isTaskCurrent(taskId, repr->videoId)) {
            emit progress(85, StageTranscribe, tr("Whisper 转写中..."));
            repr->speechSegments = m_whisper->transcribe(pcmData);

            if (m_ragStore) {
                for (int segIndex = 0; segIndex < repr->speechSegments.size(); ++segIndex) {
                    if (!isTaskCurrent(taskId, repr->videoId)) return;
                    const auto& seg = repr->speechSegments[segIndex];
                    VideoChunk c;
                    c.chunkId = makeChunkId(
                        repr->videoId, VideoChunk::SpeechSegment,
                        seg.startMs, seg.endMs, QString::number(segIndex));
                    c.videoId = repr->videoId;
                    c.startMs = seg.startMs;
                    c.endMs   = seg.endMs;
                    c.chunkType = VideoChunk::SpeechSegment;
                    c.textContent = seg.text;
                    c.metadata.insert(QStringLiteral("evidence_type"),
                                      QStringLiteral("speech_segment"));
                    c.metadata.insert(QStringLiteral("source"),
                                      QStringLiteral("whisper"));
                    c.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);
                    c.metadata.insert(QStringLiteral("embedding_model_id"),
                                      QStringLiteral("bge_text"));
                    c.metadata.insert(QStringLiteral("embedding_version"), QStringLiteral("passage_v2"));
                    c.metadata.insert(QStringLiteral("index_version"), 2);

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                    if (m_embedder && m_embedder->isReady() && !seg.text.isEmpty()) {
                        c.textEmbedding = m_embedder->embedPassage(seg.text);
                        c.metadata.insert(QStringLiteral("embedding_dimension"),
                                          static_cast<int>(c.textEmbedding.size()));
                    }
#endif
                    m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
                }

                // 原始 Whisper 段持续服务字幕、精确引用与音画对齐；额外生成 12-30 秒
                // 语义窗口仅用于主题检索，避免模型原始短段使连续台词召回碎片化。
                const auto semanticSegments = SpeechSegmenter::buildSemanticSegments(
                    repr->speechSegments);
                for (int semanticIndex = 0; semanticIndex < semanticSegments.size(); ++semanticIndex) {
                    if (!isTaskCurrent(taskId, repr->videoId)) return;
                    const SpeechSegment& segment = semanticSegments.at(semanticIndex);
                    VideoChunk chunk;
                    chunk.chunkId = makeChunkId(
                        repr->videoId, VideoChunk::Event, segment.startMs, segment.endMs,
                        QStringLiteral("semantic_speech:%1").arg(semanticIndex));
                    chunk.videoId = repr->videoId;
                    chunk.startMs = segment.startMs;
                    chunk.endMs = segment.endMs;
                    chunk.chunkType = VideoChunk::Event;
                    chunk.textContent = segment.text;
                    chunk.metadata.insert(QStringLiteral("evidence_type"),
                                          QStringLiteral("speech_semantic"));
                    chunk.metadata.insert(QStringLiteral("source"),
                                          QStringLiteral("whisper_semantic"));
                    chunk.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);
                    chunk.metadata.insert(QStringLiteral("embedding_model_id"),
                                          QStringLiteral("bge_text"));
                    chunk.metadata.insert(QStringLiteral("embedding_version"), QStringLiteral("passage_v2"));
                    chunk.metadata.insert(QStringLiteral("index_version"), 2);
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                    if (m_embedder && m_embedder->isReady()) {
                        chunk.textEmbedding = m_embedder->embedPassage(segment.text);
                        chunk.metadata.insert(QStringLiteral("embedding_dimension"),
                                              static_cast<int>(chunk.textEmbedding.size()));
                    }
#endif
                    m_ragStore->insertChunk(VideoRAGStore::TextSegments, chunk);
                }
            }

            qDebug() << "[VideoIndexer] 转写完成, 共"
                     << repr->speechSegments.size() << "原始段";
        } else if (pcmData.empty()) {
            qDebug() << "[VideoIndexer] 音频解码结果为空，跳过转写";
        }
    }
#endif

    if (!isTaskCurrent(taskId, repr->videoId)) return;
    for (Scene& scene : repr->scenes) {
        scene.keyframe = QImage();
        for (SceneFrame& frame : scene.representativeFrames) {
            frame.image = QImage();
        }
    }
    repr->level = VideoRepresentation::Level1;
    qDebug() << "[VideoIndexer] buildLevel1 完成";
    emit progress(90, StageKeyframeEncode, tr("索引 Level 1 完成"));
}

void VideoIndexer::buildLevel2Async(QSharedPointer<VideoRepresentation> /*repr*/)
{
    // Level 2 由 VideoAnalysisService 负责（需要 VLM 大模型）
    // 索引器只暴露占位接口，实际调用见 VideoAnalysisService
}

// ============================================================
// 帧采样
// ============================================================

QVector<QImage> VideoIndexer::sampleFrames(const QString& videoPath,
                                            int64_t durationMs, int count,
                                            QVector<int64_t>* outTimestamps,
                                            quint64 taskId,
                                            const QString& videoId)
{
    QVector<QImage> frames;
    if (videoPath.isEmpty() || durationMs <= 0 || count <= 0) return frames;

    QVector<int64_t> targets;
    targets.reserve(count);
    for (int i = 0; i < count; ++i) {
        targets.append(static_cast<int64_t>(
            (static_cast<double>(i) + 0.5) / count * durationMs));
    }

    QString decodeError;
    const QVector<FrameExtractor::Frame> extracted = FrameExtractor::extract(
        videoPath, targets, FrameExtractor::Options{}, &m_cancelRequested, &decodeError);
    frames.reserve(extracted.size());
    for (const FrameExtractor::Frame& frame : extracted) {
        if (!isTaskCurrent(taskId, videoId)) break;
        if (frame.image.isNull()) continue;
        frames.append(frame.image);
        if (outTimestamps) outTimestamps->append(frame.ptsMs);
    }
    if (!frames.isEmpty() || !m_player || !isTaskCurrent(taskId, videoId)) return frames;

    // 旧播放器截帧仅作为 SDK/编解码器不兼容时的兼容回退，不参与正常离线索引路径。
    qWarning() << "[VideoIndexer] FrameExtractor 失败，启用播放器兼容回退:" << decodeError;
    frames.reserve(count);
    for (const int64_t ts : targets) {
        if (!isTaskCurrent(taskId, videoId)) break;
        auto future = m_player->captureFrameAt(videoPath, ts, 2000);
        future.waitForFinished();
        if (future.resultCount() <= 0 || !isTaskCurrent(taskId, videoId)) continue;
        const QImage image = future.result();
        if (image.isNull()) continue;
        frames.append(image);
        if (outTimestamps) outTimestamps->append(ts);
    }
    return frames;
}

int VideoIndexer::persistKeyframes(QSharedPointer<VideoRepresentation> repr)
{
    if (!repr || repr->videoId.isEmpty()) return 0;

    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appData.isEmpty()) {
        appData = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                  + QStringLiteral("/FrameMind");
    }
    const QString dirPath = appData + QStringLiteral("/keyframes/") + repr->videoId;
    QDir keyframeDir(dirPath);
    if (keyframeDir.exists() && !keyframeDir.removeRecursively()) {
        qWarning() << "[VideoIndexer] 无法清理旧关键帧目录:" << dirPath;
        return 0;
    }
    if (!QDir().mkpath(dirPath)) {
        qWarning() << "[VideoIndexer] 无法创建关键帧目录:" << dirPath;
        return 0;
    }

    int saved = 0;
    for (Scene& scene : repr->scenes) {
        if (m_cancelRequested.load()) break;

        if (!scene.keyframe.isNull()) {
            const QString fileName = QStringLiteral("scene_%1_%2.jpg")
                                         .arg(scene.id)
                                         .arg(scene.keyframeMs);
            const QString path = QDir(dirPath).filePath(fileName);
            if (scene.keyframe.save(path, "JPG", 85)) {
                scene.keyframePath = path;
                ++saved;
            } else {
                qWarning() << "[VideoIndexer] 关键帧保存失败:" << path;
            }
        }

        for (int frameIndex = 0; frameIndex < scene.representativeFrames.size(); ++frameIndex) {
            SceneFrame& frame = scene.representativeFrames[frameIndex];
            if (frame.image.isNull()) continue;
            const QString fileName = QStringLiteral("scene_%1_rep_%2_%3.jpg")
                                         .arg(scene.id)
                                         .arg(frameIndex)
                                         .arg(frame.ptsMs);
            const QString path = QDir(dirPath).filePath(fileName);
            if (frame.image.save(path, "JPG", 85)) {
                frame.imagePath = path;
                ++saved;
            } else {
                qWarning() << "[VideoIndexer] 代表帧保存失败:" << path;
            }
        }
    }
    return saved;
}

QString VideoIndexer::makeChunkId(const QString& videoId,
                                            VideoChunk::ChunkType chunkType,
                                            int64_t startMs,
                                            int64_t endMs,
                                            const QString& discriminator)
{
    const QByteArray identity = QStringLiteral("%1|%2|%3|%4|%5|v1")
                                    .arg(videoId)
                                    .arg(static_cast<int>(chunkType))
                                    .arg(startMs)
                                    .arg(endMs)
                                    .arg(discriminator)
                                    .toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex());
}

void VideoIndexer::writeChunksToStore(const VideoRepresentation& repr)
{
    if (!m_ragStore) return;
    // 场景视觉描述作为 text_segments 写入（Level 2 生成后调用）。
    // 新流水线由 VideoAnalysisService 分别写入 visual/audio/fused；这里保留兼容路径，
    // 但只写纯视觉证据，避免把融合描述误标成 SceneSummary。
    for (auto it = repr.sceneDescriptions.constBegin();
         it != repr.sceneDescriptions.constEnd(); ++it) {
        const int sceneId = it.key();
        const QString desc = repr.sceneVisualDescriptions.value(sceneId, it.value());
        const Scene& s = repr.scenes.value(sceneId);
        if (!s.isValid() || desc.isEmpty()) continue;

        VideoChunk c;
        c.chunkId = makeChunkId(
            repr.videoId, VideoChunk::SceneSummary,
            s.startMs, s.endMs, QString::number(sceneId));
        c.videoId = repr.videoId;
        c.startMs = s.startMs;
        c.endMs   = s.endMs;
        c.chunkType = VideoChunk::SceneSummary;
        c.textContent = desc;
        c.metadata.insert(QStringLiteral("scene_id"), sceneId);
        c.metadata.insert(QStringLiteral("evidence_type"), QStringLiteral("visual"));
        c.metadata.insert(QStringLiteral("file_path"), repr.metadata.filePath);
        m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
    }

    // 语音段
    for (int segIndex = 0; segIndex < repr.speechSegments.size(); ++segIndex) {
        const auto& seg = repr.speechSegments[segIndex];
        VideoChunk c;
        c.chunkId = makeChunkId(
            repr.videoId, VideoChunk::SpeechSegment,
            seg.startMs, seg.endMs, QString::number(segIndex));
        c.videoId = repr.videoId;
        c.startMs = seg.startMs;
        c.endMs   = seg.endMs;
        c.chunkType = VideoChunk::SpeechSegment;
        c.textContent = seg.text;
        c.metadata.insert(QStringLiteral("evidence_type"),
                          QStringLiteral("speech_segment"));
        c.metadata.insert(QStringLiteral("source"), QStringLiteral("whisper"));
        c.metadata.insert(QStringLiteral("file_path"), repr.metadata.filePath);
        m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
    }

    // 兼容旧写入路径：原始段仍保留，语义窗口只作为额外召回单元。
    const auto semanticSegments = SpeechSegmenter::buildSemanticSegments(repr.speechSegments);
    for (int index = 0; index < semanticSegments.size(); ++index) {
        const SpeechSegment& segment = semanticSegments.at(index);
        VideoChunk chunk;
        chunk.chunkId = makeChunkId(repr.videoId, VideoChunk::Event,
                                    segment.startMs, segment.endMs,
                                    QStringLiteral("semantic_speech:%1").arg(index));
        chunk.videoId = repr.videoId;
        chunk.startMs = segment.startMs;
        chunk.endMs = segment.endMs;
        chunk.chunkType = VideoChunk::Event;
        chunk.textContent = segment.text;
        chunk.metadata.insert(QStringLiteral("evidence_type"),
                              QStringLiteral("speech_semantic"));
        chunk.metadata.insert(QStringLiteral("source"), QStringLiteral("whisper_semantic"));
        chunk.metadata.insert(QStringLiteral("file_path"), repr.metadata.filePath);
        m_ragStore->insertChunk(VideoRAGStore::TextSegments, chunk);
    }
}
