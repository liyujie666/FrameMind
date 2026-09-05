#ifndef FRAMEMIND_QUERY_PLAN_H
#define FRAMEMIND_QUERY_PLAN_H

#include <QString>
#include <cstdint>

/**
 * 本地、确定性的检索计划。
 *
 * 它只描述检索约束与证据偏好，不发起额外 LLM 请求，因此可以安全用于
 * 前台对话、后台索引和工作流节点。
 */
struct QueryPlan {
    QString normalizedQuery;

    bool retrieveText = true;
    bool retrieveVisual = true;
    bool retrieveEntity = false;
    bool needsLocalVerification = false;

    bool prefersVisualEvidence = false;
    bool prefersAudioEvidence = false;
    bool prefersFusedEvidence = true;

    int64_t startMs = -1;
    int64_t endMs = -1;
    bool hasTemporalConstraint = false;
    bool temporalConstraintUnsatisfiable = false;
    QString temporalHint;

    int candidateMultiplier = 2;

    bool hasTimeRange() const { return startMs >= 0 && endMs >= startMs; }
};

#endif // FRAMEMIND_QUERY_PLAN_H
