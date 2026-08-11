#include "service/agent/reflection_engine.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <memory>

ReflectionEngine::ReflectionEngine(QObject* parent)
    : QObject(parent)
{
}

// ============================================================
// 主入口
// ============================================================

ReflectionResult ReflectionEngine::reflect(
    const QString& answer,
    const QVector<RetrievalResult>& evidence,
    QSharedPointer<VideoRepresentation> repr,
    const QVector<ToolResult>& toolTrace)
{
    ReflectionResult result;
    const bool hasSuccessfulToolEvidence = std::any_of(
        toolTrace.cbegin(), toolTrace.cend(),
        [](const ToolResult& item) {
            return item.success && !item.data.isEmpty()
                   && item.toolName != QLatin1String("control_player");
        });

    std::unique_ptr<ReflectionResult::Issue> consist(checkConsistency(answer, repr));
    std::unique_ptr<ReflectionResult::Issue> support(
        hasSuccessfulToolEvidence ? nullptr : checkEvidenceSupport(answer, evidence));
    std::unique_ptr<ReflectionResult::Issue> temporal(checkTemporalValidity(answer, repr));
    std::unique_ptr<ReflectionResult::Issue> halluc(checkHallucination(answer, repr));

    if (consist)  result.issues.append(*consist);
    if (support)  result.issues.append(*support);
    if (temporal) result.issues.append(*temporal);
    if (halluc)   result.issues.append(*halluc);

    result.valid = result.issues.isEmpty();
    // 简单置信度：无问题 0.9；每个问题扣 0.15，最低 0.3
    result.confidence = 0.9f - 0.15f * result.issues.size();
    if (result.confidence < 0.3f) result.confidence = 0.3f;

    if (!result.valid) {
        QStringList tips;
        for (const auto& iss : result.issues) tips.append(iss.detail);
        result.fixSuggestion = tips.join(QStringLiteral("；"));
    }
    return result;
}

// ============================================================
// 一致性
// ============================================================

ReflectionResult::Issue* ReflectionEngine::checkConsistency(
    const QString& answer, QSharedPointer<VideoRepresentation> repr) const
{
    if (!repr || repr->metadata.durationMs <= 0) return nullptr;

    // 极简：若答案宣称"这个视频没有音频"但 metadata.hasAudio=true → 矛盾
    static const QRegularExpression noAudio(QStringLiteral(u"(没有音频|无音频|静音视频)"));
    if (noAudio.match(answer).hasMatch() && repr->metadata.hasAudio) {
        auto* iss = new ReflectionResult::Issue;
        iss->kind = ReflectionResult::Issue::Inconsistency;
        iss->detail = QStringLiteral("答案宣称视频无音频，但元信息显示有音频");
        return iss;
    }
    return nullptr;
}

// ============================================================
// 证据支撑
// ============================================================

ReflectionResult::Issue* ReflectionEngine::checkEvidenceSupport(
    const QString& answer, const QVector<RetrievalResult>& evidence) const
{
    // 若答案很长（>200 字符）却没有引用任何证据，视为缺乏支撑
    if (answer.size() > 200 && evidence.isEmpty()) {
        auto* iss = new ReflectionResult::Issue;
        iss->kind = ReflectionResult::Issue::EvidenceMissing;
        iss->detail = QStringLiteral("答案长度较长但缺少可追溯的检索证据");
        return iss;
    }
    return nullptr;
}

// ============================================================
// 时间合理性
// ============================================================

ReflectionResult::Issue* ReflectionEngine::checkTemporalValidity(
    const QString& answer, QSharedPointer<VideoRepresentation> repr) const
{
    if (!repr || repr->metadata.durationMs <= 0) return nullptr;
    const auto stamps = extractTimestamps(answer);
    for (const int64_t ts : stamps) {
        if (ts < 0 || ts > repr->metadata.durationMs) {
            auto* iss = new ReflectionResult::Issue;
            iss->kind = ReflectionResult::Issue::TemporalError;
            iss->detail = QStringLiteral("答案中时间戳 %1ms 超出视频时长 %2ms")
                              .arg(ts).arg(repr->metadata.durationMs);
            return iss;
        }
    }
    return nullptr;
}

// ============================================================
// 幻觉检测
// ============================================================

ReflectionResult::Issue* ReflectionEngine::checkHallucination(
    const QString& answer, QSharedPointer<VideoRepresentation> repr) const
{
    if (!repr) return nullptr;

    // 收集"已知信息池"：场景描述 + 摘要 + 语音文本
    QString pool = repr->videoSummary;
    for (auto it = repr->sceneDescriptions.constBegin();
         it != repr->sceneDescriptions.constEnd(); ++it) {
        pool += QLatin1Char('\n') + it.value();
    }
    for (const auto& seg : repr->speechSegments) {
        pool += QLatin1Char('\n') + seg.text;
    }
    if (pool.isEmpty()) return nullptr;  // 未生成表示时不判断

    const QStringList claims = extractClaims(answer);
    QStringList unsupported;
    // 简化启发式：若声明中包含 3+ 个中文实词却没在信息池中出现，视为可能幻觉
    for (const auto& claim : claims) {
        if (claim.size() < 15) continue;
        // 抽词（简化：按标点切分取长度 ≥ 2 的子串）
        static const QRegularExpression splitRe(QStringLiteral(u"[，,。.；;、\\s]"));
        const QStringList tokens = claim.split(splitRe, Qt::SkipEmptyParts);
        int notFound = 0, total = 0;
        for (const auto& t : tokens) {
            if (t.size() < 2) continue;
            ++total;
            if (!pool.contains(t)) ++notFound;
        }
        if (total >= 3 && notFound > total * 2 / 3) {
            unsupported.append(claim);
        }
    }
    if (unsupported.isEmpty()) return nullptr;

    auto* iss = new ReflectionResult::Issue;
    iss->kind = ReflectionResult::Issue::Hallucination;
    iss->detail = QStringLiteral("以下断言在视频信息中未找到支撑：%1")
                      .arg(unsupported.join(QStringLiteral(" | ")));
    return iss;
}

// ============================================================
// 辅助
// ============================================================

QVector<int64_t> ReflectionEngine::extractTimestamps(const QString& answer) const
{
    QVector<int64_t> out;
    static const QRegularExpression re(
        QStringLiteral(R"(\[((\d{1,2}):)?(\d{1,2}):(\d{2})\])"));
    auto it = re.globalMatch(answer);
    while (it.hasNext()) {
        const auto m = it.next();
        const int hh = m.captured(2).toInt();
        const int mm = m.captured(3).toInt();
        const int ss = m.captured(4).toInt();
        const int64_t ms = (int64_t(hh) * 3600 + mm * 60 + ss) * 1000;
        out.append(ms);
    }
    return out;
}

QStringList ReflectionEngine::extractClaims(const QString& answer) const
{
    static const QRegularExpression re(QStringLiteral(u"[。.!?？；;]"));
    return answer.split(re, Qt::SkipEmptyParts);
}
