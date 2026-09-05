#ifndef FRAMEMIND_VIDEO_AGENT_H
#define FRAMEMIND_VIDEO_AGENT_H

#include <QObject>
#include <QString>
#include <QImage>
#include <QList>
#include <QSharedPointer>
#include <QPointer>

#include "model/agent_types.h"
#include "model/tool_types.h"
#include "model/videocontext.h"

class AgentService;
class VideoAnalysisService;
class VideoRAGRetriever;
class QACacheManager;
class ToolOrchestrator;
class PerceptionStrategy;
class ReflectionEngine;
class EntityTracker;
class WorkflowExecutor;
class WorkflowFactory;
class WorkflowCheckpoint;
struct VideoRepresentation;

/**
 * 视频分析 Agent 的顶层协调器（agent-core-design.md §3.1 五阶段决策循环）。
 *
 *   PERCEIVE  → PerceptionStrategy    决定要看哪里
 *   REPRESENT → VideoAnalysisService  组装/更新视频表示（RAG 检索 + 必要新分析）
 *   REASON    → AgentService + ToolOrchestrator  基于上下文推理/Tool Calling
 *   ACT       → ToolOrchestrator + 6 Tools       执行具体动作
 *   REFLECT   → ReflectionEngine      校验答案 + 决定是否需要修正
 *
 * 使用：
 *   agent->ask("conv-1", "视频后半段红衣人做了什么", frames, ctx, onDelta, onDone);
 *
 * ChatViewModel 层直接调用 ask，无需感知内部编排细节。
 */
class VideoAgent : public QObject {
    Q_OBJECT
public:
    explicit VideoAgent(AgentService*          agent,
                         VideoAnalysisService*  analysis,
                         VideoRAGRetriever*     retriever,
                         QACacheManager*        qaCache,
                         ToolOrchestrator*      orchestrator,
                         PerceptionStrategy*    perception,
                         ReflectionEngine*      reflection,
                         EntityTracker*         entities,
                         QObject*               parent = nullptr);

    /**
     * 主入口：处理一次用户提问，走完五阶段循环。
     * 回调都在主线程触发。
     */
    void ask(const QString& conversationId,
             const QString& question,
             const QList<QImage>& userFrames,
             const VideoContext& videoCtx,
             int64_t currentPlayerPosMs,
             std::function<void(const QString& delta)> onProgress,
             std::function<void(const AgentAnswer& answer)> onDone,
             std::function<void(const QString& error)> onError);

    /// 取消当前推理
    void cancel();

    /// 当前是否在处理请求
    bool isBusy() const { return m_busy; }

    /// 切换活跃视频（每次打开新视频调用一次）
    void setActiveVideo(const QString& videoPath, const QString& videoId);
    QString activeVideoPath() const { return m_activeVideoPath; }
    QString activeVideoId() const   { return m_activeVideoId; }

    /// 设置 Workflow 引擎（可选，设置后 ask() 将使用 Workflow 驱动）
    void setWorkflowExecutor(WorkflowExecutor* executor);
    void setWorkflowFactory(WorkflowFactory* factory);
    void setWorkflowCheckpoint(WorkflowCheckpoint* checkpoint);

    /// 是否使用 Workflow 模式
    bool useWorkflowMode() const { return m_workflowExecutor != nullptr; }

signals:
    void stageChanged(const QString& stage);     // "PERCEIVE" / "REASON" / ...
    void toolInvoked(const QString& toolName);
    void statusChanged(const QString& status);
    void reflectionIssueFound(const QString& detail);

private:
    // 五阶段的实现（内部调度）
    void phasePerceive(const QString& question,
                        QSharedPointer<VideoRepresentation> repr,
                        int64_t currentPosMs);
    void phaseReasonAndAct(const QString& convId,
                            const QString& question,
                            const QList<QImage>& userFrames,
                            const VideoContext& ctx);

    void finishAnswer(const AgentAnswer& answer);
    void failWith(const QString& err);

    QPointer<AgentService>          m_agent;
    QPointer<VideoAnalysisService>  m_analysis;
    QPointer<VideoRAGRetriever>     m_retriever;
    QPointer<QACacheManager>        m_qaCache;
    QPointer<ToolOrchestrator>      m_orchestrator;
    QPointer<PerceptionStrategy>    m_perception;
    QPointer<ReflectionEngine>      m_reflection;
    QPointer<EntityTracker>         m_entities;

    QString m_activeVideoPath;
    QString m_activeVideoId;

    // 当前请求状态
    bool     m_busy = false;
    QString  m_currentConvId;
    QString  m_currentQuestion;
    int64_t  m_currentPlayerPosMs = 0;
    QVector<RetrievalResult> m_retrievedEvidence;
    int      m_reflectionRetries = 0;          ///< 反思重试次数
    static constexpr int kMaxReflectionRetries = 1;  ///< 最多反思重试 1 次

    // 回调
    std::function<void(const QString&)> m_onProgress;
    std::function<void(const AgentAnswer&)> m_onDone;
    std::function<void(const QString&)> m_onError;

    // 保留上一次推理的上下文，供反思重试使用
    QList<QImage> m_lastUserFrames;
    VideoContext  m_lastVideoCtx;

    // Workflow 引擎（可选，为nullptr时走传统五阶段管道）
    QPointer<WorkflowExecutor>m_workflowExecutor;
    WorkflowFactory*            m_workflowFactory = nullptr;
    WorkflowCheckpoint*         m_workflowCheckpoint = nullptr;

    // Workflow 模式的内部方法
    void askViaWorkflow(const QString& conversationId,
                        const QString& question,
                        const QList<QImage>& userFrames,
                        const VideoContext& videoCtx,
                        int64_t currentPlayerPosMs);
};

#endif // FRAMEMIND_VIDEO_AGENT_H
