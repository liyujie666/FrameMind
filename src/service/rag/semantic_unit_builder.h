#pragma once
#include "model/retrieval_result.h"
#include "model/video_representation.h"

struct UnitEvidencePage {
    QString pageId, unitId;
    QJsonArray evidence;
    QStringList sourceIds, framePaths;
    QVector<int64_t> framePtsMs;
    QJsonObject toJson() const;
};

class SemanticUnitBuilder {
  public:
    static QVector<SemanticUnit> candidates(const VideoRepresentation &, const QVector<VideoChunk> &,
                                            const VideoRAGBuildPlan &, const QString &buildId);
    static bool correct(const QJsonObject &, const QVector<SemanticUnit> &local, const QVector<VideoChunk> &,
                        const VideoRAGBuildPlan &, QVector<SemanticUnit> *output, QString *error);
    static void attachSources(QVector<SemanticUnit> &, const VideoRepresentation &,
                              const QVector<VideoChunk> &);
    static QVector<UnitEvidencePage> pages(const SemanticUnit &, const QVector<VideoChunk> &,
                                           const VideoRAGBuildPlan &);
    static QJsonArray validatedFacts(const QJsonArray &, const UnitEvidencePage &, const VideoRAGBuildPlan &,
                                     bool *valid, QString *error = nullptr);
    static QJsonObject parseObject(const QString &, QString *error = nullptr);
};
