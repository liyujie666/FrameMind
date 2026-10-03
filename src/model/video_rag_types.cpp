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
            {"unit_understanding_version", unitUnderstandingVersion},
            {"unit_synthesis_prompt_version", unitSynthesisPromptVersion},
            {"chapter_prompt_version", chapterPromptVersion},
            {"overview_prompt_version", overviewPromptVersion},
            {"presentation_policy_version", presentationPolicyVersion},
            {"presentation_schema_version", presentationSchemaVersion},
            {"presentation_budget", presentationBudget.toJson()},
            {"carry_version", carryVersion}, {"grid_version", gridVersion},
            {"grid_max_edge", gridMaxEdge}, {"grid_jpeg_quality", gridJpegQuality},
            {"grid_min_cell_short_edge", gridMinCellShortEdge}, {"grid_label_height", gridLabelHeight},
            {"grid_max_encoded_bytes", gridMaxEncodedBytes},
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
    p.unitUnderstandingVersion = j["unit_understanding_version"].toString("legacy_pages_v1");
    p.unitSynthesisPromptVersion = j["unit_synthesis_prompt_version"].toString("legacy");
    p.chapterPromptVersion = j["chapter_prompt_version"].toString("legacy");
    p.overviewPromptVersion = j["overview_prompt_version"].toString("legacy");
    p.presentationPolicyVersion = j["presentation_policy_version"].toString("legacy");
    p.presentationSchemaVersion = j["presentation_schema_version"].toString("legacy");
    p.presentationBudget = VideoPresentationBudget::fromJson(j["presentation_budget"].toObject());
    if (j.contains("presentation_budget") && !j["presentation_budget"].isObject()) p.presentationBudget.maxRequests = -1;
    p.carryVersion = j["carry_version"].toString("legacy_carry");
    p.gridVersion = j["grid_version"].toString("legacy_multiframe");
    p.gridMaxEdge = j["grid_max_edge"].toInt(2048);
    p.gridJpegQuality = j["grid_jpeg_quality"].toInt(85);
    p.gridMinCellShortEdge = j["grid_min_cell_short_edge"].toInt(480);
    p.gridLabelHeight = j["grid_label_height"].toInt(32);
    p.gridMaxEncodedBytes = j["grid_max_encoded_bytes"].toVariant().toLongLong();
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
    QJsonArray pages;
    for (const auto &page : pageUnderstandings) pages.append(page.toJson());
    return {{"codec_valid", codecValid}, {"unit_id", unitId},
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
            {"page_understandings", pages},
            {"synthesis_state", artifactStateKey(synthesisState)},
            {"synthesis_points", synthesisPoints},
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
    for (const auto &page : j["page_understandings"].toArray())
        u.pageUnderstandings.append(UnitPageAnalysisResult::fromJson(page.toObject()));
    u.synthesisState = artifactStateFromKey(j["synthesis_state"].toString());
    u.synthesisPoints = j["synthesis_points"].toArray();
    u.codecValid = (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool())) &&
        (!j.contains("page_understandings") || j["page_understandings"].isArray()) &&
        (!j.contains("synthesis_points") || j["synthesis_points"].isArray()) &&
        (!j.contains("synthesis_state") || (j["synthesis_state"].isString() && artifactStateKey(u.synthesisState) == j["synthesis_state"].toString()));
    u.state = artifactStateFromKey(j["state"].toString());
    u.coverage = EvidenceCoverage::fromJson(j["coverage"].toObject());
    return u;
}
QJsonObject VideoBuildManifest::toJson() const {
    return {{"codec_valid", codecValid}, {"build_id", buildId},
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
            {"overview_state", artifactStateKey(overviewState)},
            {"presentation", presentation.toJson()},
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
    m.overviewState = artifactStateFromKey(j["overview_state"].toString());
    m.presentation = VideoPresentation::fromJson(j["presentation"].toObject());
    m.diagnostics = strings(j["diagnostics"]);
    m.artifacts = j["artifacts"].toObject();
    m.codecValid = (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool())) &&
        (!j.contains("presentation") || j["presentation"].isObject()) &&
        (!j.contains("overview_state") || (j["overview_state"].isString() && artifactStateKey(m.overviewState) == j["overview_state"].toString()));
    if (!m.codecValid) m.presentation.codecValid = false;
    return m;
}

