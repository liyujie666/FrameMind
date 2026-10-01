#pragma once
#include "model/video_representation.h"
#include "service/agent/video_rag_build_backend.h"
#include "service/rag/semantic_unit_builder.h"
#include <QImage>
#include <QObject>
#include <functional>

class VideoRAGStore;
class OneShotVlmChannel;

class VideoRAGBuildCoordinator final : public QObject {
    Q_OBJECT
  public:
    using ModelRequest = std::function<void(const VideoBuildContext &, const QString &, const QString &,
                                            const QList<QImage> &, std::function<void(QString)>)>;
    VideoRAGBuildCoordinator(VideoRAGBuildBackend *, VideoRAGStore *, QObject *parent = nullptr);
    VideoRAGBuildCoordinator(VideoRAGBuildBackend *, VideoRAGStore *, OneShotVlmChannel *,
                             QObject *parent = nullptr);
    ~VideoRAGBuildCoordinator() override;
    void start(const QString &path, const BuildOptions &options = {});
    void cancel();
    void changeType(const QString &path, VideoContentType);
    void setModelRequest(ModelRequest request) { m_modelRequest = std::move(request); }
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
    bool current(const std::shared_ptr<Job> &) const;
    void classify(const std::shared_ptr<Job> &, int attempt = 0);
    void route(const std::shared_ptr<Job> &);
    void segment(const std::shared_ptr<Job> &);
    void correctNext(const std::shared_ptr<Job> &, int attempt = 0);
    void prepareUnitEvidence(const std::shared_ptr<Job> &);
    void analyzeNext(const std::shared_ptr<Job> &, int attempt = 0);
    void summarizeNext(const std::shared_ptr<Job> &, int attempt = 0);
    void publish(const std::shared_ptr<Job> &);
    void fail(const std::shared_ptr<Job> &, const QString &);
    void request(const std::shared_ptr<Job> &, const QString &, const QString &, const QList<QImage> &,
                 std::function<void(QString)>);
    QString modelSignature() const {
        return m_modelSignature ? m_modelSignature() : QStringLiteral("unavailable");
    }
    VideoRAGBuildBackend *m_indexer;
    VideoRAGStore *m_store;
    ModelRequest m_modelRequest;
    std::function<QString()> m_modelSignature;
    std::function<void(const QString &)> m_cancelModel;
    quint64 m_generation = 0;
    std::shared_ptr<Job> m_job;
};
