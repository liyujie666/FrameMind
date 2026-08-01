#ifndef FRAMEMIND_REFLECTION_ENGINE_H
#define FRAMEMIND_REFLECTION_ENGINE_H

#include <QObject>
#include <QString>
#include <QSharedPointer>
#include <QVector>

#include "model/agent_types.h"
#include "model/video_representation.h"
#include "model/retrieval_result.h"

/**
 * 反思引擎（agent-core-design.md §3.2 REFLECT）。
 *
 * 对 Agent 生成的答案做四项校验：
 *   1. 事实一致性：是否与已知视频信息矛盾
 *   2. 证据支撑度：每个断言是否有 evidence 支撑
 *   3. 时间合理性：所引用的时间点是否在视频范围内
 *   4. 幻觉检测：是否描述了视频中不存在的内容
 *
 * 返回 ReflectionResult：通过 or 附带问题清单与修复建议。
 */
class ReflectionEngine : public QObject {
    Q_OBJECT
public:
    explicit ReflectionEngine(QObject* parent = nullptr);

    /// 主入口
    ReflectionResult reflect(const QString& answer,
                              const QVector<RetrievalResult>& evidence,
                              QSharedPointer<VideoRepresentation> repr);

    /// 单项检查：事实一致性
    ReflectionResult::Issue* checkConsistency(
        const QString& answer,
        QSharedPointer<VideoRepresentation> repr) const;

    /// 单项检查：证据支撑度
    ReflectionResult::Issue* checkEvidenceSupport(
        const QString& answer,
        const QVector<RetrievalResult>& evidence) const;

    /// 单项检查：时间合理性
    ReflectionResult::Issue* checkTemporalValidity(
        const QString& answer,
        QSharedPointer<VideoRepresentation> repr) const;

    /// 单项检查：幻觉（提取声明与已知信息对比）
    ReflectionResult::Issue* checkHallucination(
        const QString& answer,
        QSharedPointer<VideoRepresentation> repr) const;

private:
    /// 从答案中抽取时间戳（[mm:ss] / [hh:mm:ss]），返回毫秒
    QVector<int64_t> extractTimestamps(const QString& answer) const;

    /// 从答案中抽取"断言"（简化：以句号/分号切分）
    QStringList extractClaims(const QString& answer) const;
};

#endif // FRAMEMIND_REFLECTION_ENGINE_H
