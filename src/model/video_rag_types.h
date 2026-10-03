#pragma once

#include "model/video_presentation_types.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <optional>

enum class VideoContentType {
    Unknown,
    Meeting,
    Interview,
    Educational,
    Presentation,
    Tutorial,
    Documentary,
    Drama,
    News,
    Vlog
};
QString contentTypeKey(VideoContentType type);
QString contentTypeLabel(VideoContentType type);
VideoContentType contentTypeFromKey(const QString &key);

struct VideoContentProfile {
    VideoContentType primaryType = VideoContentType::Unknown;
    QStringList secondaryTypes;
    double confidence = 0;
    QString source = QStringLiteral("fallback");
    QString reasoning;
    QStringList probeEvidenceIds;
    QStringList missingSignals;
    QString classifierVersion = QStringLiteral("profile_v1");
    bool userOverride = false;
    QJsonObject toJson() const;
    static VideoContentProfile fromJson(const QJsonObject &);
};

struct AvailableCapabilities {
    bool asr = false;
    bool textVector = false;
    bool visualVector = false;
};
struct VideoRAGBuildPlan {
    QString strategyId = QStringLiteral("generic_v1");
    QString strategyVersion = QStringLiteral("1");
    QString promptVersion = QStringLiteral("units_v1");
    QString schemaVersion = QStringLiteral("facts_v1");
    QString unitUnderstandingVersion = QStringLiteral("unit_grid_carry_v2");
    QString unitSynthesisPromptVersion = QStringLiteral("unit_synthesis_v1");
    QString chapterPromptVersion = QStringLiteral("chapter_v2_summary");
    QString overviewPromptVersion = QStringLiteral("overview_v1");
    QString presentationPolicyVersion = QStringLiteral("policy_v1");
    QString presentationSchemaVersion = QStringLiteral("presentation_v1");
    VideoPresentationBudget presentationBudget;
    QString carryVersion = QStringLiteral("carry_v1_1000");
    QString gridVersion = QStringLiteral("evidence_grid_v1");
    int gridMaxEdge = 2048;
    int gridJpegQuality = 85;
    int gridMinCellShortEdge = 480;
    int gridLabelHeight = 32;
    qint64 gridMaxEncodedBytes = 0; // 0: no provider-specific byte limit configured
    QString unitKind = QStringLiteral("topic");
    bool audioFirst = false;
    bool requireSpeech = false;
    bool asrAvailable = false;
    bool textVectorAvailable = false;
    bool visualVectorAvailable = false;
    int64_t minUnitMs = 12000;
    int64_t maxUnitMs = 120000;
    int64_t frameIntervalMs = 10000;
    int framesPerUnit = 3;
    int evidencePageChars = 4000;
    int embeddingTokens = 500;
    QStringList factKinds;
    QString analysisPrompt;
    QJsonObject modelVersions;
    QString fingerprint() const;
    QJsonObject toJson() const;
    static VideoRAGBuildPlan fromJson(const QJsonObject &);
};

struct BuildOptions {
    std::optional<VideoContentType> typeOverride;
    bool forceDerivedRebuild = false;
    bool clearTypeOverride = false;
};

class VideoRagLog;
struct VideoBuildContext {
    QString videoId, filePath, fileFingerprint, buildId, rawSnapshotId, expectedActiveBuildId;
    quint64 taskGeneration = 0;
    std::shared_ptr<VideoRagLog> log;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    QString cancellationKey() const;
    bool isCancelled() const { return cancelled->load(); }
};

struct EvidenceCoverage {
    int totalPages = 0;
    int processedPages = 0;
    QStringList failedPages;
    QStringList missingCapabilities;
    QVector<int64_t> framePtsMs;
    bool complete() const {
        return totalPages > 0 && processedPages == totalPages && failedPages.isEmpty() &&
               missingCapabilities.isEmpty();
    }
    QJsonObject toJson() const;
    static EvidenceCoverage fromJson(const QJsonObject &);
};

struct UnitPageAnalysisResult {
    QString pageId;
    int pageOrdinal = 0;
    QStringList sourceIds;
    ArtifactState state = ArtifactState::Pending;
    QString title, summary, visualDescription, audioSummary, error;
    QJsonArray facts;
    bool codecValid = true;
    // Optional in legacy snapshots. Only a matching request fingerprint permits reuse.
    QString inputFingerprint;
    QJsonObject carryContext;
    QJsonObject toJson() const;
    static UnitPageAnalysisResult fromJson(const QJsonObject &);
    QString validationError(const QSet<QString> &allowedSources) const;
};

struct SemanticUnit {
    QString unitId, buildId;
    QString kind = QStringLiteral("topic");
    int64_t startMs = 0, endMs = 0;
    QString title, parentUnitId, previousUnitId, nextUnitId;
    QVector<int> shotIds;
    QStringList sourceChunkIds;
    QString visualDescription, audioSummary, fusedDescription;
    QJsonArray facts;
    QVector<UnitPageAnalysisResult> pageUnderstandings;
    ArtifactState synthesisState = ArtifactState::Pending;
    QJsonArray synthesisPoints;
    ArtifactState state = ArtifactState::Pending;
    EvidenceCoverage coverage;
    bool codecValid = true;
    bool isValid() const { return !unitId.isEmpty() && endMs > startMs && startMs >= 0; }
    QJsonObject toJson() const;
    static SemanticUnit fromJson(const QJsonObject &);
};

struct VideoBuildManifest {
    QString buildId, videoId, filePath, fileFingerprint, rawSnapshotId, specFingerprint;
    VideoContentProfile profile;
    VideoRAGBuildPlan plan;
    ArtifactState state = ArtifactState::Pending;
    int revision = 1;
    QString summary;
    VideoPresentation presentation;
    ArtifactState overviewState = ArtifactState::Pending;
    QStringList diagnostics;
    QJsonObject artifacts;
    bool codecValid = true;
    QJsonObject toJson() const;
    static VideoBuildManifest fromJson(const QJsonObject &);
};

Q_DECLARE_METATYPE(VideoContentProfile)
Q_DECLARE_METATYPE(VideoContentType)
Q_DECLARE_METATYPE(VideoBuildContext)
Q_DECLARE_METATYPE(BuildOptions)
Q_DECLARE_METATYPE(VideoRAGBuildPlan)
Q_DECLARE_METATYPE(SemanticUnit)
Q_DECLARE_METATYPE(QVector<SemanticUnit>)
Q_DECLARE_METATYPE(VideoBuildManifest)

Q_DECLARE_METATYPE(UnitPageAnalysisResult)
Q_DECLARE_METATYPE(QVector<UnitPageAnalysisResult>)
