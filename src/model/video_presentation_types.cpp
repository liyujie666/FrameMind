#include "model/video_presentation_types.h"
#include <QJsonDocument>
#include <cmath>

namespace {
QStringList strings(const QJsonValue &v) {
    QStringList result;
    for (const auto &x : v.toArray()) result << x.toString();
    return result;
}
template<class T> QJsonArray objects(const QVector<T> &v) {
    QJsonArray result;
    for (const auto &x : v) result.append(x.toJson());
    return result;
}
template<class T> QVector<T> decodeObjects(const QJsonValue &v) {
    QVector<T> result;
    for (const auto &x : v.toArray()) result.append(T::fromJson(x.toObject()));
    return result;
}
bool references(const QStringList &ids, const QSet<QString> &allowed, bool required = true) {
    if (required && ids.isEmpty()) return false;
    QSet<QString> seen;
    for (const auto &id : ids) {
        if (id.isEmpty() || seen.contains(id) || !allowed.contains(id)) return false;
        seen.insert(id);
    }
    return true;
}
bool range(qint64 start, qint64 end, qint64 duration) {
    return start >= 0 && end > start && duration > 0 && end <= duration;
}
bool complete(ArtifactState s) {
    return s == ArtifactState::Ready || s == ArtifactState::Partial || s == ArtifactState::Skipped;
}
// Validate JSON shape before typed decoding so a dropped field never becomes
// a fabricated valid timestamp, empty attribute object or completed section.
bool shape(const QJsonObject &j, const QStringList &str, const QStringList &lists,
           const QStringList &numbers = {}, const QStringList &states = {}) {
    for (const auto &k : str) if (j.contains(k) && !j[k].isString()) return false;
    for (const auto &k : lists) {
        if (j.contains(k) && !j[k].isArray()) return false;
        for (const auto &v : j[k].toArray()) if (!v.isString()) return false;
    }
    for (const auto &k : numbers) {
        const auto v = j[k];
        if (!v.isDouble() || !std::isfinite(v.toDouble()) ||
            std::floor(v.toDouble()) != v.toDouble() || v.toDouble() < 0 ||
            v.toDouble() > 9007199254740991.0) return false;
    }
    for (const auto &k : states) if (j.contains(k) &&
        (!j[k].isString() || artifactStateKey(artifactStateFromKey(j[k].toString())) != j[k].toString())) return false;
    return true;
}
bool presentationShape(const QJsonObject &j) {
    if (j.contains("policy_selection") && !j["policy_selection"].isObject()) return false;
    if (!shape(j, {"schema_version", "policy_id", "policy_version"}, {"diagnostics"}, {}, {"chapters_state"})) return false;
    for (const auto &key : {"chapters", "anchors"}) {
        if (j.contains(key) && !j[key].isArray()) return false;
        for (const auto &v : j[key].toArray()) {
            if (!v.isObject()) return false;
            const auto o = v.toObject();
            if (!shape(o, {"chapter_id", "anchor_id", "title", "description", "precision", "previous_chapter_id", "next_chapter_id"},
                {"unit_ids", "source_chunk_ids"}, {"start_ms", "end_ms"}, {"state"})) return false;
        }
    }
    for (const auto &key : {"primary_section", "secondary_section"}) {
        if (j.contains(key) && !j[key].isObject()) return false;
        const auto s = j[key].toObject();
        if (!shape(s, {"kind", "title"}, {}, {}, {"state"})) return false;
        for (const auto &items : {"entries", "questions"}) {
            if (s.contains(items) && !s[items].isArray()) return false;
            for (const auto &v : s[items].toArray()) {
                if (!v.isObject()) return false;
                const auto o = v.toObject();
                if (!shape(o, {"entry_id", "question_id", "kind", "title", "body", "text", "intent", "anchor_id"},
                    {"unit_ids", "source_chunk_ids", "related_entry_ids"})) return false;
                if (o.contains("attributes") && !o["attributes"].isObject()) return false;
                if (o.contains("points") && !o["points"].isArray()) return false;
                for (const auto &point : o["points"].toArray()) if (!point.isObject() ||
                    !shape(point.toObject(), {"role", "text"}, {"source_chunk_ids"})) return false;
            }
        }
    }
    return !j.contains("codec_valid") || j["codec_valid"].isBool();
}
} // namespace
QJsonObject VideoContentPoint::toJson() const {
    return {{"codec_valid", codecValid},
            {"role", role},
            {"text", text},
            {"source_chunk_ids", QJsonArray::fromStringList(sourceChunkIds)}};
}
VideoContentPoint VideoContentPoint::fromJson(const QJsonObject &j) {
    VideoContentPoint v;
    v.role = j["role"].toString();
    v.text = j["text"].toString();
    v.sourceChunkIds = strings(j["source_chunk_ids"]);
    v.codecValid = shape(j, {"role", "text"}, {"source_chunk_ids"}) && (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool()));
    return v;
}
QJsonObject VideoChapter::toJson() const {
    return {{"codec_valid", codecValid},
            {"chapter_id", chapterId},
            {"start_ms", startMs},
            {"end_ms", endMs},
            {"title", title},
            {"description", description},
            {"unit_ids", QJsonArray::fromStringList(unitIds)},
            {"source_chunk_ids", QJsonArray::fromStringList(sourceChunkIds)},
            {"previous_chapter_id", previousChapterId},
            {"next_chapter_id", nextChapterId},
            {"incomplete_reasons", QJsonArray::fromStringList(incompleteReasons)},
            {"state", artifactStateKey(state)}};
}
VideoChapter VideoChapter::fromJson(const QJsonObject &j) {
    VideoChapter v;
    v.chapterId = j["chapter_id"].toString();
    v.startMs = j["start_ms"].toVariant().toLongLong();
    v.endMs = j["end_ms"].toVariant().toLongLong();
    v.title = j["title"].toString();
    v.description = j["description"].toString();
    v.unitIds = strings(j["unit_ids"]);
    v.sourceChunkIds = strings(j["source_chunk_ids"]);
    v.previousChapterId = j["previous_chapter_id"].toString();
    v.nextChapterId = j["next_chapter_id"].toString();
    v.state = artifactStateFromKey(j["state"].toString());
    v.incompleteReasons = strings(j["incomplete_reasons"]);
    v.codecValid = shape(j, {"chapter_id", "title", "description", "previous_chapter_id", "next_chapter_id"}, {"unit_ids", "source_chunk_ids"}, {"start_ms", "end_ms"}, {"state"}) && (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool()));
    if (j.contains("incomplete_reasons")) {
        if (!j["incomplete_reasons"].isArray()) v.codecValid = false;
        for (const auto& reason : j["incomplete_reasons"].toArray())
            if (!reason.isString()) v.codecValid = false;
    }
    return v;
}
QJsonObject VideoReviewAnchor::toJson() const {
    return {{"codec_valid", codecValid},
            {"anchor_id", anchorId},
            {"start_ms", startMs},
            {"end_ms", endMs},
            {"source_chunk_ids", QJsonArray::fromStringList(sourceChunkIds)},
            {"unit_ids", QJsonArray::fromStringList(unitIds)},
            {"precision", precision}};
}
VideoReviewAnchor VideoReviewAnchor::fromJson(const QJsonObject &j) {
    VideoReviewAnchor v;
    v.anchorId = j["anchor_id"].toString();
    v.startMs = j["start_ms"].toVariant().toLongLong();
    v.endMs = j["end_ms"].toVariant().toLongLong();
    v.sourceChunkIds = strings(j["source_chunk_ids"]);
    v.unitIds = strings(j["unit_ids"]);
    v.precision = j["precision"].toString();
    v.codecValid = shape(j, {"anchor_id", "precision"}, {"unit_ids", "source_chunk_ids"}, {"start_ms", "end_ms"}) && (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool()));
    return v;
}
QJsonObject VideoContentEntry::toJson() const {
    return {{"codec_valid", codecValid},
            {"entry_id", entryId},
            {"kind", kind},
            {"title", title},
            {"body", body},
            {"points", objects(points)},
            {"unit_ids", QJsonArray::fromStringList(unitIds)},
            {"source_chunk_ids", QJsonArray::fromStringList(sourceChunkIds)},
            {"anchor_id", anchorId},
            {"anchor_point_index", anchorPointIndex},
            {"attributes", attributes}};
}
VideoContentEntry VideoContentEntry::fromJson(const QJsonObject &j) {
    VideoContentEntry v;
    v.entryId = j["entry_id"].toString();
    v.kind = j["kind"].toString();
    v.title = j["title"].toString();
    v.body = j["body"].toString();
    v.points = decodeObjects<VideoContentPoint>(j["points"]);
    v.unitIds = strings(j["unit_ids"]);
    v.sourceChunkIds = strings(j["source_chunk_ids"]);
    v.anchorId = j["anchor_id"].toString();
    const auto index = j["anchor_point_index"];
    v.anchorPointIndex = index.toInt(-1);
    v.attributes = j["attributes"].toObject();
    v.codecValid = shape(j, {"entry_id", "kind", "title", "body", "anchor_id"}, {"unit_ids", "source_chunk_ids"}) && (!j.contains("attributes") || j["attributes"].isObject()) && (!j.contains("points") || j["points"].isArray()) && (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool()));
    if ((!v.anchorId.isEmpty() && index.isUndefined()) || (!index.isUndefined() && (!index.isDouble() || index.toDouble() < -1 ||
        std::floor(index.toDouble()) != index.toDouble() || index.toDouble() > 2147483647.0))) v.codecValid = false;
    return v;
}
QJsonObject VideoExploreQuestion::toJson() const {
    return {{"codec_valid", codecValid},
            {"question_id", questionId},
            {"text", text},
            {"intent", intent},
            {"related_entry_ids", QJsonArray::fromStringList(relatedEntryIds)},
            {"unit_ids", QJsonArray::fromStringList(unitIds)},
            {"source_chunk_ids", QJsonArray::fromStringList(sourceChunkIds)}};
}
VideoExploreQuestion VideoExploreQuestion::fromJson(const QJsonObject &j) {
    VideoExploreQuestion v;
    v.questionId = j["question_id"].toString();
    v.text = j["text"].toString();
    v.intent = j["intent"].toString();
    v.relatedEntryIds = strings(j["related_entry_ids"]);
    v.unitIds = strings(j["unit_ids"]);
    v.sourceChunkIds = strings(j["source_chunk_ids"]);
    v.codecValid = shape(j, {"question_id", "text", "intent"}, {"related_entry_ids", "unit_ids", "source_chunk_ids"}) && (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool()));
    return v;
}
QJsonObject VideoSummarySection::toJson() const {
    return {{"codec_valid", codecValid},
            {"kind", kind},
            {"title", title},
            {"state", artifactStateKey(state)},
            {"entries", objects(entries)},
            {"questions", objects(questions)}};
}
VideoSummarySection VideoSummarySection::fromJson(const QJsonObject &j) {
    VideoSummarySection v;
    v.kind = j["kind"].toString();
    v.title = j["title"].toString();
    v.state = artifactStateFromKey(j["state"].toString());
    v.entries = decodeObjects<VideoContentEntry>(j["entries"]);
    v.questions = decodeObjects<VideoExploreQuestion>(j["questions"]);
    v.codecValid = shape(j, {"kind", "title"}, {}, {}, {"state"}) && (!j.contains("entries") || j["entries"].isArray()) && (!j.contains("questions") || j["questions"].isArray()) && (!j.contains("codec_valid") || (j["codec_valid"].isBool() && j["codec_valid"].toBool()));
    return v;
}
QJsonObject VideoPresentation::toJson() const {
    return {{"schema_version", schemaVersion},
            {"policy_id", policyId},
            {"policy_version", policyVersion},
            {"policy_selection", policySelection},
            {"chapters_state", artifactStateKey(chaptersState)},
            {"chapters", objects(chapters)},
            {"anchors", objects(anchors)},
            {"primary_section", primarySection.toJson()},
            {"secondary_section", secondarySection.toJson()},
            {"diagnostics", QJsonArray::fromStringList(diagnostics)},
            {"codec_valid", codecValid}};
}
VideoPresentation VideoPresentation::fromJson(const QJsonObject &j) {
    VideoPresentation v;
    v.schemaVersion = j["schema_version"].toString("legacy");
    v.policyId = j["policy_id"].toString();
    v.policyVersion = j["policy_version"].toString();
    v.policySelection = j["policy_selection"].toObject();
    v.chaptersState = artifactStateFromKey(j["chapters_state"].toString());
    v.chapters = decodeObjects<VideoChapter>(j["chapters"]);
    v.anchors = decodeObjects<VideoReviewAnchor>(j["anchors"]);
    v.primarySection = VideoSummarySection::fromJson(j["primary_section"].toObject());
    v.secondarySection = VideoSummarySection::fromJson(j["secondary_section"].toObject());
    v.diagnostics = strings(j["diagnostics"]);
    v.codecValid = presentationShape(j) && j["codec_valid"].toBool(true);
    return v;
}
QString VideoContentPoint::validationError(const VideoPresentationValidationContext &c) const {
    if (!codecValid) return "invalid_decoded_presentation_item";
    if (role.isEmpty() || text.trimmed().isEmpty() || !references(sourceChunkIds, c.sourceIds)) return "invalid_content_point";
    return {};
}
QString VideoChapter::validationError(const VideoPresentationValidationContext &c) const {
    if (!codecValid) return "invalid_decoded_presentation_item";
    if (chapterId.isEmpty() || !range(startMs, endMs, c.durationMs) || !references(unitIds, c.unitIds)) return "invalid_chapter";
    if (!references(sourceChunkIds, c.sourceIds, state == ArtifactState::Ready || state == ArtifactState::Partial)) return "invalid_chapter_sources";
    if ((state == ArtifactState::Ready || state == ArtifactState::Partial) && (title.trimmed().isEmpty() || description.trimmed().isEmpty())) return "empty_chapter_content";
    return {};
}
QString VideoReviewAnchor::validationError(const VideoPresentationValidationContext &c) const {
    if (!codecValid) return "invalid_decoded_presentation_item";
    if (anchorId.isEmpty() || !range(startMs, endMs, c.durationMs) ||
        (precision != "evidence" && precision != "unit") ||
        !references(sourceChunkIds, c.sourceIds) || !references(unitIds, c.unitIds)) return "invalid_review_anchor";
    return {};
}
QString VideoContentEntry::validationError(const VideoPresentationValidationContext &c) const {
    if (!codecValid) return "invalid_decoded_presentation_item";
    if (entryId.isEmpty() || kind.isEmpty() || title.trimmed().isEmpty() || (body.trimmed().isEmpty() && points.isEmpty()) ||
        !references(unitIds, c.unitIds) || !references(sourceChunkIds, c.sourceIds)) return "invalid_content_entry";
    VideoPresentationValidationContext own = c;
    own.sourceIds = QSet<QString>(sourceChunkIds.begin(), sourceChunkIds.end());
    for (const auto &p : points) { auto e = p.validationError(own); if (!e.isEmpty()) return e; }
    if ((anchorId.isEmpty() && anchorPointIndex != -1) || (!anchorId.isEmpty() &&
        (anchorPointIndex < 0 || anchorPointIndex >= points.size()))) return "invalid_anchor_main_point";
    return {};
}
QString VideoExploreQuestion::validationError(const VideoPresentationValidationContext &c) const {
    if (!codecValid) return "invalid_decoded_presentation_item";
    if (questionId.isEmpty() || text.trimmed().isEmpty() || intent.isEmpty() ||
        !references(unitIds, c.unitIds) || !references(sourceChunkIds, c.sourceIds)) return "invalid_explore_question";
    return {};
}
QString VideoPresentation::validationError(const VideoPresentationValidationContext &c) const {
    const bool legacyEmpty = schemaVersion == "legacy" && chaptersState == ArtifactState::Pending &&
        primarySection.state == ArtifactState::Pending && secondarySection.state == ArtifactState::Pending &&
        chapters.isEmpty() && anchors.isEmpty() && primarySection.entries.isEmpty() && primarySection.questions.isEmpty() &&
        secondarySection.entries.isEmpty() && secondarySection.questions.isEmpty() && policyId.isEmpty() && policySelection.isEmpty();
    if (codecValid && legacyEmpty) return {};
    if (!codecValid || schemaVersion != "presentation_v1") return "unsupported_or_invalid_presentation_schema";
    const bool populated = !chapters.isEmpty() || !anchors.isEmpty() || !primarySection.entries.isEmpty() ||
        !primarySection.questions.isEmpty() || !secondarySection.entries.isEmpty() || !secondarySection.questions.isEmpty() ||
        chaptersState != ArtifactState::Pending || primarySection.state != ArtifactState::Pending || secondarySection.state != ArtifactState::Pending;
    if (populated && (policyId.isEmpty() || policyVersion.isEmpty())) return "missing_presentation_policy";
    if (!policySelection.isEmpty()) {
        if (!policySelection["mode"].isString() ||
            (policySelection["mode"].toString() != "inherit" && policySelection["mode"].toString() != "demo") ||
            !policySelection["reason"].isString() || policySelection["reason"].toString().trimmed().isEmpty() ||
            !policySelection["source_chunk_ids"].isArray()) return "invalid_policy_selection";
        for (const auto& source : policySelection["source_chunk_ids"].toArray()) if (!source.isString()) return "invalid_policy_selection_source";
        if (!references(strings(policySelection["source_chunk_ids"]), c.sourceIds, policySelection["mode"].toString() == "demo")) return "invalid_policy_selection_source";
        if ((policyId == "demo_v1") != (policySelection["mode"].toString() == "demo")) return "policy_mode_mismatch";
    }
    QSet<QString> ids, usedUnits, entryIds;
    auto unique = [&](const QString &id) { if (id.isEmpty() || ids.contains(id)) return false; ids.insert(id); return true; };
    qint64 lastEnd = 0;
    for (qsizetype i = 0; i < chapters.size(); ++i) {
        const auto &ch = chapters[i]; auto e = ch.validationError(c); if (!e.isEmpty()) return e;
        if (!unique(ch.chapterId) || ch.startMs < lastEnd) return "duplicate_or_overlapping_chapter";
        if (ch.previousChapterId != (i ? chapters[i-1].chapterId : QString()) ||
            ch.nextChapterId != (i+1 < chapters.size() ? chapters[i+1].chapterId : QString())) return "invalid_chapter_links";
        for (const auto &u : ch.unitIds) { if (usedUnits.contains(u)) return "duplicate_chapter_unit"; usedUnits.insert(u); }
        lastEnd = ch.endMs;
    }
    if ((chaptersState == ArtifactState::Ready || chaptersState == ArtifactState::Partial) && chapters.isEmpty()) return "empty_completed_chapters";
    QSet<QString> anchorIds;
    for (const auto &a : anchors) {
        auto e = a.validationError(c); if (!e.isEmpty()) return e;
        if (!unique(a.anchorId)) return "duplicate_anchor_id";
        anchorIds.insert(a.anchorId);
    }
    for (const auto *section : {&primarySection, &secondarySection}) {
        if (!section->codecValid) return "invalid_decoded_section";
        if (section->state == ArtifactState::Skipped && (!section->entries.isEmpty() || !section->questions.isEmpty())) return "nonempty_skipped_section";
        if (section->state == ArtifactState::Ready && section->entries.isEmpty() && section->questions.isEmpty()) return "empty_ready_section";
        if (section->state == ArtifactState::Failed && (!section->entries.isEmpty() || !section->questions.isEmpty())) return "nonempty_failed_section";
        for (const auto &entry : section->entries) {
            auto e = entry.validationError(c); if (!e.isEmpty()) return e;
            if (!unique(entry.entryId)) return "duplicate_entry_id";
            entryIds.insert(entry.entryId);
            if (!entry.anchorId.isEmpty()) {
                if (!anchorIds.contains(entry.anchorId)) return "unknown_anchor_id";
                for (const auto &a : anchors) if (a.anchorId == entry.anchorId) {
                    if (!references(a.unitIds, QSet<QString>(entry.unitIds.begin(), entry.unitIds.end())) ||
                        !references(a.sourceChunkIds, QSet<QString>(entry.sourceChunkIds.begin(), entry.sourceChunkIds.end()))) return "anchor_entry_source_mismatch";
                    if (!references(a.sourceChunkIds, QSet<QString>(entry.points[entry.anchorPointIndex].sourceChunkIds.begin(),
                        entry.points[entry.anchorPointIndex].sourceChunkIds.end()))) return "anchor_main_point_mismatch";
                }
            }
        }
    }
    for (const auto *section : {&primarySection, &secondarySection}) for (const auto &q : section->questions) {
        auto e = q.validationError(c); if (!e.isEmpty()) return e;
        if (!unique(q.questionId) || !references(q.relatedEntryIds, entryIds, false)) return "invalid_question_relation";
    }
    return {};
}
bool VideoPresentation::hasCompletedSections() const {
    if (!codecValid || !primarySection.codecValid || !secondarySection.codecValid || schemaVersion != "presentation_v1" ||
        policySelection.isEmpty() || policySelection["status"].toString() != "ready" ||
        policyId.isEmpty() || policyVersion.isEmpty() || !complete(chaptersState) ||
        !complete(primarySection.state) || !complete(secondarySection.state)) return false;
    if ((chaptersState == ArtifactState::Ready || chaptersState == ArtifactState::Partial) && chapters.isEmpty()) return false;
    for (const auto &ch : chapters) if (!ch.codecValid || !complete(ch.state)) return false;
    for (const auto &anchor : anchors) if (!anchor.codecValid) return false;
    for (const auto *section : {&primarySection, &secondarySection}) {
        if (section->state == ArtifactState::Ready && section->entries.isEmpty() && section->questions.isEmpty()) return false;
        if (section->state == ArtifactState::Skipped && (!section->entries.isEmpty() || !section->questions.isEmpty())) return false;
        for (const auto &entry : section->entries) {
            if (!entry.codecValid) return false;
            for (const auto &point : entry.points) if (!point.codecValid) return false;
        }
        for (const auto &question : section->questions) if (!question.codecValid) return false;
    }
    return true; // prerequisite only; reference/policy validation still needs the build context
}
QJsonObject VideoPresentationBudget::toJson() const {
    return {{"page_input_chars", pageInputChars},
            {"synthesis_input_chars", synthesisInputChars},
            {"chapter_window_chars", chapterWindowChars},
            {"max_output_chars", maxOutputChars},
            {"max_page_bytes", maxPageBytes},
            {"max_unit_bytes", maxUnitBytes},
            {"max_presentation_bytes", maxPresentationBytes},
            {"max_manifest_bytes", maxManifestBytes},
            {"max_reduction_depth", maxReductionDepth},
            {"max_requests", maxRequests},
            {"max_retries", maxRetries},
            {"model_context_tokens", modelContextTokens},
            {"reserved_output_tokens", reservedOutputTokens},
            {"protocol_overhead_tokens", protocolOverheadTokens},
            {"fallback_input_chars", fallbackInputChars},
            {"overview_output_chars", overviewOutputChars},
            {"reduction_output_chars", reductionOutputChars},
            {"reduction_output_tokens", reductionOutputTokens}};
}
VideoPresentationBudget VideoPresentationBudget::fromJson(const QJsonObject &j) {
    VideoPresentationBudget v;
    if (j.contains("page_input_chars")) v.pageInputChars = j["page_input_chars"].isDouble() && std::floor(j["page_input_chars"].toDouble()) == j["page_input_chars"].toDouble() ? j["page_input_chars"].toInt(-1) : -1;
    if (j.contains("synthesis_input_chars")) v.synthesisInputChars = j["synthesis_input_chars"].isDouble() && std::floor(j["synthesis_input_chars"].toDouble()) == j["synthesis_input_chars"].toDouble() ? j["synthesis_input_chars"].toInt(-1) : -1;
    if (j.contains("chapter_window_chars")) v.chapterWindowChars = j["chapter_window_chars"].isDouble() && std::floor(j["chapter_window_chars"].toDouble()) == j["chapter_window_chars"].toDouble() ? j["chapter_window_chars"].toInt(-1) : -1;
    if (j.contains("max_output_chars")) v.maxOutputChars = j["max_output_chars"].isDouble() && std::floor(j["max_output_chars"].toDouble()) == j["max_output_chars"].toDouble() ? j["max_output_chars"].toInt(-1) : -1;
    if (j.contains("max_page_bytes")) v.maxPageBytes = j["max_page_bytes"].isDouble() && std::floor(j["max_page_bytes"].toDouble()) == j["max_page_bytes"].toDouble() ? j["max_page_bytes"].toInt(-1) : -1;
    if (j.contains("max_unit_bytes")) v.maxUnitBytes = j["max_unit_bytes"].isDouble() && std::floor(j["max_unit_bytes"].toDouble()) == j["max_unit_bytes"].toDouble() ? j["max_unit_bytes"].toInt(-1) : -1;
    if (j.contains("max_presentation_bytes")) v.maxPresentationBytes = j["max_presentation_bytes"].isDouble() && std::floor(j["max_presentation_bytes"].toDouble()) == j["max_presentation_bytes"].toDouble() ? j["max_presentation_bytes"].toInt(-1) : -1;
    if (j.contains("max_manifest_bytes")) v.maxManifestBytes = j["max_manifest_bytes"].isDouble() && std::floor(j["max_manifest_bytes"].toDouble()) == j["max_manifest_bytes"].toDouble() ? j["max_manifest_bytes"].toInt(-1) : -1;
    if (j.contains("max_reduction_depth")) v.maxReductionDepth = j["max_reduction_depth"].isDouble() && std::floor(j["max_reduction_depth"].toDouble()) == j["max_reduction_depth"].toDouble() ? j["max_reduction_depth"].toInt(-1) : -1;
    if (j.contains("max_requests")) v.maxRequests = j["max_requests"].isDouble() && std::floor(j["max_requests"].toDouble()) == j["max_requests"].toDouble() ? j["max_requests"].toInt(-1) : -1;
    if (j.contains("max_retries")) v.maxRetries = j["max_retries"].isDouble() && std::floor(j["max_retries"].toDouble()) == j["max_retries"].toDouble() ? j["max_retries"].toInt(-1) : -1;
    if (j.contains("model_context_tokens")) v.modelContextTokens = j["model_context_tokens"].isDouble() && std::floor(j["model_context_tokens"].toDouble()) == j["model_context_tokens"].toDouble() ? j["model_context_tokens"].toInt(-1) : -1;
    if (j.contains("reserved_output_tokens")) v.reservedOutputTokens = j["reserved_output_tokens"].isDouble() && std::floor(j["reserved_output_tokens"].toDouble()) == j["reserved_output_tokens"].toDouble() ? j["reserved_output_tokens"].toInt(-1) : -1;
    if (j.contains("protocol_overhead_tokens")) v.protocolOverheadTokens = j["protocol_overhead_tokens"].isDouble() && std::floor(j["protocol_overhead_tokens"].toDouble()) == j["protocol_overhead_tokens"].toDouble() ? j["protocol_overhead_tokens"].toInt(-1) : -1;
    if (j.contains("fallback_input_chars")) v.fallbackInputChars = j["fallback_input_chars"].isDouble() && std::floor(j["fallback_input_chars"].toDouble()) == j["fallback_input_chars"].toDouble() ? j["fallback_input_chars"].toInt(-1) : -1;
    v.overviewOutputChars = qMin(v.overviewOutputChars, v.maxOutputChars);
    v.reductionOutputChars = qMin(v.reductionOutputChars, v.maxOutputChars);
    if (j.contains("overview_output_chars")) v.overviewOutputChars = j["overview_output_chars"].isDouble() && std::floor(j["overview_output_chars"].toDouble()) == j["overview_output_chars"].toDouble() ? j["overview_output_chars"].toInt(-1) : -1;
    if (j.contains("reduction_output_chars")) v.reductionOutputChars = j["reduction_output_chars"].isDouble() && std::floor(j["reduction_output_chars"].toDouble()) == j["reduction_output_chars"].toDouble() ? j["reduction_output_chars"].toInt(-1) : -1;
    if (j.contains("reduction_output_tokens")) v.reductionOutputTokens = j["reduction_output_tokens"].isDouble() && std::floor(j["reduction_output_tokens"].toDouble()) == j["reduction_output_tokens"].toDouble() ? j["reduction_output_tokens"].toInt(-1) : -1;
    return v;
}
QString VideoPresentationBudget::validationError() const {
    if (overviewOutputChars < 128 || overviewOutputChars > maxOutputChars ||
        reductionOutputChars < 128 || reductionOutputChars > maxOutputChars ||
        reductionOutputTokens < 32 || reductionOutputTokens > 131072) return "invalid_summary_output_budget";
    for (int n : {pageInputChars, synthesisInputChars, chapterWindowChars, maxOutputChars, fallbackInputChars})
        if (n < 256 || n > 262144) return "invalid_character_budget";
    if (maxPageBytes < 1024 || maxPageBytes > 1048576 || maxUnitBytes < maxPageBytes || maxUnitBytes > 8388608 ||
        maxPresentationBytes < 1024 || maxPresentationBytes > 16777216 || maxManifestBytes < maxPresentationBytes || maxManifestBytes > 33554432) return "invalid_payload_budget";
    if (maxReductionDepth < 1 || maxReductionDepth > 8 || maxRequests < 1 || maxRequests > 10000 || maxRetries < 0 || maxRetries > 2) return "invalid_request_budget";
    if (modelContextTokens < 0 || modelContextTokens > 2097152 || reservedOutputTokens < 1 || reservedOutputTokens > 131072 ||
        protocolOverheadTokens < 0 || protocolOverheadTokens > 65536 ||
        (modelContextTokens && qint64(reservedOutputTokens) + protocolOverheadTokens >= modelContextTokens)) return "invalid_token_budget";
    return {};
}
