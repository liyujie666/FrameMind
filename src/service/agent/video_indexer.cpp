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
#endif

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QtConcurrent>
#include <QMutexLocker>
#include <QDebug>

namespace {
constexpr int kInitialSampleCount = 20;   // Level 0 场景分割用的采样帧数上限
constexpr int kHashPrefixBytes    = 1024 * 1024;  // 头 1MB
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
    if (m_running && m_currentPath == videoPath) return;
    cancel();

    m_running = true;
    m_cancelRequested = false;
    m_currentPath = videoPath;

    // 立即在工作线程运行 Level 0 + Level 1
    QtConcurrent::run(&m_pool, [this, videoPath]() {
        // Level 0
        auto repr = QSharedPointer<VideoRepresentation>::create();
        repr->videoId = computeVideoId(videoPath);
        repr->metadata.filePath = videoPath;
        repr->metadata.fileName = QFileInfo(videoPath).fileName();

        buildLevel0(repr, videoPath);
        if (m_cancelRequested) { m_running = false; return; }

        {
            QMutexLocker l(&m_reprMutex);
            m_repr[videoPath] = repr;
        }
        emit levelReady(0, repr);

        // Level 1
        buildLevel1(repr, videoPath);
        if (m_cancelRequested) { m_running = false; return; }
        emit levelReady(1, repr);

        // Level 2 交由 VideoAnalysisService 负责（需要 VLM）
        // 索引器本身只到 Level 1 结束
        emit progress(100, StageDone, tr("索引完成 (Level 1)"));
        emit indexCompleted(repr);
        m_running = false;
    });
}

void VideoIndexer::cancel()
{
    m_cancelRequested = true;
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
                               const QString& videoPath)
{
    emit progress(0, StageMetadata, tr("解析视频元信息"));

    // metadata 通常在 PlayerService open 之后可用；此处允许调用方在 open 后触发。
    // 对齐架构：从 player 拉当前 videoInfo（可能字段不全，容忍）
    if (m_player) {
        const VideoInfo info = m_player->videoInfo();
        if (info.durationMs > 0) repr->metadata = info;
        repr->metadata.filePath = videoPath;   // 保证路径存在
        repr->metadata.fileName = QFileInfo(videoPath).fileName();
    }

    emit progress(10, StageSceneSplit, tr("场景分割"));
    if (m_cancelRequested) return;

    // 均匀采样 → 场景分割
    QVector<int64_t> timestamps;
    QVector<QImage> frames = sampleFrames(
        videoPath, repr->metadata.durationMs, kInitialSampleCount, &timestamps);

    if (m_sceneDet && !frames.isEmpty()) {
        repr->scenes = m_sceneDet->detectScenes(frames, timestamps);
    } else {
        // 兜底：整个视频作为一个场景
        Scene single;
        single.id = 0;
        single.startMs = 0;
        single.endMs   = repr->metadata.durationMs;
        single.keyframeMs = repr->metadata.durationMs / 2;
        if (!frames.isEmpty()) single.keyframe = frames.first();
        repr->scenes.append(single);
    }

    repr->level = VideoRepresentation::Level0;
    emit progress(30, StageSceneSplit,
                  tr("识别到 %1 个场景").arg(repr->scenes.size()));
}

// ============================================================
// Level 1: 关键帧 embedding + ASR + 文本 embedding
// ============================================================

void VideoIndexer::buildLevel1(QSharedPointer<VideoRepresentation> repr,
                               const QString& videoPath)
{
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_clip && m_clip->isReady() && !repr->scenes.isEmpty()) {
        emit progress(40, StageKeyframeEncode, tr("编码关键帧 CLIP 向量"));

        std::vector<QImage> keyframes;
        keyframes.reserve(repr->scenes.size());
        for (const Scene& s : repr->scenes) {
            if (!s.keyframe.isNull()) keyframes.push_back(s.keyframe);
        }

        if (!keyframes.empty()) {
            const auto embeddings = m_clip->encodeImages(keyframes);
            // 写入 RAG store: visual_frames
            for (int i = 0; i < repr->scenes.size() && i < static_cast<int>(embeddings.size()); ++i) {
                if (m_cancelRequested) return;
                const Scene& s = repr->scenes[i];

                VideoChunk c;
                c.chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                c.videoId = repr->videoId;
                c.startMs = s.startMs;
                c.endMs   = s.endMs;
                c.chunkType = VideoChunk::FrameDesc;
                c.textContent = tr("场景 %1 关键帧").arg(s.id);
                c.frameEmbedding = embeddings[i];
                c.keyframePath = s.keyframePath;
                c.metadata.insert(QStringLiteral("scene_id"), s.id);

                if (m_ragStore) {
                    m_ragStore->insertChunk(VideoRAGStore::VisualFrames, c);
                }
            }
        }
    }
#endif

#ifdef FRAMEMIND_HAS_WHISPER
    if (m_whisper && m_whisper->isReady()) {
        emit progress(70, StageTranscribe, tr("语音转写"));
        // 注意：真正的 PCM 数据抽取需从 PlayerService 或独立解码器提取
        // 此处保留 TODO，等 M4-T4 完整接入
        // repr->speechSegments = m_whisper->transcribe(pcmData);
    }
#endif

    repr->level = VideoRepresentation::Level1;
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
                                            QVector<int64_t>* outTimestamps)
{
    (void)videoPath;
    QVector<QImage> frames;
    if (!m_player || durationMs <= 0 || count <= 0) return frames;

    frames.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (m_cancelRequested) break;
        const int64_t ts = static_cast<int64_t>(
            (static_cast<double>(i) + 0.5) / count * durationMs);
        auto future = m_player->captureFrameAt(ts, 2000);
        future.waitForFinished();
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

void VideoIndexer::writeChunksToStore(const VideoRepresentation& repr)
{
    if (!m_ragStore) return;
    // 场景描述作为 text_segments 写入（Level 2 生成后调用）
    for (auto it = repr.sceneDescriptions.constBegin();
         it != repr.sceneDescriptions.constEnd(); ++it) {
        const int sceneId = it.key();
        const QString desc = it.value();
        const Scene& s = repr.scenes.value(sceneId);
        if (!s.isValid() || desc.isEmpty()) continue;

        VideoChunk c;
        c.chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        c.videoId = repr.videoId;
        c.startMs = s.startMs;
        c.endMs   = s.endMs;
        c.chunkType = VideoChunk::SceneSummary;
        c.textContent = desc;
        c.metadata.insert(QStringLiteral("scene_id"), sceneId);
        m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
    }

    // 语音段
    for (const auto& seg : repr.speechSegments) {
        VideoChunk c;
        c.chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        c.videoId = repr.videoId;
        c.startMs = seg.startMs;
        c.endMs   = seg.endMs;
        c.chunkType = VideoChunk::SpeechSegment;
        c.textContent = seg.text;
        m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
    }
}
