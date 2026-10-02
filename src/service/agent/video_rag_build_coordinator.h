#pragma once
#include "model/model_reply.h"
#include "model/unit_analysis_request.h"
#include "model/video_representation.h"
#include "service/agent/video_rag_build_backend.h"
#include "service/rag/semantic_unit_builder.h"
#include "service/rag/evidence_grid_composer.h"
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <functional>

class VideoRAGStore;
class OneShotVlmChannel;
class UnitAnalysisWorkerPool;

class VideoRAGBuildCoordinator final : public QObject {
    Q_OBJECT
  public:
    using ModelRequest = std::function<void(const VideoBuildContext &, const QString &, const QString &,
                                            const QList<QImage> &, std::function<void(QString)>)>;
    using DetailedModelRequest =
        std::function<void(const VideoBuildContext &, const QString &, const QString &, const QList<QImage> &,
                           std::function<void(ModelReply)>)>;
    VideoRAGBuildCoordinator(VideoRAGBuildBackend *, VideoRAGStore *, QObject *parent = nullptr);
    VideoRAGBuildCoordinator(VideoRAGBuildBackend *, VideoRAGStore *, OneShotVlmChannel *,
                             QObject *parent = nullptr);
    ~VideoRAGBuildCoordinator() override;
    void start(const QString &path, const BuildOptions &options = {});
    void cancel();
    void changeType(const QString &path, VideoContentType);
    void setModelRequest(ModelRequest request) {
        m_modelRequest = [request](const auto &context, const auto &system, const auto &text,
                                   const auto &frames, std::function<void(ModelReply)> done) {
            request(context, system, text, frames, [done](QString content) { done({content, {}}); });
        };
    }
    void setDetailedModelRequest(DetailedModelRequest request) { m_modelRequest = std::move(request); }
    using UnitModelRequest = std::function<void(const UnitAnalysisRequest&, const QString&, const QString&,
                                               const QList<QImage>&, std::function<void(ModelReply)>)>;
    void setUnitModelRequest(UnitModelRequest request) { m_unitModelRequest = std::move(request); }
    void setUnitWorkerPool(UnitAnalysisWorkerPool*);
    void setUnitConcurrency(int value) { m_unitConcurrency = qBound(1, value, 3); }
    void setUnitGridConfig(const EvidenceGridConfig& config) { m_gridConfig = config; }
    void setUnitRequestCancellation(std::function<void(const QString&)> fn) { m_cancelUnitRequest = std::move(fn); }
    void setUnitWatchdogTimeoutProvider(std::function<int(int)> fn) { m_unitWatchdogTimeout = std::move(fn); }
    void setModelSignatureProvider(std::function<QString()> fn) { m_modelSignature = std::move(fn); }
    bool isRunning() const { return bool(m_job); }
  signals:
    void progress(int, const QString &);
    void published(const VideoRepresentation &);
    void finished(const VideoBuildManifest &);
    void profileReady(const QString &filePath, const VideoContentProfile &);
    void buildFailed(const QString &);

  private:
    struct Job;
    struct UnitAnalysisTask;
    bool current(const std::shared_ptr<Job> &) const;
    void classify(const std::shared_ptr<Job> &, int attempt = 0);
    void route(const std::shared_ptr<Job> &);
    void segment(const std::shared_ptr<Job> &);
    void correctNext(const std::shared_ptr<Job> &, int attempt = 0);
    void prepareUnitEvidence(const std::shared_ptr<Job> &);
    void beginUnitAnalysis(const std::shared_ptr<Job>&);
    void scheduleUnits(const std::shared_ptr<Job>&);
    void analyzeUnitPage(const std::shared_ptr<Job>&, const std::shared_ptr<UnitAnalysisTask>&);
    void finishUnitAnalysis(const std::shared_ptr<Job>&);
    void stopUnitRequests(const std::shared_ptr<Job>&);
    void writeUnitDiagnostic(const std::shared_ptr<Job>&, QJsonObject);
    void requestUnit(const std::shared_ptr<Job>&, const std::shared_ptr<UnitAnalysisTask>&,
                     const QString&, const QString&, const QList<QImage>&, std::function<void(ModelReply)>);
    void summarizeNext(const std::shared_ptr<Job> &, int attempt = 0);
    void publish(const std::shared_ptr<Job> &);
    void fail(const std::shared_ptr<Job> &, const QString &);
    void request(const std::shared_ptr<Job> &, const QString &, const QString &, const QList<QImage> &,
                 std::function<void(ModelReply)>);
    QString modelSignature() const {
        return m_modelSignature ? m_modelSignature() : QStringLiteral("unavailable");
    }
    VideoRAGBuildBackend *m_indexer;
    VideoRAGStore *m_store;
    DetailedModelRequest m_modelRequest;
    UnitModelRequest m_unitModelRequest;
    int m_unitConcurrency = 3;
    int m_poolCapacity = 3;
    EvidenceGridConfig m_gridConfig;
    std::function<int(int)> m_unitWatchdogTimeout;
    std::function<void(const QString&)> m_cancelUnitRequest, m_cancelUnitBuild;
    std::function<void(int, const QString&)> m_releaseUnitWorker;
    std::function<QString()> m_modelSignature;
    std::function<int()> m_modelWatchdogTimeout;
    std::function<void(const QString &)> m_cancelModel;
    quint64 m_generation = 0;
    std::shared_ptr<Job> m_job;
    QThreadPool m_imagePool;
};
