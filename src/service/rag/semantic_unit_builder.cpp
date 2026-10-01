#include "service/rag/semantic_unit_builder.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
QSet<QString> contextTerms(const QString &text) {
    QSet<QString> terms;
    const auto clean = text.toLower().simplified();
    static QRegularExpression words(QStringLiteral("[a-z0-9]+"));
    auto it = words.globalMatch(clean);
    while (it.hasNext())
        terms.insert(it.next().captured());
    for (int i = 0; i + 1 < clean.size(); ++i)
        if (clean[i].unicode() >= 0x2e80 && clean[i + 1].unicode() >= 0x2e80)
            terms.insert(clean.mid(i, 2));
    return terms;
}
} // namespace

QJsonObject UnitEvidencePage::toJson() const {
    return {{"page_id", pageId}, {"unit_id", unitId}, {"core_evidence", evidence}};
}
QJsonObject SemanticUnitBuilder::parseObject(const QString &reply) {
    const int start = reply.indexOf('{'), end = reply.lastIndexOf('}');
    if (start < 0 || end < start)
        return {};
    return QJsonDocument::fromJson(reply.mid(start, end - start + 1).toUtf8()).object();
}

QVector<SemanticUnit> SemanticUnitBuilder::candidates(const VideoRepresentation &r,
                                                      const QVector<VideoChunk> &chunks,
                                                      const VideoRAGBuildPlan &plan, const QString &build) {
    QVector<int64_t> boundaries{0};
    QSet<int64_t> natural{0, r.metadata.durationMs};
    QVector<VideoChunk> speech;
    for (const auto &c : chunks)
        if (c.chunkType == VideoChunk::SpeechSegment)
            speech << c;
    std::sort(speech.begin(), speech.end(),
              [](const auto &a, const auto &b) { return a.startMs < b.startMs; });
    const QRegularExpression topic(
        QStringLiteral(
            "^(那么接下来|接下来|下面我们|另一个问题|第二|第三|最后|首先|然后|下一步|now |next |finally )"),
        QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < speech.size(); ++i) {
        const auto &c = speech[i];
        const auto span = c.startMs - boundaries.last();
        bool newQuestion = plan.unitKind == "qa_pair" &&
                           (c.textContent.contains('?') || c.textContent.contains(QStringLiteral("？")));
        bool pause = i > 0 && c.startMs - speech[i - 1].endMs >= 2000;
        bool transition = topic.match(c.textContent.trimmed()).hasMatch();
        if (i > 0 && !newQuestion && plan.unitKind != "qa_pair") {
            QString previous;
            for (int k = qMax(0, i - 3); k < i; ++k)
                previous += speech[k].textContent;
            const auto left = contextTerms(previous), right = contextTerms(c.textContent);
            int shared = 0;
            for (const auto &term : right)
                if (left.contains(term))
                    ++shared;
            if (left.size() > 8 && right.size() > 8 && double(shared) / right.size() < 0.08)
                transition = true;
        }
        // Dialogue deliberately ignores shot boundaries: a Q/A can span shots.
        if (span >= plan.maxUnitMs || (span >= plan.minUnitMs && (transition || newQuestion || pause))) {
            boundaries << c.startMs;
            if (transition || newQuestion || pause)
                natural.insert(c.startMs);
        }
    }
    if (!plan.audioFirst || speech.isEmpty()) {
        for (const auto &shot : r.scenes)
            if (shot.startMs > 0) {
                boundaries << shot.startMs;
                natural.insert(shot.startMs);
            }
    } else if (plan.unitKind == "concept" || plan.unitKind == "step") {
        // Slides/state changes propose candidates; the model may merge them again.
        for (const auto &shot : r.scenes)
            if (shot.startMs > 0)
                boundaries << shot.startMs;
    }
    boundaries << r.metadata.durationMs;
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    QVector<int64_t> bounded{0};
    for (auto endpoint : boundaries) {
        if (endpoint <= bounded.last())
            continue;
        while (endpoint - bounded.last() > plan.maxUnitMs)
            bounded << bounded.last() + plan.maxUnitMs;
        if (endpoint == r.metadata.durationMs || endpoint - bounded.last() >= plan.minUnitMs)
            bounded << endpoint;
    }
    QVector<SemanticUnit> out;
    for (int i = 0; i + 1 < bounded.size(); ++i) {
        SemanticUnit u;
        u.unitId = build + ":unit:" + QString::number(i);
        u.buildId = build;
        u.kind = plan.unitKind;
        u.startMs = bounded[i];
        u.endMs = bounded[i + 1];
        u.title = QStringLiteral("%1 %2").arg(plan.unitKind).arg(i + 1);
        out << u;
    }
    if (plan.audioFirst && !speech.isEmpty()) {
        QVector<int64_t> topics(natural.begin(), natural.end());
        std::sort(topics.begin(), topics.end());
        for (int i = 0; i + 1 < topics.size(); ++i)
            if (topics[i + 1] - topics[i] > plan.maxUnitMs)
                for (auto &u : out)
                    if (u.startMs >= topics[i] && u.endMs <= topics[i + 1])
                        u.parentUnitId = build + ":parent:" + QString::number(topics[i]);
    }
    attachSources(out, r, chunks);
    return out;
}

