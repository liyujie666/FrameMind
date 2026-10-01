#pragma once
#include <QObject>
#include <functional>
#include <QImage>
#include "model/video_representation.h"
#include "service/rag/semantic_unit_builder.h"

class VideoIndexer;
class VideoRAGStore;
class OneShotVlmChannel;

class VideoRAGBuildCoordinator final : public QObject {
    Q_OBJECT
public:
    using ModelRequest=std::function<void(const VideoBuildContext&,const QString&,const QString&,const QList<QImage>&,std::function<void(QString)>)>;
    VideoRAGBuildCoordinator(VideoIndexer*,VideoRAGStore*,OneShotVlmChannel*,QObject* parent=nullptr);
    ~VideoRAGBuildCoordinator() override;
    void start(const QString& path,const BuildOptions& options={});
    void cancel();
    void changeType(const QString& path,VideoContentType);
    void setModelRequest(ModelRequest request) {m_modelRequest=std::move(request);}
    bool isRunning() const {return bool(m_job);}
signals:
    void progress(int,const QString&);
    void published(const VideoRepresentation&);
    void finished(const VideoBuildManifest&);
    void profileReady(const QString& filePath,const VideoContentProfile&);
    void buildFailed(const QString&);
private:
    struct Job;
    bool current(const std::shared_ptr<Job>&) const;
    void classify(const std::shared_ptr<Job>&,int attempt=0);
    void route(const std::shared_ptr<Job>&);
    void segment(const std::shared_ptr<Job>&);
    void correctNext(const std::shared_ptr<Job>&,int attempt=0);
    void analyzeNext(const std::shared_ptr<Job>&,int attempt=0);
    void summarizeNext(const std::shared_ptr<Job>&,int attempt=0);
    void publish(const std::shared_ptr<Job>&);
    void fail(const std::shared_ptr<Job>&,const QString&);
    void request(const std::shared_ptr<Job>&,const QString&,const QString&,const QList<QImage>&,std::function<void(QString)>);
    VideoIndexer* m_indexer;
    VideoRAGStore* m_store;
    OneShotVlmChannel* m_channel;
    ModelRequest m_modelRequest;
    quint64 m_generation=0;
    std::shared_ptr<Job> m_job;
};
