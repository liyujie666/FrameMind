#include "service/agent/video_indexer.h"

#include "service/playerservice.h"
#include "service/scene_detector.h"
#include "service/rag/video_rag_store.h"

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
} // namespace

VideoIndexer::VideoIndexer(PlayerService* player,
                           SceneDetector* sceneDetector,
                           VideoRAGStore* ragStore,
                           QObject* parent)
    : QObject(parent)
    , m_player(player)
    , m_sceneDet(sceneDetector)
    , m_ragStore(ragStore)
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
    
    // 检查数据库中是否已有该视频的 RAG 索引
    if (m_ragStore && m_ragStore->hasVideoIndex(videoId)) {
        qDebug() << "[VideoIndexer] 知识库中已存在该视频索引 | videoId:" << videoId
                 << "| 跳过重建，直接加载";
        
        // 加载已有的知识库到内存
        if (!m_ragStore->isVideoLoaded(videoId)) {
            m_ragStore->loadVideo(videoId);
        }
        
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

        m_pool.start([this, videoPath, videoId, taskId, initialInfo]() {
            auto repr = QSharedPointer<VideoRepresentation>::create();
            repr->videoId = videoId;
            repr->metadata = initialInfo;
            repr->metadata.filePath = videoPath;
            repr->metadata.fileName = QFileInfo(videoPath).fileName();

            emit progress(20, StageSceneSplit, tr("加载已有知识库"));

            // 从数据库 chunk 恢复场景信息和描述
            if (m_ragStore) {
                // 1. 从 visual_frames 恢复场景基本信息
                const auto visualChunks = m_ragStore->listChunks(VideoRAGStore::VisualFrames, videoId);
                QMap<int, Scene> sceneMap;
                for (const auto& chunk : visualChunks) {
                    if (chunk.chunkType == VideoChunk::FrameDesc) {
                        const int sceneId = chunk.metadata.value(QStringLiteral("scene_id"), -1).toInt();
                        if (sceneId < 0) continue;
                        
                        Scene& scene = sceneMap[sceneId];
                        scene.id = sceneId;
                        scene.startMs = chunk.startMs;
                        scene.endMs = chunk.endMs;
                        scene.keyframeMs = chunk.metadata.value(QStringLiteral("keyframe_ms"), chunk.startMs).toLongLong();
                        scene.keyframePath = chunk.keyframePath;
                    }
                }
                
                // 2. 从 text_segments 恢复场景描述（三类证据）和语音段
                const auto textChunks = m_ragStore->listChunks(VideoRAGStore::TextSegments, videoId);
                for (const auto& chunk : textChunks) {
                    // 恢复语音转写段（字幕）
                    if (chunk.chunkType == VideoChunk::SpeechSegment) {
                        SpeechSegment seg;
                        seg.startMs = chunk.startMs;
                        seg.endMs = chunk.endMs;
                        seg.text = chunk.textContent;
                        repr->speechSegments.append(seg);
                        continue;
                    }
                    
                    const int sceneId = chunk.metadata.value(QStringLiteral("scene_id"), -1).toInt();
                    if (sceneId < 0) continue;
                    
                    const QString evidenceType = chunk.metadata.value(
                        QStringLiteral("evidence_type")).toString();
                    
                    if (chunk.chunkType == VideoChunk::SceneSummary && 
                        evidenceType == QLatin1String("visual")) {
                        // 纯视觉描述
                        repr->sceneVisualDescriptions[sceneId] = chunk.textContent;
                        if (sceneMap.contains(sceneId)) {
                            sceneMap[sceneId].visualDescription = chunk.textContent;
                        }
                    } else if (chunk.chunkType == VideoChunk::SceneAudio) {
                        // 音频摘要
                        if (sceneMap.contains(sceneId)) {
                            sceneMap[sceneId].audioSummary = chunk.textContent;
                        }
                    } else if (chunk.chunkType == VideoChunk::SceneFused) {
                        // 融合描述
                        repr->sceneDescriptions[sceneId] = chunk.textContent;
                        if (sceneMap.contains(sceneId)) {
                            sceneMap[sceneId].fusedDescription = chunk.textContent;
                            sceneMap[sceneId].description = chunk.textContent;
                            
                            // 恢复音视频关系元数据
                            const QString relationStr = chunk.metadata.value(
                                QStringLiteral("audio_relation")).toString();
                            if (relationStr == QLatin1String("strong")) {
                                sceneMap[sceneId].audioRelation = AudioVisualRelation::Strong;
                            } else if (relationStr == QLatin1String("contextual")) {
                                sceneMap[sceneId].audioRelation = AudioVisualRelation::Contextual;
                            } else if (relationStr == QLatin1String("independent")) {
                                sceneMap[sceneId].audioRelation = AudioVisualRelation::Independent;
                            }
                            
                            sceneMap[sceneId].audioRelationConfidence = 
                                chunk.metadata.value(QStringLiteral("audio_confidence"), 0.0).toFloat();
                            
                            const QString audioTypeStr = chunk.metadata.value(
                                QStringLiteral("audio_type")).toString();
                            if (audioTypeStr == QLatin1String("dialogue")) {
                                sceneMap[sceneId].audioType = SceneAudioType::Dialogue;
                            } else if (audioTypeStr == QLatin1String("narration")) {
                                sceneMap[sceneId].audioType = SceneAudioType::Narration;
                            } else if (audioTypeStr == QLatin1String("background_media")) {
                                sceneMap[sceneId].audioType = SceneAudioType::BackgroundMedia;
                            } else if (audioTypeStr == QLatin1String("ambient")) {
                                sceneMap[sceneId].audioType = SceneAudioType::Ambient;
                            }
                        }
                    }
                }
                
                // 对于只有视觉描述的场景，将视觉描述作为最终描述
                for (auto it = sceneMap.begin(); it != sceneMap.end(); ++it) {
                    if (it->description.isEmpty() && !it->visualDescription.isEmpty()) {
                        it->description = it->visualDescription;
                        repr->sceneDescriptions[it->id] = it->visualDescription;
                    }
                }
                
                for (const Scene& s : sceneMap.values()) {
                    repr->scenes.append(s);
                }
                std::sort(repr->scenes.begin(), repr->scenes.end(),
                          [](const Scene& a, const Scene& b) { return a.id < b.id; });
                
                // 对语音段按时间排序
                std::sort(repr->speechSegments.begin(), repr->speechSegments.end(),
                          [](const SpeechSegment& a, const SpeechSegment& b) {
                    return a.startMs < b.startMs;
                });
                
                qDebug() << "[VideoIndexer] 恢复场景描述 | videoId:" << videoId
                         << "| 视觉描述:" << repr->sceneVisualDescriptions.size()
                         << "| 融合描述:" << repr->sceneDescriptions.size()
                         << "| 语音段:" << repr->speechSegments.size();
            }

            if (!isTaskCurrent(taskId, videoId)) {
                if (isTaskCurrent(taskId, videoId)) m_running.store(false);
                return;
            }

            // 如果场景描述已完整，标记为 Level2，否则为 Level1
            const bool hasDescriptions = !repr->sceneDescriptions.isEmpty() || 
                                        !repr->sceneVisualDescriptions.isEmpty();
            repr->level = VideoRepresentation::Level1;
            
            {
                QMutexLocker l(&m_reprMutex);
                if (m_cancelRequested.load() || m_taskGeneration.load() != taskId 
                        || m_currentVideoId != videoId) {
                    return;
                }
                m_repr[videoPath] = repr;
            }

            emit progress(60, StageSceneSplit, tr("场景信息已恢复"));
            emit levelReady(0, repr);
            emit levelReady(1, repr);
            
            // 如果有完整的场景描述，标记为 Level2 并发出信号
            if (hasDescriptions) {
                repr->level = VideoRepresentation::Level2;
            }

            const int visualCount = m_ragStore
                ? m_ragStore->listChunks(VideoRAGStore::VisualFrames, videoId).size() : 0;
            const int textCount = m_ragStore
                ? m_ragStore->listChunks(VideoRAGStore::TextSegments, videoId).size() : 0;
            
            qDebug() << "[VideoIndexer] 知识库加载完成 | videoId:" << videoId
                     << "| scenes:" << repr->scenes.size()
                     << "| visual:" << visualCount << "| text:" << textCount;

            emit progress(100, StageDone,
                          tr("知识库加载完成 (视觉 %1 / 文本 %2)")
                              .arg(visualCount).arg(textCount));
            emit indexCompleted(repr);
            if (isTaskCurrent(taskId, videoId)) m_running.store(false);
        });
        
        return;
    }

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
    if (videoPath.isEmpty()) {
        return m_repr.value(m_currentPath);
    }
    return m_repr.value(videoPath);
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

        std::vector<QImage> keyframes;
        QVector<int> keyframeSceneIndexes;
        keyframes.reserve(repr->scenes.size());
        keyframeSceneIndexes.reserve(repr->scenes.size());
        for (int sceneIndex = 0; sceneIndex < repr->scenes.size(); ++sceneIndex) {
            if (!isTaskCurrent(taskId, repr->videoId)) return;
            const Scene& scene = repr->scenes[sceneIndex];
            if (!scene.keyframe.isNull()) {
                keyframes.push_back(scene.keyframe);
                keyframeSceneIndexes.append(sceneIndex);
            }
        }

        if (!keyframes.empty()) {
            const auto embeddings = m_clip->encodeImages(keyframes);
            // 写入 RAG store: visual_frames
            const int embeddingCount = qMin(
                keyframeSceneIndexes.size(), static_cast<int>(embeddings.size()));
            for (int embeddingIndex = 0; embeddingIndex < embeddingCount; ++embeddingIndex) {
                if (!isTaskCurrent(taskId, repr->videoId)) return;
                const Scene& s = repr->scenes[keyframeSceneIndexes[embeddingIndex]];

                VideoChunk c;
                c.chunkId = makeChunkId(
                    repr->videoId, VideoChunk::FrameDesc,
                    s.startMs, s.endMs, QString::number(s.id));
                c.videoId = repr->videoId;
                c.startMs = s.startMs;
                c.endMs   = s.endMs;
                c.chunkType = VideoChunk::FrameDesc;
                c.textContent = tr("场景 %1 关键帧").arg(s.id);
                c.frameEmbedding = embeddings[embeddingIndex];
                c.keyframePath = s.keyframePath;
                c.metadata.insert(QStringLiteral("scene_id"), s.id);
                c.metadata.insert(QStringLiteral("keyframe_ms"),
                                  static_cast<qlonglong>(s.keyframeMs));
                c.metadata.insert(QStringLiteral("image_width"), s.keyframe.width());
                c.metadata.insert(QStringLiteral("image_height"), s.keyframe.height());
                c.metadata.insert(QStringLiteral("index_version"), 1);
                c.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);

                if (m_ragStore) {
                    m_ragStore->insertChunk(VideoRAGStore::VisualFrames, c);
                }
            }
        }
        qDebug() << "[VideoIndexer] CLIP embedding 完成 | keyframes:"
                 << keyframes.size();
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

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                    if (m_embedder && m_embedder->isReady() && !seg.text.isEmpty()) {
                        c.textEmbedding = m_embedder->embed(seg.text);
                    }
#endif
                    m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
                }
            }

            qDebug() << "[VideoIndexer] 转写完成, 共"
                     << repr->speechSegments.size() << "段";
        } else if (pcmData.empty()) {
            qDebug() << "[VideoIndexer] 音频解码结果为空，跳过转写";
        }
    }
#endif

    if (!isTaskCurrent(taskId, repr->videoId)) return;
    for (Scene& scene : repr->scenes) {
        scene.keyframe = QImage();
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
    if (!m_player || videoPath.isEmpty() || durationMs <= 0 || count <= 0) return frames;

    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (!isTaskCurrent(taskId, videoId)) break;
        const int64_t ts = static_cast<int64_t>(
            (static_cast<double>(i) + 0.5) / count * durationMs);
        auto future = m_player->captureFrameAt(videoPath, ts, 2000);
        future.waitForFinished();
        if (!isTaskCurrent(taskId, videoId)) break;
        if (future.resultCount() > 0) {
            const QImage img = future.result();
            if (!img.isNull()) {
                frames.append(img);
                if (outTimestamps) outTimestamps->append(ts);
            }
        }
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
        if (m_cancelRequested.load() || scene.keyframe.isNull()) break;
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
}