QJsonObject UnitPageAnalysisResult::toJson() const {
    return {{"page_id", pageId}, {"page_ordinal", pageOrdinal},
        {"source_ids", QJsonArray::fromStringList(sourceIds)}, {"state", artifactStateKey(state)},
        {"title", title}, {"summary", summary}, {"visual_description", visualDescription},
        {"audio_summary", audioSummary}, {"error", error}, {"facts", facts}, {"codec_valid", codecValid},
        {"input_fingerprint", inputFingerprint}, {"carry_context", carryContext}};
}
UnitPageAnalysisResult UnitPageAnalysisResult::fromJson(const QJsonObject &j) {
    UnitPageAnalysisResult r;
    r.pageId = j["page_id"].toString(); r.pageOrdinal = j["page_ordinal"].toInt();
    r.sourceIds = strings(j["source_ids"]); r.state = artifactStateFromKey(j["state"].toString());
    r.title = j["title"].toString(); r.summary = j["summary"].toString();
    r.visualDescription = j["visual_description"].toString(); r.audioSummary = j["audio_summary"].toString();
    r.error = j["error"].toString(); r.facts = j["facts"].toArray();
    r.inputFingerprint = j["input_fingerprint"].toString(); r.carryContext = j["carry_context"].toObject();
    r.codecValid = !j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool());
    for (const auto &key : {"page_id", "title", "summary", "visual_description", "audio_summary", "error"})
        if (j.contains(key) && !j[key].isString()) r.codecValid = false;
    if (j.contains("page_ordinal") && (!j["page_ordinal"].isDouble() || j["page_ordinal"].toDouble() != r.pageOrdinal)) r.codecValid = false;
    if (!j["source_ids"].isArray() || (j.contains("facts") && !j["facts"].isArray())) r.codecValid = false;
    for (const auto &id : j["source_ids"].toArray()) if (!id.isString()) r.codecValid = false;
    if (j.contains("state") && (!j["state"].isString() || artifactStateKey(r.state) != j["state"].toString())) r.codecValid = false;
    if ((j.contains("input_fingerprint") && !j["input_fingerprint"].isString()) ||
        (j.contains("carry_context") && !j["carry_context"].isObject())) r.codecValid = false;
    return r;
}
QString UnitPageAnalysisResult::validationError(const QSet<QString> &allowed) const {
    if (!codecValid || pageId.isEmpty() || pageOrdinal < 0 || (state == ArtifactState::Ready && sourceIds.isEmpty())) return "invalid_page_result";
    QSet<QString> own;
    for (const auto &id : sourceIds) {
        if (id.isEmpty() || !allowed.contains(id) || own.contains(id)) return "invalid_page_sources";
        own.insert(id);
    }
    if (state == ArtifactState::Ready && summary.trimmed().isEmpty()) return "empty_ready_page";
    for (const auto &value : facts) {
        if (!value.isObject()) return "invalid_page_fact";
        const auto fact = value.toObject();
        if (fact["text"].toString().trimmed().isEmpty() || !fact["source_chunk_ids"].isArray() || fact["source_chunk_ids"].toArray().isEmpty()) return "invalid_page_fact";
        QSet<QString> seen;
        for (const auto &ref : fact["source_chunk_ids"].toArray()) {
            auto id = ref.toString();
            if (!ref.isString() || !own.contains(id) || seen.contains(id)) return "invalid_page_fact_source";
            seen.insert(id);
        }
    }
    return {};
}
