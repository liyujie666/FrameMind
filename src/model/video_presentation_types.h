#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QStringList>
#include <QVector>

enum class ArtifactState { Pending, Running, Ready, Partial, Failed, Skipped, Cancelled };
QString artifactStateKey(ArtifactState state);
ArtifactState artifactStateFromKey(const QString &key);
Q_DECLARE_METATYPE(ArtifactState)

// Validation uses the current raw snapshot and build's unit IDs, never UI state.
struct VideoPresentationValidationContext {
    QSet<QString> sourceIds, unitIds;
    qint64 durationMs = 0;
};
struct VideoContentPoint {
    QString role, text;
    QStringList sourceChunkIds;
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoContentPoint fromJson(const QJsonObject &);
    QString validationError(const VideoPresentationValidationContext &) const;
};
struct VideoChapter {
    QString chapterId;
    qint64 startMs = 0, endMs = 0;
    QString title, description;
    QStringList unitIds, sourceChunkIds;
    QStringList incompleteReasons;
    QString previousChapterId, nextChapterId;
    ArtifactState state = ArtifactState::Pending;
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoChapter fromJson(const QJsonObject &);
    QString validationError(const VideoPresentationValidationContext &) const;
};
struct VideoReviewAnchor {
    QString anchorId;
    qint64 startMs = 0, endMs = 0;
    QStringList sourceChunkIds, unitIds;
    QString precision = QStringLiteral("evidence");
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoReviewAnchor fromJson(const QJsonObject &);
    QString validationError(const VideoPresentationValidationContext &) const;
};
struct VideoContentEntry {
    QString entryId, kind, title, body;
    QVector<VideoContentPoint> points;
    QStringList unitIds, sourceChunkIds;
    QString anchorId;
    int anchorPointIndex = -1; // selected main point; -1 when no review anchor
    QJsonObject attributes;
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoContentEntry fromJson(const QJsonObject &);
    QString validationError(const VideoPresentationValidationContext &) const;
};
struct VideoExploreQuestion {
    QString questionId, text, intent;
    QStringList relatedEntryIds, unitIds, sourceChunkIds;
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoExploreQuestion fromJson(const QJsonObject &);
    QString validationError(const VideoPresentationValidationContext &) const;
};
struct VideoSummarySection {
    QString kind, title;
    ArtifactState state = ArtifactState::Pending;
    QVector<VideoContentEntry> entries;
    QVector<VideoExploreQuestion> questions;
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoSummarySection fromJson(const QJsonObject &);
};
struct VideoPresentation {
    QString schemaVersion = QStringLiteral("presentation_v1");
    QString policyId, policyVersion;
    QJsonObject policySelection;
    ArtifactState chaptersState = ArtifactState::Pending;
    QVector<VideoChapter> chapters;
    QVector<VideoReviewAnchor> anchors;
    VideoSummarySection primarySection, secondarySection;
    QStringList diagnostics;
    // A malformed/unknown persisted schema remains readable, but cannot be complete.
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoPresentation fromJson(const QJsonObject &);
    QString validationError(const VideoPresentationValidationContext &) const;
    bool hasCompletedSections() const;
};

// Limits are serialized into the plan fingerprint. Characters are a separate
// safety guard, not a token count. Token allowance reserves output and overhead.
struct VideoPresentationBudget {
    int pageInputChars = 12000;
    int synthesisInputChars = 24000;
    int chapterWindowChars = 32000;
    int maxOutputChars = 16000;
    int maxPageBytes = 65536;
    int maxUnitBytes = 1048576;
    int maxPresentationBytes = 2097152;
    int maxManifestBytes = 3145728;
    int maxReductionDepth = 4;
    int maxRequests = 4096; // whole build, attempts/retries included
    int maxRetries = 1;
    int modelContextTokens = 0; // unknown until supplied for the generation model
    int reservedOutputTokens = 4096;
    int protocolOverheadTokens = 1024;
    int fallbackInputChars = 8000; // no token counter/context knowledge
    int overviewOutputChars = 2400;
    int reductionOutputChars = 1200;
    int reductionOutputTokens = 768;
    QString validationError() const;
    QJsonObject toJson() const;
    static VideoPresentationBudget fromJson(const QJsonObject &);
};
Q_DECLARE_METATYPE(VideoContentPoint)
Q_DECLARE_METATYPE(VideoChapter)
Q_DECLARE_METATYPE(VideoReviewAnchor)
Q_DECLARE_METATYPE(VideoContentEntry)
Q_DECLARE_METATYPE(VideoExploreQuestion)
Q_DECLARE_METATYPE(VideoSummarySection)
Q_DECLARE_METATYPE(VideoPresentation)
Q_DECLARE_METATYPE(VideoPresentationBudget)
Q_DECLARE_METATYPE(QVector<VideoChapter>)
Q_DECLARE_METATYPE(QVector<VideoContentEntry>)
Q_DECLARE_METATYPE(QVector<VideoExploreQuestion>)
