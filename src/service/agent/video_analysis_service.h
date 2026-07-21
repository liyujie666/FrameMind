#ifndef FRAMEMIND_VIDEO_ANALYSIS_SERVICE_H
#define FRAMEMIND_VIDEO_ANALYSIS_SERVICE_H

#include <QObject>
#include <QString>
#include <QSharedPointer>
#include <QImage>
#include <QQueue>
#include <QTimer>

#include "model/video_representation.h"
#include "model/agent_types.h"
#include "model/videocontext.h"

class AgentService;
class VideoIndexer;
class VideoRAGStore;
class PlayerService;
class EmbeddingService;

/**
 * 视频分析主服务（架构 §3.3.2 / agent-core-design.md §3.2 REPRESENT）。
 *
 * 职责：
 *   1. 统筹 VideoIndexer 完成 Level 0/1
 *   2. 借用 AgentService 调用 VLM 生成 Level 2（场景描述、全视频摘要）
 *   3. 分析单帧 / 时间区间的内容（供 Tool 层复用）
 *   4. 将分析结果写入 VideoRAGStore（形成新的可检索 chunks）
 *
 * 优化策略：
 *   - 全量场景描述：所有场景都会被描述，而非仅前N个
 *   - 上下文关联：描述每个场景时注入前后场景上下文，确保叙事连贯
 *   - 渐进式并行：通过队列控制并发数，防止 VLM 请求过载
 *   - 二阶段摘要：全部场景描述完成后自动生成连贯的全视频摘要
 */
class VideoAnalysisService : public QObject {
    Q_OBJECT
public:
    explicit VideoAnalysisService(AgentService*    agent,
                                  VideoIndexer*    indexer,
                                  VideoRAGStore*   ragStore,
                                  PlayerService*   player,
                                  QObject*         parent = nullptr);

    void setEmbeddingService(EmbeddingService* e) { m_embedder = e; }

    /// 设置并发描述数。默认1（串行），确保上下文连贯。
    /// 若追求速度可设为 2~3，但场景描述的关联性会略有下降。
    void setConcurrency(int n) { m_maxConcurrent = qBound(1, n, 5); }

    // ---- 统筹入口 ----

    /// 打开新视频时调用：启动索引 + 计划生成 Level 2
    void onVideoOpened(const QString& videoPath);

    /// 手动触发全量分析（含 Level 2）
    void analyzeVideo(const QString& videoPath);

    /// 获取指定视频的表示（VideoIndexer 中的引用）
    QSharedPointer<VideoRepresentation> representation(const QString& videoPath = {}) const;

    // ---- Level 2: VLM 描述 & 摘要 ----

    /**
     * 为单个场景生成结构化描述。
     * 会自动注入前后场景上下文以维持叙事连贯性。
     * 结果写入 repr->sceneDescriptions[sceneId] 并同步到 RAG store。
     */
    void describeScene(int sceneId,
                       QSharedPointer<VideoRepresentation> repr);

    /**
     * 批量描述所有未描述的场景（全量覆盖策略）。
     * 内部通过队列控制并发，每完成一个场景 emit sceneDescribed。
     * 全部完成后自动触发 summarizeVideo。
     */
    void describeAllScenes(QSharedPointer<VideoRepresentation> repr);

    /**
     * 汇总所有场景描述生成全视频摘要。
     * 结果写入 repr->videoSummary。
     */
    void summarizeVideo(QSharedPointer<VideoRepresentation> repr);

    // ---- Level 3: 局部深度分析（供 Tool 层调用）----

    /**
     * 单帧描述（对应 Tool: seek_and_analyze）
     * @param frame  已截取的关键帧
     * @param focus  分析关注点，如"关注画面中的人物动作"
     * @param onDone 完成回调（在主线程触发）
     */
    void describeFrame(const QImage& frame,
                       int64_t timestampMs,
                       const QString& focus,
                       std::function<void(const QString& description)> onDone);

    /**
     * 时间区间多帧联合分析（对应 Tool: analyze_time_range）
     */
    void analyzeTimeRange(int64_t startMs, int64_t endMs,
                          const QString& focus,
                          int sampleCount,
                          std::function<void(const QString& description)> onDone);

    // ---- 构建 System Prompt 用的 VideoContext ----

    /// 从当前 repr 组装可注入 system prompt 的视频上下文
    VideoContext buildVideoContext(QSharedPointer<VideoRepresentation> repr) const;

signals:
    void analysisProgress(int percent, const QString& stage);
    void sceneDescribed(int sceneId, const QString& description);
    void allScenesDescribed();
    void summaryReady(const QString& summary);
    void analysisError(const QString& message);

private:
    /// 带上下文的场景描述核心实现
    void doDescribeSceneWithContext(int sceneId, QSharedPointer<VideoRepresentation> repr);

    /// 构建场景描述时的上下文信息（前后场景摘要 + 全局位置）
    QString buildSceneContext(int sceneId, QSharedPointer<VideoRepresentation> repr) const;

    /// 从场景描述队列中推进下一批任务
    void drainDescribeQueue();

    /// 场景描述完成回调（检查队列 & 检查是否全部完成）
    void onSceneDescribeFinished(int sceneId, QSharedPointer<VideoRepresentation> repr);

    /**
     * 启动预热阶段：描述前 N 个场景，全部完成后自动触发 summarizeVideo。
     * @param repr      视频表示
     * @param prewarm   预热场景数量（通常 3，视频场景少时取实际场景数）
     */
    void startPrewarmAndSummarize(QSharedPointer<VideoRepresentation> repr, int prewarm);

    /// 借助 AgentService 走一次一次性（非流式）VLM 调用
    void oneShotVLM(const QString& sysPrompt,
                    const QString& userText,
                    const QList<QImage>& frames,
                    std::function<void(const QString&)> onDone);

    AgentService*    m_agent    = nullptr;
    VideoIndexer*    m_indexer  = nullptr;
    VideoRAGStore*   m_ragStore = nullptr;
    PlayerService*   m_player   = nullptr;
    EmbeddingService* m_embedder = nullptr;

    // ---- 批量描述队列管理 ----
    // 采用顺序串行模式：确保场景 N 描述完成后才开始场景 N+1，
    // 这样后续场景能获得前面场景的完整描述作为上下文，保证叙事连贯性。
    struct DescribeTask {
        int sceneId;
        QSharedPointer<VideoRepresentation> repr;
    };
    QQueue<DescribeTask>  m_describeQueue;
    int  m_activeTasks     = 0;
    int  m_maxConcurrent   = 1;      // 默认串行（保证上下文完整性）
    int  m_totalToDescribe = 0;      // 本轮待描述总数
    int  m_describedCount  = 0;      // 已完成数
    bool m_batchMode       = false;  // 是否正在批量描述中
};

#endif // FRAMEMIND_VIDEO_ANALYSIS_SERVICE_H
