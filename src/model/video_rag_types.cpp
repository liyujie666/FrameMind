#include "model/video_rag_types.h"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace {
QStringList strings(const QJsonValue &v) {
    QStringList out;
    for (auto x : v.toArray())
        out << x.toString();
    return out;
}
} // namespace
QString contentTypeKey(VideoContentType t) {
    switch (t) {
    case VideoContentType::Meeting:
        return "meeting";
    case VideoContentType::Interview:
        return "interview";
    case VideoContentType::Educational:
        return "educational";
    case VideoContentType::Presentation:
        return "presentation";
    case VideoContentType::Tutorial:
        return "tutorial";
    case VideoContentType::Documentary:
        return "documentary";
    case VideoContentType::Drama:
        return "drama";
    case VideoContentType::News:
        return "news";
    case VideoContentType::Vlog:
        return "vlog";
    default:
        return "unknown";
    }
}
VideoContentType contentTypeFromKey(const QString &key) {
    for (int i = 0; i <= int(VideoContentType::Vlog); ++i)
        if (contentTypeKey(VideoContentType(i)) == key)
            return VideoContentType(i);
    return VideoContentType::Unknown;
}
QString contentTypeLabel(VideoContentType t) {
    switch (t) {
    case VideoContentType::Meeting:
        return QStringLiteral("会议");
    case VideoContentType::Interview:
        return QStringLiteral("访谈");
    case VideoContentType::Educational:
        return QStringLiteral("课程");
    case VideoContentType::Presentation:
        return QStringLiteral("报告／演讲");
    case VideoContentType::Tutorial:
        return QStringLiteral("操作教程");
    case VideoContentType::Documentary:
        return QStringLiteral("纪录片");
    case VideoContentType::Drama:
        return QStringLiteral("影视");
    case VideoContentType::News:
        return QStringLiteral("新闻");
    case VideoContentType::Vlog:
        return QStringLiteral("生活记录");
    default:
        return QStringLiteral("通用／未知");
    }
}
QString artifactStateKey(ArtifactState s) {
    switch (s) {
    case ArtifactState::Running:
        return "running";
    case ArtifactState::Ready:
        return "ready";
    case ArtifactState::Partial:
        return "partial";
    case ArtifactState::Failed:
        return "failed";
    case ArtifactState::Skipped:
        return "skipped";
    case ArtifactState::Cancelled:
        return "cancelled";
    default:
        return "pending";
    }
}
ArtifactState artifactStateFromKey(const QString &k) {
    for (int i = 0; i <= int(ArtifactState::Cancelled); ++i)
        if (artifactStateKey(ArtifactState(i)) == k)
            return ArtifactState(i);
    return ArtifactState::Pending;
}
QJsonObject VideoContentProfile::toJson() const {
    return {{"type", contentTypeKey(primaryType)},
            {"secondary_types", QJsonArray::fromStringList(secondaryTypes)},
            {"confidence", confidence},
            {"source", source},
            {"reasoning", reasoning},
            {"probe_evidence_ids", QJsonArray::fromStringList(probeEvidenceIds)},
            {"missing_signals", QJsonArray::fromStringList(missingSignals)},
            {"classifier_version", classifierVersion},
            {"user_override", userOverride}};
}
VideoContentProfile VideoContentProfile::fromJson(const QJsonObject &j) {
    VideoContentProfile p;
    p.primaryType = contentTypeFromKey(j["type"].toString());
    p.secondaryTypes = strings(j["secondary_types"]);
    p.confidence = j["confidence"].toDouble();
    p.source = j["source"].toString("fallback");
    p.reasoning = j["reasoning"].toString();
    p.probeEvidenceIds = strings(j["probe_evidence_ids"]);
    p.missingSignals = strings(j["missing_signals"]);
    p.classifierVersion = j["classifier_version"].toString("profile_v1");
    p.userOverride = j["user_override"].toBool();
    return p;
}
QJsonObject VideoRAGBuildPlan::toJson() const {
    return {{"strategy_id", strategyId},
            {"strategy_version", strategyVersion},
            {"prompt_version", promptVersion},
            {"schema_version", schemaVersion},
            {"unit_kind", unitKind},
            {"audio_first", audioFirst},
            {"require_speech", requireSpeech},
            {"asr", asrAvailable},
            {"text_vector", textVectorAvailable},
            {"visual_vector", visualVectorAvailable},
            {"min_unit_ms", qint64(minUnitMs)},
            {"max_unit_ms", qint64(maxUnitMs)},
            {"frame_interval_ms", qint64(frameIntervalMs)},
            {"frames_per_unit", framesPerUnit},
            {"page_chars", evidencePageChars},
            {"embedding_tokens", embeddingTokens},
            {"fact_kinds", QJsonArray::fromStringList(factKinds)},
            {"analysis_prompt", analysisPrompt},
            {"model_versions", modelVersions}};
}
VideoRAGBuildPlan VideoRAGBuildPlan::fromJson(const QJsonObject &j) {
    VideoRAGBuildPlan p;
    p.strategyId = j["strategy_id"].toString("generic_v1");
    p.strategyVersion = j["strategy_version"].toString("1");
    p.promptVersion = j["prompt_version"].toString("units_v1");
    p.schemaVersion = j["schema_version"].toString("facts_v1");
    p.unitKind = j["unit_kind"].toString("topic");
    p.audioFirst = j["audio_first"].toBool();
    p.requireSpeech = j["require_speech"].toBool();
    p.asrAvailable = j["asr"].toBool();
    p.textVectorAvailable = j["text_vector"].toBool();
    p.visualVectorAvailable = j["visual_vector"].toBool();
    p.minUnitMs = j["min_unit_ms"].toVariant().toLongLong();
    p.maxUnitMs = j["max_unit_ms"].toVariant().toLongLong();
    p.frameIntervalMs = j["frame_interval_ms"].toVariant().toLongLong();
    p.framesPerUnit = j["frames_per_unit"].toInt(3);
    p.evidencePageChars = j["page_chars"].toInt(4000);
    p.embeddingTokens = j["embedding_tokens"].toInt(500);
    p.factKinds = strings(j["fact_kinds"]);
    p.analysisPrompt = j["analysis_prompt"].toString();
    p.modelVersions = j["model_versions"].toObject();
    return p;
}
QString VideoRAGBuildPlan::fingerprint() const {
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(toJson()).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
QString VideoBuildContext::cancellationKey() const {
    return videoId + ":" + buildId + ":" + QString::number(taskGeneration);
}
QJsonObject EvidenceCoverage::toJson() const {
    QJsonArray pts;
    for (auto t : framePtsMs)
        pts.append(qint64(t));
    return {{"total_pages", totalPages},
            {"processed_pages", processedPages},
            {"failed_pages", QJsonArray::fromStringList(failedPages)},
            {"missing_capabilities", QJsonArray::fromStringList(missingCapabilities)},
            {"frame_pts_ms", pts}};
}
EvidenceCoverage EvidenceCoverage::fromJson(const QJsonObject &j) {
    EvidenceCoverage c;
    c.totalPages = j["total_pages"].toInt();
    c.processedPages = j["processed_pages"].toInt();
    c.failedPages = strings(j["failed_pages"]);
    c.missingCapabilities = strings(j["missing_capabilities"]);
    for (auto t : j["frame_pts_ms"].toArray())
        c.framePtsMs << t.toVariant().toLongLong();
    return c;
}
QJsonObject SemanticUnit::toJson() const {
    QJsonArray shots;
    for (auto i : shotIds)
        shots.append(i);
    return {{"unit_id", unitId},
            {"build_id", buildId},
            {"kind", kind},
            {"start_ms", qint64(startMs)},
            {"end_ms", qint64(endMs)},
            {"title", title},
            {"parent_id", parentUnitId},
            {"previous_id", previousUnitId},
            {"next_id", nextUnitId},
            {"shot_ids", shots},
            {"source_chunk_ids", QJsonArray::fromStringList(sourceChunkIds)},
            {"visual_description", visualDescription},
            {"audio_summary", audioSummary},
            {"fused_description", fusedDescription},
            {"facts", facts},
            {"state", artifactStateKey(state)},
            {"coverage", coverage.toJson()}};
}
SemanticUnit SemanticUnit::fromJson(const QJsonObject &j) {
    SemanticUnit u;
    u.unitId = j["unit_id"].toString();
    u.buildId = j["build_id"].toString();
    u.kind = j["kind"].toString("topic");
    u.startMs = j["start_ms"].toVariant().toLongLong();
    u.endMs = j["end_ms"].toVariant().toLongLong();
    u.title = j["title"].toString();
    u.parentUnitId = j["parent_id"].toString();
    u.previousUnitId = j["previous_id"].toString();
    u.nextUnitId = j["next_id"].toString();
    for (auto i : j["shot_ids"].toArray())
        u.shotIds << i.toInt();
    u.sourceChunkIds = strings(j["source_chunk_ids"]);
    u.visualDescription = j["visual_description"].toString();
    u.audioSummary = j["audio_summary"].toString();
    u.fusedDescription = j["fused_description"].toString();
    u.facts = j["facts"].toArray();
    u.state = artifactStateFromKey(j["state"].toString());
    u.coverage = EvidenceCoverage::fromJson(j["coverage"].toObject());
    return u;
}
QJsonObject VideoBuildManifest::toJson() const {
    return {{"build_id", buildId},
            {"video_id", videoId},
            {"file_path", filePath},
            {"file_fingerprint", fileFingerprint},
            {"raw_snapshot_id", rawSnapshotId},
            {"spec_fingerprint", specFingerprint},
            {"profile", profile.toJson()},
            {"plan", plan.toJson()},
            {"state", artifactStateKey(state)},
            {"revision", revision},
            {"summary", summary},
            {"diagnostics", QJsonArray::fromStringList(diagnostics)},
            {"artifacts", artifacts}};
}
VideoBuildManifest VideoBuildManifest::fromJson(const QJsonObject &j) {
    VideoBuildManifest m;
    m.buildId = j["build_id"].toString();
    m.videoId = j["video_id"].toString();
    m.filePath = j["file_path"].toString();
    m.fileFingerprint = j["file_fingerprint"].toString();
    m.rawSnapshotId = j["raw_snapshot_id"].toString();
    m.specFingerprint = j["spec_fingerprint"].toString();
    m.profile = VideoContentProfile::fromJson(j["profile"].toObject());
    m.plan = VideoRAGBuildPlan::fromJson(j["plan"].toObject());
    m.state = artifactStateFromKey(j["state"].toString());
    m.revision = j["revision"].toInt(1);
    m.summary = j["summary"].toString();
    m.diagnostics = strings(j["diagnostics"]);
    m.artifacts = j["artifacts"].toObject();
    return m;
}
