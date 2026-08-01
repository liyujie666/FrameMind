#ifndef FRAMEMIND_PERCEPTION_STRATEGY_H
#define FRAMEMIND_PERCEPTION_STRATEGY_H

#include <QObject>
#include <QString>
#include <QSharedPointer>

#include "model/agent_types.h"
#include "model/video_representation.h"

class VideoRAGRetriever;

/**
 * 感知策略（agent-core-design.md §3.2 PERCEIVE）。
 *
 * "决定看哪里" — 根据问题类型 + 已有视频表示，生成 SamplingPlan：
 *   - 是否需要新的感知？
 *   - 需要哪个时间区间？采样密度？
 *   - 目标帧数预算？
 *
 * 输出交给 VideoAnalysisService 执行实际的截帧 + VLM 分析。
 */
class PerceptionStrategy : public QObject {
    Q_OBJECT
public:
    explicit PerceptionStrategy(VideoRAGRetriever* retriever,
                                 QObject* parent = nullptr);

    /// 主入口：问题 + 视频表示 → 采样计划
    SamplingPlan decideSampling(const QString& question,
                                 QSharedPointer<VideoRepresentation> repr,
                                 int64_t currentPlayerPosMs = -1);

    /// 问题类型分类（agent-core-design.md §3.2 QuestionType）
    QuestionType classifyQuestion(const QString& question) const;

    /// 判断已有信息是否足够回答
    SufficiencyCheck checkSufficiency(const QString& question,
                                       QSharedPointer<VideoRepresentation> repr);

private:
    // 各问题类型的采样规划
    SamplingPlan planUniform(QSharedPointer<VideoRepresentation> repr,
                              SamplingPlan::Density density);
    SamplingPlan planTargeted(const QString& question,
                                QSharedPointer<VideoRepresentation> repr);
    SamplingPlan planEntityTracking(const QString& question,
                                      QSharedPointer<VideoRepresentation> repr);
    SamplingPlan planCausalContext(const QString& question,
                                     QSharedPointer<VideoRepresentation> repr);
    SamplingPlan planCurrentFrame(int64_t currentPosMs);
    SamplingPlan planExhaustive(const QString& question,
                                  QSharedPointer<VideoRepresentation> repr);

    /// 计算采样预算：purpose = "initial_overview" | "question_targeted" | "full_analysis"
    int computeBudget(int64_t durationMs, const QString& purpose) const;

    VideoRAGRetriever* m_retriever = nullptr;
};

#endif // FRAMEMIND_PERCEPTION_STRATEGY_H
