#pragma once
#include "model/retrieval_result.h"
#include "model/video_representation.h"
#include <functional>

struct VideoEvidenceExtractionResult {
    VideoRepresentation representation;
    QVector<VideoChunk> chunks;
    QStringList diagnostics;
    ArtifactState state = ArtifactState::Ready;
};

// Execution seam: strategies never own workers, databases, or model clients.
class VideoRAGBuildBackend {
  public:
    virtual ~VideoRAGBuildBackend() = default;
    virtual AvailableCapabilities capabilities() const = 0;
    virtual QJsonObject modelVersions() const = 0;
    virtual void extract(const VideoBuildContext &, const VideoRAGBuildPlan &, bool probe,
                         std::function<void(VideoEvidenceExtractionResult)>) = 0;
    virtual void encodeChunks(const VideoBuildContext &, QVector<VideoChunk>,
                              std::function<void(QVector<VideoChunk>)>) = 0;
    virtual void setPublished(const VideoRepresentation &) = 0;
    virtual void extractUnitFrames(const VideoBuildContext &, const QVector<SemanticUnit> &,
                                   const VideoRAGBuildPlan &,
                                   std::function<void(VideoEvidenceExtractionResult)> done) {
        done({});
    }
};
