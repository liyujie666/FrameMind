#pragma once
#include "service/rag/video_presentation_policy_registry.h"
#include "model/retrieval_result.h"
#include <functional>
#include <optional>
#include <QHash>

struct VideoPresentationStageDefinition {
    QString requestStage, metricStage;
    int progressStart = 0, progressEnd = 0;
};
struct VideoPresentationStageMetrics {
    int calls = 0, retries = 0, failures = 0;
    qint64 elapsedMs = 0;
    QJsonObject toJson() const;
};
// A per-build ledger. The coordinator reserves every attempt (including existing
// probe/page calls) before submission and records terminal results exactly once.
class VideoPresentationRequestLedger {
public:
    explicit VideoPresentationRequestLedger(VideoPresentationBudget budget = {});
    QString reserve(const QString &stage, const QString &operationId, int attempt = 0);
    void finish(const QString &operationId, int attempt, qint64 elapsedMs, bool failed);
    int calls() const { return m_calls; }
    QJsonObject artifacts(const QJsonObject &existing = {}) const;
private:
    VideoPresentationBudget m_budget;
    int m_calls = 0;
    QHash<QString, VideoPresentationStageMetrics> m_metrics;
    struct Attempt { QString stage; bool finished = false; };
    QHash<QString, QVector<Attempt>> m_attempts;
};
struct VideoPresentationPreparedRequest {
    QJsonObject input;
    QString prompt, error;
    QHash<QString, QString> originalIds; // request-local alias -> persisted ID
    QHash<QString, QStringList> sourceNodes; // short evidence node -> original sources; never sent
    int outputChars = 0, outputTokens = 0;
    int inputChars = 0;
    qint64 inputLimitChars = 0;
    std::optional<qint64> inputTokens;
    QJsonObject restoreIds(const QJsonObject &, QString *error) const;
    bool isValid() const { return error.isEmpty() && !input.isEmpty(); }
};
class VideoPresentationBuilder {
public:
    // Reading length for each generated chapter, separate from whole-request budgets.
    static constexpr int ChapterSummaryMaxChars = 360;
    // Counter must count the complete text given to it using the selected
    // generation model tokenizer. std::nullopt falls back conservatively.
    using TokenCounter = std::function<std::optional<qint64>(const QString &)>;
    static QVector<VideoPresentationStageDefinition> stages();
    static VideoPresentationPreparedRequest prepare(const QString &stage, const QJsonObject &payload,
        const VideoRAGBuildPlan &, const VideoPresentationPolicy &, TokenCounter counter = {});
    static QString inputBudgetError(const QString &stage, const QString &completeInput,
        const VideoPresentationBudget &, TokenCounter counter = {});
    static QString outputBudgetError(const QJsonObject &, const VideoPresentationPreparedRequest &,
        TokenCounter counter = {}, const QString &wireReply = {});
    static QVector<SemanticUnit> sectionEvidenceParts(const SemanticUnit &);
    static QString reductionBudgetError(int currentItems, int nextItems, int depth, const VideoPresentationBudget &,
        qint64 currentBytes = 0, qint64 nextBytes = 0);
    static QString reductionProgressError(const VideoPresentationPreparedRequest& before,
        const VideoPresentationPreparedRequest& after, int depth, const VideoPresentationBudget&);
    static QJsonArray atomicEvidenceItems(const QJsonArray &);
    static QStringList chapterIncompleteReasons(const QVector<SemanticUnit> &);
    static QJsonObject parseReply(const QString &, const VideoPresentationBudget &, QString *error);
    static QJsonArray synthesisItems(const SemanticUnit &, const QVector<VideoChunk> &);
    static QJsonObject synthesisPayload(const SemanticUnit &, const QJsonArray &);
    static QJsonObject reductionResult(const QJsonObject &, const QJsonArray &, const SemanticUnit &, QString *error);
    static QString synthesisValidationError(const QJsonObject &, const SemanticUnit &);
    static QString unitValidationError(const SemanticUnit &, const VideoPresentationBudget &);
    static bool hasChapterEvidence(const SemanticUnit &);
    static bool canJoinChapter(const SemanticUnit &, const SemanticUnit &);
    static QJsonObject chapterLeafInput(const SemanticUnit &, bool includeFacts = false);
    static QVector<SemanticUnit> chapterUnits(const VideoChapter &, const QVector<SemanticUnit> &);
    static QJsonObject chapterPlanPayload(const QVector<SemanticUnit> &);
    static QJsonObject chapterRefinePayload(const QVector<VideoChapter> &, int offset, int count,
        const QVector<SemanticUnit> &);
    static VideoChapter chapterForUnits(const QVector<SemanticUnit> &, const QString &title, bool provisional = false);
    static QVector<VideoChapter> parseChapterPlan(const QJsonObject &, const QVector<SemanticUnit> &, QString *error);
    static QString chapterTextError(const QString &, const QVector<SemanticUnit> &);
    static QString chapterNarrativeError(const QJsonObject &, const QVector<VideoChapter> &, const QVector<SemanticUnit> &);
    static void linkChapters(QVector<VideoChapter> &, const QString &buildId);
    static QString chaptersValidationError(const VideoPresentation &, const QVector<SemanticUnit> &,
        const VideoPresentationValidationContext &, const VideoPresentationBudget &);
    static QJsonArray overviewOutline(const QVector<VideoChapter> &);
    static QJsonArray overviewItems(const QVector<VideoChapter> &, const QVector<SemanticUnit> &,
        bool useSyntheses = true);
    static QJsonObject overviewPayload(const QJsonArray &outline, const QJsonArray &items);
    static QJsonObject overviewReductionResult(const QJsonObject &, const QJsonArray &,
        const QVector<SemanticUnit> &, QString *error);
    static QString overviewTextError(const QString &, const QVector<SemanticUnit> & = {});
    static QString overviewReplyError(const QJsonObject &, const QVector<SemanticUnit> &);
    static QString overviewValidationError(const VideoBuildManifest &, const QVector<SemanticUnit> & = {});
    static QString policySelectionError(const QJsonObject &, const QVector<SemanticUnit> &);
    static QStringList understoodSources(const SemanticUnit &);
    static QVector<VideoReviewAnchor> reviewAnchors(const QString &buildId, qint64 duration,
        const QVector<SemanticUnit> &, const QVector<VideoChunk> &);
    static QJsonObject sectionPayload(bool primary, const VideoPresentation &, const QVector<SemanticUnit> &,
        const VideoPresentationPolicy &, const QString &buildId,
        const QHash<QString, QJsonArray> &compactEvidence = {});
    static VideoSummarySection parseSection(const QJsonObject &, bool primary, const VideoPresentation &,
        const QVector<SemanticUnit> &, const QVector<VideoChunk> &, const VideoPresentationPolicy &,
        const QString &buildId, QString *error, QStringList *diagnostics = nullptr,
        const VideoSummarySection *completePrimary = nullptr);
    static void retainSelectedAnchors(VideoPresentation &, const QVector<VideoContentEntry> &,
        const QVector<VideoReviewAnchor> &candidates);
    static QVector<VideoExploreQuestion> selectExploreQuestions(const QVector<VideoExploreQuestion> &, int limit);
    static QString sectionsValidationError(const VideoPresentation &, const QVector<SemanticUnit> &,
        const QVector<VideoChunk> &, const QString &buildId, qint64 duration, const VideoPresentationBudget &);
    static QString presentationValidationError(const VideoPresentation &, const VideoPresentationValidationContext &,
        const VideoPresentationBudget &);
    static QVector<VideoChunk> derivedChunks(const VideoBuildManifest &, const QVector<SemanticUnit> &);
    static QString normalizeAnchors(VideoBuildManifest &, const QVector<SemanticUnit> &,
        const QVector<VideoChunk> &, qint64 duration);
    static ArtifactState overallState(const VideoBuildManifest &, const QVector<SemanticUnit> &);
    static QString buildValidationError(const VideoBuildManifest &, const QVector<SemanticUnit> &,
        const QVector<VideoChunk> &raw, qint64 duration, const QVector<VideoChunk> &derived);
    static bool reusable(const VideoBuildManifest &, const QVector<SemanticUnit> &);
    static QVector<SemanticUnit> orderedLeaves(const QVector<SemanticUnit> &);
};
