#ifndef FRAMEMIND_AGENT_TYPES_H
#define FRAMEMIND_AGENT_TYPES_H

#include <QString>
#include <QVector>
#include <QVariantMap>
#include <QMetaType>
#include <cstdint>

#include "model/retrieval_result.h"

/**
 * Agent 核心决策阶段共用的枚举与结构体。
 * 对齐 agent-core-design.md §3。
 */

// ---- 问题类型分类（§3.2 PERCEIVE）----
enum class QuestionType {
    GlobalSummary,         // "这个视频讲了什么"
    TemporalLocalization,  // "什么时候出现了 X"
    EntityQuery,           // "那个红衣人是谁"
    SpatialQuery,          // "画面左上角"
    ActionRecognition,     // "他在做什么"
    CausalReasoning,       // "为什么..."
    Counterfactual,        // "如果...会怎样"
    Comparison,            // "A 和 B 的不同"
    Counting,              // "出现了几次"
    CurrentFrame,          // "现在画面里有什么"
    TemporalOrder,         // "A 在 B 之前还是之后"
    Duration,              // "持续了多久"
    DetailDescription,     // "详细描述"
    Unknown
};

// ---- 采样计划（§3.2 PERCEIVE.decide_sampling）----
struct SamplingPlan {
    enum Density { None, Sparse, Medium, Dense };

    bool                needNewPerception = false;
    Density             density = Sparse;

    /// 需要采样的时间区间列表（可多段）
    QVector<QPair<int64_t, int64_t>> timeRanges;

    /// 单点采样（如 CURRENT_FRAME 场景）
    QVector<int64_t> exactTimestampsMs;

    /// 预算：目标帧数
    int frameBudget = 8;

    /// 关注点提示（送给 VLM）
    QString focusHint;
};

// ---- 信息充分性检查结果 ----
struct SufficiencyCheck {
    bool    isEnough = false;
    QString reason;                    // 不足时的说明
    QString suggestedAction;           // 建议的下一步（如 "search_video_content"）
    QVariantMap suggestedArgs;
};

// ---- 推理阶段状态（§3.2 REASON）----
enum class ReasoningStatus {
    Complete,           // 得出可信答案
    NeedMoreInfo,       // 需要更多感知/检索
    NeedVerification,   // 置信度低，需要验证
    Failed              // 无法完成
};

struct ReasoningResult {
    ReasoningStatus status = ReasoningStatus::Failed;
    QString         answer;
    float           confidence = 0.0f;

    // 证据链
    QVector<RetrievalResult> evidence;

    // 需要下一步动作时携带
    QString      nextAction;
    QVariantMap  nextActionArgs;

    // 全过程思路痕迹（供反思用）
    QStringList reasoningTrace;
};

// ---- 反思校验结果（§3.2 REFLECT）----
struct ReflectionResult {
    struct Issue {
        enum Kind { Inconsistency, EvidenceMissing, TemporalError, Hallucination };
        Kind    kind;
        QString detail;
    };

    bool            valid = false;
    float           confidence = 0.0f;
    QVector<Issue>  issues;
    QString         fixSuggestion;
};

// ---- 单轮 Agent 最终对外结果 ----
struct AgentAnswer {
    QString conversationId;
    QString question;
    QString answer;                        // Markdown
    float   confidence = 0.0f;

    QVector<RetrievalResult> evidence;
    QVector<QString>         toolCallsTrace; // 工具调用序列
    int                      rounds = 0;    // 使用的 Tool Round 数
    bool                     fromCache = false;
};

Q_DECLARE_METATYPE(SamplingPlan)
Q_DECLARE_METATYPE(ReasoningResult)
Q_DECLARE_METATYPE(ReflectionResult)
Q_DECLARE_METATYPE(AgentAnswer)

#endif // FRAMEMIND_AGENT_TYPES_H