void SemanticUnitBuilder::attachSources(QVector<SemanticUnit> &units, const VideoRepresentation &r,
                                        const QVector<VideoChunk> &chunks) {
    for (int i = 0; i < units.size(); ++i) {
        auto &u = units[i];
        u.shotIds.clear();
        u.sourceChunkIds.clear();
        u.previousUnitId = i > 0 ? units[i - 1].unitId : QString();
        u.nextUnitId = i + 1 < units.size() ? units[i + 1].unitId : QString();
        for (const auto &shot : r.scenes)
            if (shot.startMs < u.endMs && shot.endMs > u.startMs)
                u.shotIds << shot.id;
        for (const auto &c : chunks)
            if (c.startMs < u.endMs && c.endMs > u.startMs)
                u.sourceChunkIds << c.chunkId;
    }
}

bool SemanticUnitBuilder::correct(const QJsonObject &result, const QVector<SemanticUnit> &local,
                                  const QVector<VideoChunk> &raw, const VideoRAGBuildPlan &plan,
                                  QVector<SemanticUnit> *output, QString *error) {
    auto reject = [&](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (local.isEmpty() || !result["units"].isArray())
        return reject("missing_units");
    QSet<int64_t> endpoints;
    QHash<QString, VideoChunk> sources;
    for (const auto &u : local) {
        endpoints.insert(u.startMs);
        endpoints.insert(u.endMs);
    }
    for (const auto &c : raw) {
        sources.insert(c.chunkId, c);
        endpoints.insert(c.startMs);
        endpoints.insert(c.endMs);
    }
    const int64_t from = local.first().startMs, to = local.last().endMs;
    int64_t cursor = from;
    QVector<SemanticUnit> accepted;
    for (auto value : result["units"].toArray()) {
        const auto o = value.toObject();
        SemanticUnit u;
        u.buildId = local.first().buildId;
        u.kind = plan.unitKind;
        u.startMs = o["start_ms"].toVariant().toLongLong();
        u.endMs = o["end_ms"].toVariant().toLongLong();
        u.title = o["title"].toString();
        u.unitId = u.buildId + ":unit:" + QString::number(u.startMs) + ":" + QString::number(u.endMs);
        if (!u.isValid() || u.startMs != cursor || u.endMs > to || u.endMs - u.startMs > plan.maxUnitMs ||
            !endpoints.contains(u.startMs) || !endpoints.contains(u.endMs))
            return reject("illegal_boundary_or_coverage");
        for (auto id : o["source_chunk_ids"].toArray()) {
            const QString source = id.toString();
            if (!sources.contains(source))
                return reject("fabricated_source");
            const auto &c = sources[source];
            if (c.startMs >= u.endMs || c.endMs <= u.startMs)
                return reject("source_outside_unit");
            u.sourceChunkIds << source;
        }
        QStringList required;
        for (const auto &c : raw)
            if (c.startMs < u.endMs && c.endMs > u.startMs)
                required << c.chunkId;
        if (QSet<QString>(required.begin(), required.end()) !=
            QSet<QString>(u.sourceChunkIds.begin(), u.sourceChunkIds.end()))
            return reject("missing_source");
        cursor = u.endMs;
        accepted << u;
    }
    if (cursor != to || accepted.isEmpty())
        return reject("incomplete_coverage");
    for (auto &u : accepted) {
        QString parent;
        bool first = true, same = true;
        for (const auto &candidate : local)
            if (candidate.startMs < u.endMs && candidate.endMs > u.startMs) {
                if (first) {
                    parent = candidate.parentUnitId;
                    first = false;
                } else if (parent != candidate.parentUnitId)
                    same = false;
            }
        if (same)
            u.parentUnitId = parent;
    }
    *output = accepted;
    return true;
}

QVector<UnitEvidencePage> SemanticUnitBuilder::pages(const SemanticUnit &u, const QVector<VideoChunk> &raw,
                                                     const VideoRAGBuildPlan &plan) {
    QVector<UnitEvidencePage> out;
    UnitEvidencePage current;
    current.unitId = u.unitId;
    int chars = 0;
    auto flush = [&] {
        if (current.evidence.isEmpty())
            return;
        current.pageId = u.unitId + ":page:" + QString::number(out.size());
        out << current;
        current = {};
        current.unitId = u.unitId;
        chars = 0;
    };
    const int budget = qMax(256, plan.evidencePageChars - 512);
    for (const auto &c : raw)
        if (u.sourceChunkIds.contains(c.chunkId)) {
            if (c.chunkType == VideoChunk::FrameDesc) {
                const QJsonObject entry{
                    {"source_id", c.chunkId}, {"pts_ms", qint64(c.startMs)}, {"modality", "frame"}};
                const int entryChars =
                    QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Compact)).size();
                if (current.framePaths.size() >= plan.framesPerUnit || chars + entryChars > budget)
                    flush();
                current.framePaths << c.keyframePath;
                current.framePtsMs << c.startMs;
                current.sourceIds << c.chunkId;
                current.evidence.append(entry);
                chars += entryChars;
            } else {
                for (int offset = 0; offset < c.textContent.size();) {
                    int length = qMin(int(c.textContent.size()) - offset, qMax(64, budget - 384));
                    if (offset + length < c.textContent.size() &&
                        c.textContent[offset + length - 1].isHighSurrogate())
                        --length;
                    const auto text = c.textContent.mid(offset, length);
                    const QJsonObject entry{{"source_id", c.chunkId},
                                            {"start_ms", qint64(c.startMs)},
                                            {"end_ms", qint64(c.endMs)},
                                            {"text_offset", offset},
                                            {"text", text},
                                            {"modality", "speech"},
                                            {"speaker", "unknown"}};
                    const int entryChars =
                        QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Compact)).size();
                    if (chars + entryChars > budget)
                        flush();
                    current.sourceIds << c.chunkId;
                    current.evidence.append(entry);
                    chars += entryChars;
                    offset += length;
                }
            }
        }
    flush();
    return out;
}

QJsonArray SemanticUnitBuilder::validatedFacts(const QJsonArray &facts, const UnitEvidencePage &page,
                                               const VideoRAGBuildPlan &plan, bool *valid) {
    *valid = true;
    QJsonArray accepted;
    for (auto v : facts) {
        auto fact = v.toObject();
        const auto refs = fact["source_chunk_ids"].toArray();
        if (fact["text"].toString().trimmed().isEmpty() ||
            !plan.factKinds.contains(fact["kind"].toString()) || refs.isEmpty()) {
            *valid = false;
            return {};
        }
        for (auto id : refs)
            if (!page.sourceIds.contains(id.toString())) {
                *valid = false;
                return {};
            }
        if (fact["kind"] == "operation" && page.framePaths.size() < 2) {
            fact["observation_status"] = "narration_only";
        }
        QString quoted;
        for (auto evidence : page.evidence)
            quoted += evidence.toObject()["text"].toString();
        for (const auto &field : {QStringLiteral("owner"), QStringLiteral("deadline")})
            if (!fact[field].toString().isEmpty() && !quoted.contains(fact[field].toString()))
                fact[field] = QJsonValue::Null;
        fact["speaker"] = "unknown";
        accepted.append(fact);
    }
    return accepted;
}
