#ifndef FRAMEMIND_VIDEO_INDEXER_H
#define FRAMEMIND_VIDEO_INDEXER_H

#include <QObject>
#include <QString>
#include <QHash>
#include <QThreadPool>
#include <QMutex>
#include <QSharedPointer>
#include <QVector>
#include <atomic>
#include <memory>
#include <functional>

#include "model/video_representation.h"
#include "model/retrieval_result.h"
#include "service/agent/video_rag_build_backend.h"

class PlayerService;
class SceneDetector;
class ClipService;
class EmbeddingService;
class WhisperService;
class VideoRAGStore;
class DatabaseManager;

/** Plan-driven evidence execution backend. Workers return values; the coordinator
 * commits snapshots and publishes representations on the owning thread. */
class VideoIndexer : public QObject, public VideoRAGBuildBackend {
    Q_OBJECT
public:
    /// 每个 stage 的进度信号 stage 值
    enum Stage {
        StageIdle,
        StageMetadata,
        StageSceneSplit,
        StageKeyframeEncode,
        StageTranscribe,
        StageSceneDesc,
        StageSummarize,
        StageDone
    };
    Q_ENUM(Stage)

    explicit VideoIndexer(PlayerService* player,
                          SceneDetector* sceneDetector,
                          VideoRAGStore* ragStore,
                          DatabaseManager* db,
                          QObject* parent = nullptr);
    ~VideoIndexer() override;

    /// 可选注入（未启用 ONNX/Whisper 时传 nullptr）
    void setClipService(ClipService* clip)         { m_clip = clip; }
    void setEmbeddingService(EmbeddingService* e)  { m_embedder = e; }
    void setWhisperService(WhisperService* w)      { m_whisper = w; }

    /// 获取当前视频的表示（可能未完成）
    /// 若 videoPath 为空，返回最近一次
    QSharedPointer<VideoRepresentation> representation(const QString& videoPath = {}) const;

    /// 计算确定性 chunk ID，供各索引阶段幂等写入。
    static QString makeChunkId(const QString& videoId,
                               VideoChunk::ChunkType chunkType,
                               int64_t startMs,
                               int64_t endMs,
                               const QString& discriminator = {});

    /// 根据文件路径计算稳定 videoId（size + 头 1MB hash）
    static QString computeVideoId(const QString& videoPath);
    using ExtractionResult=VideoEvidenceExtractionResult;
    AvailableCapabilities capabilities() const;
    QJsonObject modelVersions() const;
    void extract(const VideoBuildContext&, const VideoRAGBuildPlan&, bool probe,
                 std::function<void(ExtractionResult)>);
    void encodeChunks(const VideoBuildContext&, QVector<VideoChunk>,
                      std::function<void(QVector<VideoChunk>)>);
    void setPublished(const VideoRepresentation&);
    void extractUnitFrames(const VideoBuildContext&,const QVector<SemanticUnit>&,const VideoRAGBuildPlan&,
                           std::function<void(ExtractionResult)>) override;

signals:
    /// 进度上报
    void progress(int percent, Stage stage, const QString& message);

    /// 某个级别刚构建完成，可以开始使用
    void levelReady(int level, QSharedPointer<VideoRepresentation> repr);

    /// 整体流水线结束
    void indexCompleted(QSharedPointer<VideoRepresentation> repr);

    /// 出错（记录 stage 与消息）
    void indexError(Stage stage, const QString& error);

private:
    PlayerService*    m_player     = nullptr;
    SceneDetector*    m_sceneDet   = nullptr;
    VideoRAGStore*    m_ragStore   = nullptr;
    DatabaseManager*  m_db         = nullptr;
    ClipService*      m_clip       = nullptr;
    EmbeddingService* m_embedder   = nullptr;
    WhisperService*   m_whisper    = nullptr;

    QThreadPool m_pool;
    QVector<std::weak_ptr<std::atomic_bool>> m_cancellations;

    // 记录已加载 / 正在加载的视频
    mutable QMutex m_reprMutex;
    QHash<QString, QSharedPointer<VideoRepresentation>> m_repr; // videoPath → repr
    QString       m_currentPath;
    QString       m_currentVideoId;
};

#endif // FRAMEMIND_VIDEO_INDEXER_H
