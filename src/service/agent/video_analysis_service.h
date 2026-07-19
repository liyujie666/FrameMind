#ifndef FRAMEMIND_VIDEO_ANALYSIS_SERVICE_H
#define FRAMEMIND_VIDEO_ANALYSIS_SERVICE_H

#include <QObject>
#include <QString>
#include <QSharedPointer>
#include <QImage>

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
 * 是 Agent 与"感知/表示"能力之间的桥梁。
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

    // ---- 统筹入口 ----

    /// 打开新视频时调用：启动索引 + 计划生成 Level 2
    void onVideoOpened(const QString& videoPath);

    /// 手动触发全量分析（含 Level 2）
    void analyzeVideo(const QString& videoPath);

    /// 获取指定视频的表示（VideoIndexer 中的引用）
    QSharedPointer<VideoRepresentation> representation(const QString& videoPath = {}) const;

    // ---- Level 2: VLM 描述 & 摘要 ----

    /**
     * 为单个场景生成结构化描述（agent-core-design.md §3.2 SCENE_DESCRIPTION_PROMPT）。
     * 结果写入 repr->sceneDescriptions[sceneId] 并同步到 RAG store。
     */
    void describeScene(int sceneId,
                       QSharedPointer<VideoRepresentation> repr);

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
    void summaryReady(const QString& summary);
    void analysisError(const QString& message);

private:
    /// SCENE_DESCRIPTION_PROMPT 组装 & 调用 VLM
    void doDescribeScene(int sceneId, QSharedPointer<VideoRepresentation> repr);

    /**
     * 启动预热阶段：描述前 N 个场景，全部完成后自动触发 summarizeVideo。
     * @param repr      视频表示
     * @param prewarm   预热场景数量（通常 3，视频场景少时取实际场景数）
     */
    void startPrewarmAndSummarize(QSharedPointer<VideoRepresentation> repr, int prewarm);

    /// 借助 AgentService 走一次一次性（非流式）VLM 调用
    /// 通过临时 conversation + 简单 aggregator 实现
    void oneShotVLM(const QString& sysPrompt,
                    const QString& userText,
                    const QList<QImage>& frames,
                    std::function<void(const QString&)> onDone);

    AgentService*    m_agent    = nullptr;
    VideoIndexer*    m_indexer  = nullptr;
    VideoRAGStore*   m_ragStore = nullptr;
    PlayerService*   m_player   = nullptr;
    EmbeddingService* m_embedder = nullptr;
};

#endif // FRAMEMIND_VIDEO_ANALYSIS_SERVICE_H
