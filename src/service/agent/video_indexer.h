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

#include "model/video_representation.h"
#include "model/retrieval_result.h"

class PlayerService;
class SceneDetector;
class ClipService;
class EmbeddingService;
class WhisperService;
class VideoRAGStore;
class DatabaseManager;

/**
 * 视频渐进式索引流水线（agent-core-design.md §5.2 / video-rag-plan.md 3.1）。
 *
 * 分级构建视频知识：
 *   Level 0 (即时, <1s): 元信息 + 场景边界（直方图差异，无需大模型）
 *   Level 1 (秒级, 后台): 关键帧 CLIP embedding + Whisper ASR + 文本 embedding
 *   Level 2 (按需, VLM):  场景语义描述 + 全视频摘要
 *
 * 输出：
 *   - 内存中的 VideoRepresentation（供 Agent 访问）
 *   - 写入 VideoRAGStore（供后续检索）
 *
 * 线程模型：
 *   - 主线程：接收 startIndex 请求、发出信号（progress / levelReady）
 *   - 工作线程池：所有耗时任务（场景分割、embedding、Whisper）
 *
 * 使用示例：
 *   auto* indexer = new VideoIndexer(...);
 *   indexer->startIndex("d:/videos/a.mp4");
 *   connect(indexer, &VideoIndexer::levelReady, [](int lvl, auto repr){ ... });
 */
class VideoIndexer : public QObject {
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

    /// 启动索引（异步）。已在跑相同 videoPath 时忽略。
    void startIndex(const QString& videoPath);

    /// 取消当前索引
    void cancel();

    /// 是否有正在运行的索引任务
    bool isRunning() const { return m_running.load(); }

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
    /// 各级构建函数（在工作线程执行）
    void buildLevel0(QSharedPointer<VideoRepresentation> repr,
                     const QString& videoPath, quint64 taskId);
    void buildLevel1(QSharedPointer<VideoRepresentation> repr,
                     const QString& videoPath, quint64 taskId);
    void buildLevel2Async(QSharedPointer<VideoRepresentation> repr);

    /// 从 PlayerService 按均匀采样抽帧（同步等待 captureFrameAt future）
    QVector<QImage> sampleFrames(const QString& videoPath,
                                  int64_t durationMs, int count,
                                  QVector<int64_t>* outTimestamps,
                                  quint64 taskId,
                                  const QString& videoId);

    /// 将关键帧持久化到 AppData/keyframes/<videoId>/，返回成功数量
    int persistKeyframes(QSharedPointer<VideoRepresentation> repr);

    bool isTaskCurrent(quint64 taskId, const QString& videoId) const;

    /// 将场景/语音段写入 RAG 存储
    void writeChunksToStore(const VideoRepresentation& repr);

    PlayerService*    m_player     = nullptr;
    SceneDetector*    m_sceneDet   = nullptr;
    VideoRAGStore*    m_ragStore   = nullptr;
    DatabaseManager*  m_db         = nullptr;
    ClipService*      m_clip       = nullptr;
    EmbeddingService* m_embedder   = nullptr;
    WhisperService*   m_whisper    = nullptr;

    QThreadPool m_pool;
    std::atomic_bool m_running{false};
    std::atomic_bool m_cancelRequested{false};
    std::atomic<quint64> m_taskGeneration{0};

    // 记录已加载 / 正在加载的视频
    mutable QMutex m_reprMutex;
    QHash<QString, QSharedPointer<VideoRepresentation>> m_repr; // videoPath → repr
    QString       m_currentPath;
    QString       m_currentVideoId;
};

#endif // FRAMEMIND_VIDEO_INDEXER_H
