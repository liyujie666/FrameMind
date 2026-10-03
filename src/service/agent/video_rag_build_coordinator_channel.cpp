#include "service/agent/one_shot_vlm_channel.h"
#include "service/agent/video_rag_build_coordinator.h"
#include <QPointer>
#include "model/build_request_policy.h"
#include "service/agent/unit_analysis_worker_pool.h"
VideoRAGBuildCoordinator::VideoRAGBuildCoordinator(VideoRAGBuildBackend *backend, VideoRAGStore *store,
                                                   OneShotVlmChannel *channel, QObject *parent)
    : VideoRAGBuildCoordinator(backend, store, parent) {
    QPointer<OneShotVlmChannel> guard(channel);
    // The legacy constructor remains safe without a pool: one worker, explicit request cancellation.
    m_poolCapacity = 1;
    m_unitWatchdogTimeout = [guard](int) { return guard ? guard->watchdogTimeoutMs() : 1; };
    m_cancelUnitRequest = [guard](const QString& id) { if (guard) guard->cancelRequest(id); };
    m_unitModelRequest = [guard](const UnitAnalysisRequest& r, const QString& system, const QString& text,
                               const QList<QImage>& images, std::function<void(ModelReply)> done) {
        if (guard) guard->enqueueRequest(r.requestId, system, text, images,
            OneShotVlmChannel::Priority::Background, r.context.cancellationKey(), std::move(done), r.imageOptions, r.maxOutputTokens);
        else done({{}, QStringLiteral("模型通道未初始化")});
    };
    m_modelSignature = [guard] { return guard ? guard->modelSignature() : QStringLiteral("unavailable"); };
    m_modelWatchdogTimeout = [guard] {
        return guard ? guard->watchdogTimeoutMs()
                     : BuildRequestPolicy::TotalTimeoutMs + BuildRequestPolicy::QueueGraceMs;
    };
    m_cancelModel = [guard](const QString &key) {
        if (guard)
            guard->cancelBackground(key);
    };
    m_modelRequest = [guard](const VideoBuildContext &context, const QString &system, const QString &text,
                             const QList<QImage> &images, std::function<void(ModelReply)> done) {
        if (guard)
            guard->enqueueDetailed(system, text, images, OneShotVlmChannel::Priority::Background,
                           context.cancellationKey(), done);
        else
            done({{}, QStringLiteral("模型通道未初始化")});
    };
}

void VideoRAGBuildCoordinator::setUnitWorkerPool(UnitAnalysisWorkerPool* pool) {
    QPointer<UnitAnalysisWorkerPool> guard(pool);
    m_poolCapacity = pool ? pool->capacity() : 1;
    m_unitWatchdogTimeout = [guard](int id) { return guard ? guard->watchdogTimeoutMs(id) : 0; };
    m_cancelUnitRequest = [guard](const QString& id) { if (guard) guard->cancelRequest(id); };
    m_cancelUnitBuild = [guard](const QString& key) { if (guard) guard->cancelBuild(key); };
    m_releaseUnitWorker = [guard](int id, const QString& unit) { if (guard) guard->releaseUnit(id, unit); };
    m_unitModelRequest = [guard](const UnitAnalysisRequest& r, const QString& system, const QString& text,
                               const QList<QImage>& images, std::function<void(ModelReply)> done) {
        if (guard) guard->submit(r, system, text, images, std::move(done));
        else done({{}, QStringLiteral("单元模型工作池未初始化")});
    };
}
